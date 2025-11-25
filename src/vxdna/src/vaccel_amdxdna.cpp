/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * AMDXDNA Device Initialization
 * Handles device-specific initialization for AMDXDNA capset
 */

 #include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <vector>
#include <type_traits>

#include <drm/drm.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "drm_local/amdxdna_accel.h"
#include "amdxdna_proto.h"

#include "vaccel_error.h"
#include "../include/vaccel_renderer.h"
#include "vaccel_amdxdna.h"
#include "../util/vxdna_debug.h"

vxdna_bo::vxdna_bo(int ctx_fd_in, const struct amdxdna_ccmd_create_bo_req *req)
{
    struct amdxdna_drm_get_bo_info bo_info = {};
    struct amdxdna_drm_create_bo args = {};
    int ret;

    ctx_fd = ctx_fd_in;
    bo_type = req->bo_type;
    size = req->size;
    map_size = 0;
    args.size = size;
    args.type = bo_type;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_CREATE_BO, &args);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Create bo failed ret %d", ret);

    bo_handle = args.handle;
    bo_info.handle = bo_handle;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &bo_info);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Get bo info faild ret %d", ret);

    map_offset = bo_info.map_offset;
    xdna_addr = bo_info.xdna_addr;
    vaddr = bo_info.vaddr;
}

vxdna_bo::vxdna_bo(const std::shared_ptr<vaccel_resource> &res, int ctx_fd_in,
                   const struct amdxdna_ccmd_create_bo_req *req)
{
    struct amdxdna_drm_get_bo_info bo_info = {};
    struct amdxdna_drm_create_bo args = {};
    int ret;

    ctx_fd = ctx_fd_in;
    bo_type = req->bo_type;
    size = req->size;
    map_size = 0;
    const struct vaccel_iovec *iovecs;
    auto num_iovs = res->get_iovecs(&iovecs);
    alignas(amdxdna_drm_va_tbl)
    char buf[sizeof(amdxdna_drm_va_tbl) + sizeof(amdxdna_drm_va_entry) * num_iovs];
    auto tbl = reinterpret_cast<amdxdna_drm_va_tbl*>(buf);
    tbl->udma_fd = -1;
    tbl->num_entries = num_iovs;
    for (uint32_t i = 0; i < num_iovs; i++) {
        tbl->va_entries[i].vaddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(iovecs[i].iov_base));
        tbl->va_entries[i].len = static_cast<uint64_t>(iovecs[i].iov_len);
        map_size += tbl->va_entries[i].len;
    }
    args.vaddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buf));
    args.size = size;
    args.type = bo_type;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_CREATE_BO, &args);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Create bo failed ret %d", ret);

    bo_handle = args.handle;
    bo_info.handle = bo_handle;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &bo_info);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Get bo info faild ret %d", ret);

    map_offset = bo_info.map_offset;
    xdna_addr = bo_info.xdna_addr;
    vaddr = bo_info.vaddr;

    // mmap is required for non-dev BOs
    uint64_t resv_vaddr = 0, resv_size = 0, va_to_map = 0;
    void *resv_va = nullptr;
    int flags = MAP_SHARED | MAP_LOCKED;
    if (req->map_align) {
        resv_va = ::mmap(0, map_size + req->map_align, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (resv_va == MAP_FAILED)
            VACCEL_THROW_MSG(-ENOMEM, "Reserve vaddr range failed, map_align=%zu", req->map_align);

        resv_size = map_size + req->map_align;
        resv_vaddr = reinterpret_cast<uint64_t>(resv_va);
        va_to_map = ((uint64_t)resv_vaddr + req->map_align - 1) & (~((uint64_t)req->map_align - 1));
        flags |= MAP_FIXED;
    }

    void *va = ::mmap(reinterpret_cast<void *>(va_to_map), map_size, PROT_READ | PROT_WRITE,
                           flags, ctx_fd, map_offset);
    if (va == MAP_FAILED)
        VACCEL_THROW_MSG(-EFAULT, "Map bo failed");

    vaddr = reinterpret_cast<uint64_t>(va);
    if (vaddr > resv_vaddr)
        munmap(resv_va, static_cast<size_t>(vaddr - resv_vaddr));

    if (resv_vaddr + resv_size > vaddr + map_size) {
        munmap((void *)(vaddr + map_size),
               (size_t)(resv_vaddr + resv_size - vaddr - map_size));
    }
}

vxdna_bo::~vxdna_bo()
{
    if (vaddr != AMDXDNA_INVALID_ADDR)
        munmap(reinterpret_cast<void *>(vaddr), static_cast<size_t>(map_size));
    if (bo_handle != AMDXDNA_INVALID_BO_HANDLE) {
        struct drm_gem_close arg = {};
        arg.handle = bo_handle;
        auto ret = ioctl(ctx_fd, DRM_IOCTL_GEM_CLOSE, &arg);
        if (ret)
            vxdna_err("Close vxdna bo failed ret %d", ret);
    }
}

int
vxdna_context::vxdna_hwctx::init(const vxdna_context &ctx,
                                 const struct amdxdna_ccmd_create_ctx_req *req) noexcept
{
    hwctx_handle = AMDXDNA_INVALID_CTX_HANDLE;
    syncobj_handle = AMDXDNA_INVALID_FENCE_HANDLE;
    ctx_fd = ctx.get_fd();

    struct amdxdna_drm_create_hwctx args = {};
    args.max_opc = req->max_opc;
    args.num_tiles = req->num_tiles;
    args.mem_size = req->mem_size;
    args.qos_p = (uint64_t)&req->qos_info;

    int ret = ioctl(ctx.get_fd(), DRM_IOCTL_AMDXDNA_CREATE_HWCTX, &args);
    if (ret)
        return -errno;

    hwctx_handle = args.handle;
    syncobj_handle = args.syncobj_handle;

    return 0;
}

void
vxdna_context::vxdna_hwctx::finish() noexcept
{
    if (syncobj_handle != AMDXDNA_INVALID_FENCE_HANDLE) {
        struct drm_syncobj_destroy arg = {};
        arg.handle = syncobj_handle;
        auto ret = ioctl(ctx_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &arg);
        if (ret)
            vxdna_err("Destroy sync object failed ret %d", ret);
        syncobj_handle = AMDXDNA_INVALID_FENCE_HANDLE;
    }
    if (hwctx_handle != AMDXDNA_INVALID_CTX_HANDLE) {
        struct amdxdna_drm_destroy_hwctx arg = {};
        arg.handle = hwctx_handle;
        auto ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_DESTROY_HWCTX, &arg);
        if (ret)
            vxdna_err("Close hw context failed ret %d", ret);
        hwctx_handle = AMDXDNA_INVALID_CTX_HANDLE;
    }
}

void
vxdna_context::create_bo(const vxdna &device, const struct amdxdna_ccmd_create_bo_req *req)
{
    std::shared_ptr<vxdna_bo> xdna_bo;
    if (req->bo_type != AMDXDNA_BO_DEV) {
        auto res = device.get_resource(req->res_id);
        if (!res)
            VACCEL_THROW_MSG(-EINVAL, "Res: %u not found", req->res_id);
        xdna_bo = std::make_shared<vxdna_bo>(res, get_fd(), req);
    } else {
        xdna_bo = std::make_shared<vxdna_bo>(get_fd(), req);
    }

    struct amdxdna_ccmd_create_bo_rsp rsp = {};
    rsp.xdna_addr = xdna_bo->get_addr();
    rsp.handle = xdna_bo->get_handle();

    rsp.hdr.base.len = sizeof(rsp);
    auto resp_res = get_resp_res();
    if (!resp_res)
        VACCEL_THROW_MSG(-EINVAL, "Resp resource not found for context %u", get_id());
    (void)resp_res->write(req->hdr.rsp_off, &rsp, sizeof(rsp));
    add_bo(std::move(xdna_bo));
}

void
vxdna_context::add_bo(std::shared_ptr<vxdna_bo> &&bo)
{
    bo_table.insert(bo->get_handle(), std::move(bo));
}

void
vxdna_context::remove_bo(uint32_t handle)
{
    bo_table.erase(handle);
}

void
vxdna_context::create_hwctx(const struct amdxdna_ccmd_create_ctx_req *req)
{
    struct amdxdna_ccmd_create_ctx_rsp rsp = {};
    auto [hwctx_idx, hwctx] = hwctx_table.alloc();
    auto ret = hwctx->init(*this, req);
    if (ret) {
        hwctx_table.free(hwctx_idx);
        VACCEL_THROW_MSG(-errno, "Create hw context failed ret %d", ret);
    }

    rsp.hdr.base.len = sizeof(rsp);
    rsp.handle = hwctx_idx;
    write_rsp(&rsp, sizeof(rsp), req->hdr.rsp_off);
}

void
vxdna_context::remove_hwctx(uint32_t handle)
{
    auto hwctx = hwctx_table.get(handle);
    if (hwctx)
        hwctx->finish();
    hwctx_table.free(handle);
}

void
vxdna_context::write_err_rsp(int err)
{
    auto resp_res = get_resp_res();
    if (!resp_res)
        VACCEL_THROW_MSG(-EINVAL, "Resp resource not found for context %u", get_id());
    resp_res->write(0, &err, sizeof(err));
}

void
vxdna_context::write_rsp(const void *rsp, size_t rsp_size, uint32_t rsp_off)
{
    auto resp_res = get_resp_res();
    if (!resp_res)
        VACCEL_THROW_MSG(-EINVAL, "Resp resource not found for context %u", get_id());
    resp_res->write(rsp_off, rsp, rsp_size);
}

void
vxdna::get_capset_info(uint32_t *max_version, uint32_t *max_size)
{
    /* Return max version if requested */
    if (max_version)
        *max_version = vxdna::capset.max_version;

    /* Return max size if requested */
    if (max_size)
        *max_size = sizeof(vxdna::capset);
}

void
vxdna::fill_capset(uint32_t capset_size, void *capset_buf)
{
    vxdna_dbg("Filling capset for capset_id=%u, capset_version=%u, capset_size=%zu",
              get_capset_id(), vxdna::capset.max_version, sizeof(vxdna::capset));

    if (capset_size < sizeof(vxdna::capset))
        VACCEL_THROW_MSG(-EINVAL, "Provided capset_size (%u) is smaller than expected (%zu)",
                         capset_size, sizeof(vxdna::capset));

    /* Copy the capset structure to user buffer */
    memcpy(capset_buf, &vxdna::capset, sizeof(vxdna::capset));
    vxdna_info("Capset structure filled for capset_id=%u, version=%u",
               get_capset_id(), vxdna::capset.max_version);
}

void
vxdna::create_ctx(uint32_t ctx_id, uint32_t ctx_flags, uint32_t nlen, const char *name)
{
    vxdna_dbg("Creating execution ctx: ctx_id=%u, flags=0x%x, nlen=%u, name=%s",
              ctx_id, ctx_flags, nlen, name ? name : "(null)");
    auto fd = get_drm_fd();
    auto ctx = std::make_shared<vxdna_context>(ctx_id, fd, 8);
    if (name && nlen) {
        struct drm_set_client_name n = {
            .name_len = nlen,
            .name = (uint64_t) name,
        };
        int ret = ioctl(fd, DRM_IOCTL_SET_CLIENT_NAME, &n);
        if (ret < 0) {
            VACCEL_THROW_MSG(-errno, "Failed to set client name: ctx_id=%u", ctx_id);
        }
    }
    add_ctx(ctx_id, std::move(ctx));
}

void
vxdna::destroy_ctx(uint32_t ctx_id)
{
    remove_ctx(ctx_id);
    vxdna_dbg("Context destroyed: ctx_id=%u", ctx_id);
}

// Forward declarations of handler functions (to be implemented elsewhere)
static void
vxdna_ccmd_nop(vxdna &device, const std::shared_ptr<vxdna_context>& ctx,
                 const void *hdr)
{
    (void)device;
    (void)ctx;
    (void)hdr;
}

static void
vxdna_ccmd_init(vxdna &device, const std::shared_ptr<vxdna_context>& ctx,
                  const void *hdr)
{
    auto *req = static_cast<const struct amdxdna_ccmd_init_req *>(hdr);

    auto res = device.get_resource(req->rsp_res_id);
    if (!res)
        VACCEL_THROW_MSG(-EINVAL, "Resp resource not found");

    // Set the response resource for the context for the following ccmds to use
    ctx->set_resp_res(std::move(res));
}

static void
vxdna_ccmd_create_bo(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    auto *req = static_cast<const struct amdxdna_ccmd_create_bo_req *>(hdr);
    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->create_bo(device, req);
    });
}

static void
vxdna_ccmd_destroy_bo(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    (void)device;
    auto *req = static_cast<const struct amdxdna_ccmd_destroy_bo_req *>(hdr);

    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->remove_bo(req->handle);
    });
}

static void
vxdna_ccmd_create_ctx(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    (void)device;
    auto *req = static_cast<const struct amdxdna_ccmd_create_ctx_req *>(hdr);

    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->create_hwctx(req);
    });
}

static void
vxdna_ccmd_destroy_ctx(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    (void)device;
    auto *req = static_cast<const struct amdxdna_ccmd_destroy_ctx_req *>(hdr);

    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->remove_hwctx(req->handle);
    });
}
// Definition of the CCMD handler type for AMDXDNA
using amdxdna_ccmd_handler_t = void(*)(vxdna &device,
    const std::shared_ptr<vxdna_context>& ctx,
    const void *hdr);

// Structure describing each command handler entry
struct amdxdna_ccmd_dispatch_entry {
    const char *name;
    amdxdna_ccmd_handler_t handler;
    uint32_t size;
};

// Helper macro to calculate array size at compile-time
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

// Macro to statically define and initialize amdxdna_ccmd_dispatch_entry with a command name.
// The macro accepts the command name (without quotes) and expands to:
// { #name, amdxdna_ccmd_<name>, sizeof(struct amdxdna_ccmd_<name>_req) }
#define AMD_CCMD_DISPATCH_ENTRY(name) \
    { #name, vxdna_ccmd_##name, sizeof(struct amdxdna_ccmd_##name##_req) }


constexpr std::array<amdxdna_ccmd_dispatch_entry, 11> amdxdna_ccmd_dispatch_table = {{
    AMD_CCMD_DISPATCH_ENTRY(nop),
    AMD_CCMD_DISPATCH_ENTRY(init),
    AMD_CCMD_DISPATCH_ENTRY(create_bo),
    AMD_CCMD_DISPATCH_ENTRY(destroy_bo),
    AMD_CCMD_DISPATCH_ENTRY(create_ctx),
    AMD_CCMD_DISPATCH_ENTRY(destroy_ctx),
}};

void
vxdna::dispatch_ccmd(std::shared_ptr<vxdna_context> &ctx, const struct vdrm_ccmd_req *hdr)
{
    if (hdr->cmd > ARRAY_SIZE(amdxdna_ccmd_dispatch_table))
        VACCEL_THROW_MSG(-EINVAL, "invalid cmd: %u", hdr->cmd);

    const struct amdxdna_ccmd_dispatch_entry *ccmd = &amdxdna_ccmd_dispatch_table[hdr->cmd - 1];

    if (!ccmd->handler) {
        VACCEL_THROW_MSG(-EINVAL, "no handler: %u", hdr->cmd);
    }

    if (hdr->len < ccmd->size)
        VACCEL_THROW_MSG(-EINVAL, "request length is smaller than the expected size: %u < %u",
                         hdr->len, ccmd->size);

    vxdna_dbg("%s: hdr={cmd=%u, len=%u, seqno=%u, rsp_off=0x%x)", ccmd->name, hdr->cmd,
              hdr->len, hdr->seqno, hdr->rsp_off);

    /* copy request to let ccmd handler patch command in-place */
    size_t ccmd_size = std::max(ccmd->size, hdr->len);
    std::vector<uint8_t> buf(ccmd_size);
    memcpy(buf.data(), hdr, hdr->len);

    /* Request length from the guest can be smaller than the expected
     * size, ie. newer host and older guest, we need to zero initialize
     * the new fields at the end.
     */
    if (ccmd->size > hdr->len)
        memset(&buf[hdr->len], 0, ccmd->size - hdr->len);

    struct vdrm_ccmd_req *ccmd_hdr = reinterpret_cast<struct vdrm_ccmd_req *>(buf.data());
    ccmd->handler(*this, ctx, static_cast<const void *>(ccmd_hdr));
}

void
vxdna::create_fence(uint32_t ctx_id, uint32_t flags, uint32_t ring_idx, uint32_t *fence_id)
{
    vxdna_dbg("Creating fence: ctx_id=%u, flags=0x%x, ring_idx=%u", ctx_id, flags, ring_idx);
    (void)ctx_id;
    (void)flags;
    (void)ring_idx;
    (void)fence_id;
}

void
vxdna::destroy_fence(uint32_t fence_id)
{
    vxdna_dbg("Destroying fence: fence_id=%u", fence_id);
    (void)fence_id;
}