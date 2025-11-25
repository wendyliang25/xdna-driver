/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * AMDXDNA Device Management
 * Provides device initialization, buffer object management, context handling,
 * command execution, and command dispatching for the AMDXDNA capset.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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

vxdna_bo::
vxdna_bo(int ctx_fd_in, const struct amdxdna_ccmd_create_bo_req *req)
{
    struct amdxdna_drm_get_bo_info bo_info = {};
    struct amdxdna_drm_create_bo args = {};
    int ret;

    ctx_fd = ctx_fd_in;
    bo_type = req->bo_type;
    size = req->size;
    map_size = 0;
    map_offset = 0;
    vaddr = AMDXDNA_INVALID_ADDR;
    xdna_addr = AMDXDNA_INVALID_ADDR;
    args.size = size;
    args.type = bo_type;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_CREATE_BO, &args);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Create bo failed ret %d", ret);

    bo_handle = args.handle;
    bo_info.handle = bo_handle;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &bo_info);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Get bo info failed ret %d", ret);

    map_offset = bo_info.map_offset;
    xdna_addr = bo_info.xdna_addr;
    vaddr = bo_info.vaddr;
    vxdna_dbg("Created bo: handle=%u, xdna_addr=%lu", bo_handle, xdna_addr);
}

vxdna_bo::
vxdna_bo(const std::shared_ptr<vaccel_resource> &res, int ctx_fd_in,
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

    // Use vector to avoid VLA and potential stack overflow
    size_t buf_size = sizeof(amdxdna_drm_va_tbl) + sizeof(amdxdna_drm_va_entry) * num_iovs;
    std::vector<uint8_t> buf_vec(buf_size);
    auto tbl = reinterpret_cast<amdxdna_drm_va_tbl*>(buf_vec.data());
    tbl->udma_fd = -1;
    tbl->num_entries = num_iovs;
    for (uint32_t i = 0; i < num_iovs; i++) {
        tbl->va_entries[i].vaddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(iovecs[i].iov_base));
        tbl->va_entries[i].len = static_cast<uint64_t>(iovecs[i].iov_len);
        map_size += tbl->va_entries[i].len;
    }
    args.vaddr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buf_vec.data()));
    args.size = size;
    args.type = bo_type;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_CREATE_BO, &args);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Create bo failed ret %d, errno %d, %s", ret, -errno, strerror(errno));

    bo_handle = args.handle;
    bo_info.handle = bo_handle;
    ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &bo_info);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Get bo info failed ret %d", ret);

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

    if (va == MAP_FAILED) {
        if (resv_va && resv_va != MAP_FAILED)
            munmap(resv_va, resv_size);
        VACCEL_THROW_MSG(-EFAULT, "Map bo failed");
    }
    vaddr = reinterpret_cast<uint64_t>(va);

    if (req->map_align && vaddr > resv_vaddr)
        munmap(resv_va, static_cast<size_t>(vaddr - resv_vaddr));
    if (resv_vaddr + resv_size > vaddr + map_size)
        munmap((void *)(vaddr + map_size),
               (size_t)(resv_vaddr + resv_size - vaddr - map_size));
    vxdna_dbg("%s, %u with resource: type=%u, res_id=%u, num_iovs=%u", __func__, __LINE__, req->bo_type, req->res_id, num_iovs);
}

vxdna_bo::
~vxdna_bo()
{
    vxdna_dbg("Destroying bo: handle=%u, vaddr=%lu, map_size=%lu", bo_handle, vaddr, map_size);
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

void
vxdna_context::vxdna_hwctx::
poll_and_retire_pending(std::vector<std::shared_ptr<vaccel_fence>> &&copy_pending_fences)
{
    for (auto &fence : copy_pending_fences) {
        uint64_t sync_point = fence->get_sync_point();
        drm_syncobj_timeline_wait arg = {};
        arg.handles = reinterpret_cast<uintptr_t>(&syncobj_handle);
        arg.points = reinterpret_cast<uintptr_t>(&sync_point);
        arg.timeout_nsec = fence->get_timeout_nsec();
        arg.count_handles = 1;
        /* Keep waiting even if not submitted yet */
        arg.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
        auto ret = ioctl(ctx_fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &arg);
        if (ret)
            vxdna_err("vxdna_hwctx::poll_and_retire_pending: Wait for fence failed ret %d, errno %d, %s, expect timeout: %ld",
                      ret, errno, strerror(errno), fence->get_timeout_nsec());
        // Fence is retired, write fence callback
        write_fence_callback(cookie, ctx_id, hwctx_handle, fence->get_id());
    }
}

vxdna_context::vxdna_hwctx::
vxdna_hwctx(const vxdna_context &ctx,
     const struct amdxdna_ccmd_create_ctx_req *req)
{
    hwctx_handle = AMDXDNA_INVALID_CTX_HANDLE;
    syncobj_handle = AMDXDNA_INVALID_FENCE_HANDLE;
    ctx_fd = ctx.get_fd();

    // Request XDNA driver to create a hardware context
    struct amdxdna_drm_create_hwctx args = {};
    args.max_opc = req->max_opc;
    args.num_tiles = req->num_tiles;
    args.mem_size = req->mem_size;
    args.qos_p = (uint64_t)&req->qos_info;

    int ret = ioctl(ctx.get_fd(), DRM_IOCTL_AMDXDNA_CREATE_HWCTX, &args);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Create hw context failed ret %d, errno %d, %s", ret, errno, strerror(errno));

    if (args.handle == AMDXDNA_INVALID_CTX_HANDLE)
        VACCEL_THROW_MSG(-EINVAL, "Create hw context failed, returns invalid hwctx handle");

    hwctx_handle = args.handle;
    syncobj_handle = args.syncobj_handle;
    write_fence_callback = ctx.get_callbacks()->write_context_fence;
    if (!write_fence_callback)
        VACCEL_THROW_MSG(-EINVAL, "Write fence callback not found");

    cookie = ctx.get_cookie();
    ctx_id = ctx.get_id();

    // Start polling thread to retire fences
    stop_polling.store(false, std::memory_order_relaxed);
    polling_thread = std::thread([this]() {
        while (!stop_polling.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lock(fences_lock);
            cv.wait(lock, [this] {
                return stop_polling.load(std::memory_order_relaxed) || !pending_fences.empty();
            });
            if (stop_polling.load(std::memory_order_relaxed))
                break;
            std::vector<std::shared_ptr<vaccel_fence>> tmp_pending_fences = std::move(pending_fences);
            pending_fences.clear();
            lock.unlock();
            poll_and_retire_pending(std::move(tmp_pending_fences));
        }
    });
}

vxdna_context::vxdna_hwctx::
~vxdna_hwctx()
{
    vxdna_dbg("HW context finishing: ctx_id=%u, hwctx_handle=%u", ctx_id, hwctx_handle);
    // Signal polling thread to stop
    stop_polling.store(true, std::memory_order_relaxed);
    cv.notify_all();
    
    // Wait for polling thread to finish
    if (polling_thread.joinable()) {
        polling_thread.join();
    }

    // Destroy sync object and hardware context
    if (syncobj_handle != AMDXDNA_INVALID_FENCE_HANDLE) {
        struct drm_syncobj_destroy arg = {};
        arg.handle = syncobj_handle;
        auto ret = ioctl(ctx_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &arg);
        if (ret)
            vxdna_err("Destroy sync object failed ret %d", ret);
        syncobj_handle = AMDXDNA_INVALID_FENCE_HANDLE;
    }
    // Destroy hardware context
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
vxdna_context::vxdna_hwctx::
config(const struct amdxdna_ccmd_config_ctx_req *req)
{
    struct amdxdna_drm_config_hwctx args = {};
    args.handle = hwctx_handle;
    args.param_type = req->param_type;
    args.param_val_size = req->param_val_size;
    if (req->param_val_size)
        args.param_val = (uint64_t)req->param_val;
    else
        args.param_val = req->inline_param;
    auto ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_CONFIG_HWCTX, &args);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Config hw context failed ret %d, errno %d, %s", ret, errno, strerror(errno));
}

uint64_t
vxdna_context::vxdna_hwctx::
exec_cmd(const struct amdxdna_ccmd_exec_cmd_req *req)
{
    struct amdxdna_drm_exec_cmd args = {};
    args.hwctx = hwctx_handle;
    args.type = req->type;
    args.cmd_count = req->cmd_count;
    if (req->cmd_count > 1)
        args.cmd_handles = (uint64_t)req->cmds_n_args;
    else
        args.cmd_handles = req->cmds_n_args[0];

    args.arg_count = req->arg_count;
    args.args = (uint64_t)(req->cmds_n_args + req->arg_offset);
    auto ret = ioctl(ctx_fd, DRM_IOCTL_AMDXDNA_EXEC_CMD, &args);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Exec cmd failed ret %d", ret);
    return args.seq;
}

void
vxdna_context::
vxdna_hwctx::
submit_fence(uint64_t fence_id)
{
    std::lock_guard<std::mutex> lock(fences_lock);
    if (!has_sync_point) {
        // Fence is not submitted yet, write fence callback directly
        write_fence_callback(cookie, ctx_id, hwctx_handle, fence_id);
        return;
    }
    auto fence = std::make_shared<vaccel_fence>(fence_id, sync_point, syncobj_handle, hwctx_handle, timeout_nsec);
    pending_fences.push_back(std::move(fence));
    has_sync_point = false;
    cv.notify_one();
}

void
vxdna_context::
create_bo(const vxdna &device, const struct amdxdna_ccmd_create_bo_req *req)
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
    vxdna_dbg("Created bo: handle=%u, xdna_addr=%lu", rsp.handle, rsp.xdna_addr);
}

void
vxdna_context::
add_bo(std::shared_ptr<vxdna_bo> &&bo)
{
    bo_table.insert(bo->get_handle(), std::move(bo));
}

void
vxdna_context::
remove_bo(uint32_t handle)
{
    vxdna_dbg("Removing bo: handle=%u", handle);
    bo_table.erase(handle);
}

void
vxdna_context::
create_hwctx(const struct amdxdna_ccmd_create_ctx_req *req)
{
    struct amdxdna_ccmd_create_ctx_rsp rsp = {};
    auto hwctx = std::make_shared<vxdna_hwctx>(*this, req);
    rsp.hdr.base.len = sizeof(rsp);
    rsp.handle = hwctx->get_handle();
    hwctx_table.insert(hwctx->get_handle(), std::move(hwctx));
    write_rsp(&rsp, sizeof(rsp), req->hdr.rsp_off);
}

void
vxdna_context::
remove_hwctx(uint32_t handle)
{
    hwctx_table.erase(handle);
}

void
vxdna_context::
config_hwctx(const struct amdxdna_ccmd_config_ctx_req *req)
{
    auto hwctx = hwctx_table.lookup(req->handle);
    if (!hwctx)
        VACCEL_THROW_MSG(-EINVAL, "HW context not found handle %u", req->handle);
    hwctx->config(req);
}

void
vxdna_context::
submit_fence(uint32_t ring_idx, uint64_t fence_id)
{
    if (ring_idx == AMDXDNA_INVALID_CTX_HANDLE) {
        // there is fence for commands doesn't belong to any hardware context
        // in this case, just write the fence callback directly
        // TODO: in future, if there are async commands not related to any hardware
        // context, we can add a default hardware context per device context for it.
        callbacks->write_context_fence(cookie, get_id(), ring_idx, fence_id);
        return;
    }
    auto hwctx = hwctx_table.lookup(ring_idx);
    if (!hwctx)
        VACCEL_THROW_MSG(-EINVAL, "HW context not found ring_idx %u", ring_idx);
    hwctx->submit_fence(fence_id);
}

void
vxdna_context::
exec_cmd(const struct amdxdna_ccmd_exec_cmd_req *req)
{
    auto hwctx = hwctx_table.lookup(req->ctx_handle);
    if (!hwctx)
        VACCEL_THROW_MSG(-EINVAL, "HW context not found handle %u", req->ctx_handle);
    struct amdxdna_ccmd_exec_cmd_rsp rsp = {};
    rsp.seq = hwctx->exec_cmd(req);
    rsp.hdr.base.len = sizeof(rsp); 
    write_rsp(&rsp, sizeof(rsp), req->hdr.rsp_off);
}

void
vxdna_context::
wait_cmd(const struct amdxdna_ccmd_wait_cmd_req *req)
{
    auto hwctx = hwctx_table.lookup(req->ctx_handle);
    if (!hwctx)
        VACCEL_THROW_MSG(-EINVAL, "HW context not found handle %u", req->ctx_handle);
    hwctx->set_sync_point(req->seq, req->timeout_nsec);
    write_err_rsp(0); // Success
}

void
vxdna_context::
get_info(const vxdna &device, const struct amdxdna_ccmd_get_info_req *req)
{
    auto res = device.get_resource(req->info_res);
    if (!res)
        VACCEL_THROW_MSG(-EINVAL, "%s, Did not find info resource, res_id %u", __func__, req->info_res);

    struct amdxdna_drm_get_array array_args = {};
    struct amdxdna_ccmd_get_info_rsp rsp = {};
    struct amdxdna_drm_get_info args = {};
    uint32_t info_size;
    unsigned long cmd;
    void *pargs;
    int ret;

    if (req->num_element) {
        // Check for integer overflow
        if (req->size > 0 && req->num_element > UINT32_MAX / req->size)
            VACCEL_THROW_MSG(-EINVAL, "Info size overflow: size=%u, num_element=%u", 
                             req->size, req->num_element);
        info_size = req->size * req->num_element;
        array_args.param = req->param;
        array_args.element_size = req->size;
        array_args.num_element = req->num_element;
        cmd = DRM_IOCTL_AMDXDNA_GET_ARRAY;
        pargs = &array_args;
    } else {
        info_size = req->size;
        args.param = req->param;
        args.buffer_size = req->size;
        cmd = DRM_IOCTL_AMDXDNA_GET_INFO;
        pargs = &args;
    }

    std::vector<uint8_t> info_buf(info_size);
    // Read argument data from resource
    res->read(0, info_buf.data(), info_size);
    if (req->num_element) {
        array_args.buffer = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info_buf.data()));
    } else {
        args.buffer = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info_buf.data()));
    }

    ret = ioctl(get_fd(), cmd, pargs);
    if (ret)
        VACCEL_THROW_MSG(-errno, "Get info failed ret %d, errno %d", ret, errno);

    if (cmd == DRM_IOCTL_AMDXDNA_GET_ARRAY) {
        rsp.num_element = array_args.num_element;
        rsp.size = array_args.element_size;
        info_size = array_args.element_size * array_args.num_element;
    } else {
        rsp.size = args.buffer_size;
        info_size = args.buffer_size;
    }

    res->write(0, info_buf.data(), info_size);
    rsp.hdr.base.len = sizeof(rsp);
    write_rsp(&rsp, sizeof(rsp), req->hdr.rsp_off);
}

void
vxdna_context::
read_sysfs(const struct amdxdna_ccmd_read_sysfs_req *req)
{
    struct amdxdna_ccmd_read_sysfs_rsp rsp = {};
    std::string path;
    struct stat st = {};
    int ret;

    ret = fstat(get_fd(), &st);
    if (ret)
        VACCEL_THROW_MSG(-errno, "fstat failed ret %d, errno %d", ret, errno);

    std::ostringstream oss;
    oss << "/sys/dev/char/" << major(st.st_rdev) << ":" << minor(st.st_rdev) << "/device/" << req->node_name;
    path = oss.str();

    // Open the sysfs file in binary mode and read the full contents into a buffer.
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        VACCEL_THROW_MSG(-errno, "Open %s failed, errno %d", path.c_str(), errno);

    //Read all content into buffer
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    rsp.val_len = buffer.size();
    rsp.hdr.base.len = sizeof(rsp) + rsp.val_len;
    write_rsp(&rsp, sizeof(rsp), req->hdr.rsp_off);
    write_rsp(buffer.data(), buffer.size(), req->hdr.rsp_off + sizeof(rsp));
}

void
vxdna_context::
write_err_rsp(int err)
{
    auto resp_res = get_resp_res();
    if (!resp_res)
        VACCEL_THROW_MSG(-EINVAL, "Resp resource not found for context %u", get_id());
    struct amdxdna_ccmd_rsp rsp = {};
    rsp.ret = err;
    rsp.base.len = sizeof(rsp);
    resp_res->write(0, &rsp, sizeof(rsp));
}

void
vxdna_context::
write_rsp(const void *rsp, size_t rsp_size, uint32_t rsp_off)
{
    auto resp_res = get_resp_res();
    if (!resp_res)
        VACCEL_THROW_MSG(-EINVAL, "Resp resource not found for context %u", get_id());
    resp_res->write(rsp_off, rsp, rsp_size);
}

void
vxdna::
get_capset_info(uint32_t *max_version, uint32_t *max_size)
{
    /* Return max version if requested */
    if (max_version)
        *max_version = vxdna::capset.version_major;

    /* Return max size if requested */
    if (max_size)
        *max_size = sizeof(vxdna::capset);
}

void
vxdna::
fill_capset(uint32_t capset_size, void *capset_buf)
{
    if (capset_size < sizeof(vxdna::capset))
        VACCEL_THROW_MSG(-EINVAL, "Provided capset_size (%u) is smaller than expected (%zu)",
                         capset_size, sizeof(vxdna::capset));

    /* Copy the capset structure to user buffer */
    memcpy(capset_buf, &vxdna::capset, sizeof(vxdna::capset));
    vxdna_dbg("Capset structure filled for capset_id=%u, version=%u",
               get_capset_id(), vxdna::capset.version_major);
}

void
vxdna::
create_ctx(uint32_t ctx_id, uint32_t ctx_flags, uint32_t nlen, const char *name)
{
    vxdna_dbg("Creating execution ctx: ctx_id=%u, flags=0x%x, nlen=%u, name=%s",
              ctx_id, ctx_flags, nlen, name ? name : "(null)");
    auto fd = get_drm_fd();
    auto ctx = std::make_shared<vxdna_context>(ctx_id, fd, 8, get_cookie(), get_callbacks());
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
vxdna::
destroy_ctx(uint32_t ctx_id)
{
    vxdna_dbg("Destroying execution ctx: ctx_id=%u", ctx_id);
    remove_ctx(ctx_id);
}

void
vxdna::
destroy_resource(const std::shared_ptr<vaccel_resource> &res)
{
    (void)res;
    // Required by vaccel<T>::destroy_resource
    // TODO: it is not required for now as guest xdna shim virtio
    // driver already ensure the sequence by creating the resource
    // blob and then create the BO and does in reverse order when destroying.
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

static void
vxdna_ccmd_config_ctx(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    (void)device;
    auto *req = static_cast<const struct amdxdna_ccmd_config_ctx_req *>(hdr);

    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->config_hwctx(req);
    });
}

static void
vxdna_ccmd_exec_cmd(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    (void)device;
    auto *req = static_cast<const struct amdxdna_ccmd_exec_cmd_req *>(hdr);

    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->exec_cmd(req);
    });
}

static void
vxdna_ccmd_wait_cmd(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    (void)device;
    auto *req = static_cast<const struct amdxdna_ccmd_wait_cmd_req *>(hdr);

    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->wait_cmd(req);
    });
}

static void
vxdna_ccmd_get_info(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    auto *req = static_cast<const struct amdxdna_ccmd_get_info_req *>(hdr);
    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->get_info(device, req);
    });
}

static void
vxdna_ccmd_read_sysfs(vxdna &device, const std::shared_ptr<vxdna_context>& ctx, const void *hdr)
{
    (void)device;
    auto *req = static_cast<const struct amdxdna_ccmd_read_sysfs_req *>(hdr);
    vxdna_ccmd_error_wrap(ctx, [&]() {
        ctx->read_sysfs(req);
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
    AMD_CCMD_DISPATCH_ENTRY(config_ctx),
    AMD_CCMD_DISPATCH_ENTRY(exec_cmd),
    AMD_CCMD_DISPATCH_ENTRY(wait_cmd),
    AMD_CCMD_DISPATCH_ENTRY(get_info),
    AMD_CCMD_DISPATCH_ENTRY(read_sysfs),
}};

void
vxdna::
dispatch_ccmd(std::shared_ptr<vxdna_context> &ctx, const struct vdrm_ccmd_req *hdr)
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
vxdna::
submit_fence(uint32_t ctx_id, uint32_t flags, uint32_t ring_idx, uint64_t fence_id)
{
    vxdna_dbg("Submitting fence: ctx_id=%u, flags=0x%x, ring_idx=%u, fence_id=%lu",
              ctx_id, flags, ring_idx, fence_id);
    auto ctx = get_ctx(ctx_id);
    if (!ctx)
        VACCEL_THROW_MSG(-EINVAL, "Context not found");
    ctx->submit_fence(ring_idx, fence_id);
}