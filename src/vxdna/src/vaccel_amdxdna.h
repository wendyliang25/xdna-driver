/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file vaccel_amdxdna.h
 * @brief Internal API for AMDXDNA device management
 *
 * This header defines the internal AMDXDNA-specific functions used by
 * the vaccel renderer implementation. Not part of the public API.
 */

#ifndef VACCEL_AMDXDNA_H
#define VACCEL_AMDXDNA_H

#include <cstdint>
#include <stddef.h>
#include <stdexcept>
#include <memory>

#include "drm_hw.h" // from xdna shim virtio

#include "drm_local/amdxdna_accel.h"
#include "amdxdna_proto.h"
#include "vaccel_renderer.h"
#include "vaccel_internal.h"

class vxdna_bo {
    public:
        vxdna_bo(const std::shared_ptr<vaccel_resource> &res, int ctx_fd_in, const struct amdxdna_ccmd_create_bo_req *req);
        vxdna_bo(int ctx_fd_in, const struct amdxdna_ccmd_create_bo_req *req);
        ~vxdna_bo();
        vxdna_bo(const vxdna_bo&) = delete;
        vxdna_bo& operator=(const vxdna_bo&) = delete;
        vxdna_bo(vxdna_bo&&) = delete;
        vxdna_bo& operator=(vxdna_bo&&) = delete;

        uint64_t get_addr() const noexcept
        {
            if (xdna_addr != AMDXDNA_INVALID_ADDR)
                return xdna_addr;
            return vaddr;
        }
        uint32_t get_handle() const noexcept
        {
            return bo_handle;
        }
    private:
        uint64_t size;
        uint64_t vaddr;
        uint64_t map_offset;
        uint64_t xdna_addr;
        uint64_t map_size;
        uint32_t bo_handle;
        uint32_t bo_type;
        int opaque_handle;
        int ctx_fd;
    };


/**
 * @brief AMDXDNA context structure
 *
 * Represents an AMDXDNA context.
 */
class vxdna;
class vxdna_context : public vaccel_context<vxdna_context> {
public:
    vxdna_context(uint32_t ctx_id, int fd, uint32_t ccmd_align, void *cookie, const struct vaccel_callbacks *callbacks)
        : vaccel_context<vxdna_context>(ctx_id, fd, ccmd_align)
        , cookie(cookie)
        , callbacks(callbacks)
    { vxdna_dbg("Context created: ctx_id=%u, fd=%d", get_id(), get_fd()); }
    ~vxdna_context() {
        vxdna_dbg("Context destroying: ctx_id=%u, fd=%d", get_id(), get_fd());
        close(get_fd());
    };
    void set_resp_res(std::shared_ptr<vaccel_resource> &&res)
    {
        resp_res = std::move(res);
    }
    std::shared_ptr<vaccel_resource> get_resp_res() const noexcept
    {
        return resp_res;
    }
    int export_resource_fd(const std::shared_ptr<vaccel_resource> &res);

    void create_bo(const vxdna &device, const struct amdxdna_ccmd_create_bo_req *req);
    void add_bo(std::shared_ptr<vxdna_bo> &&bo);
    void remove_bo(uint32_t handle);
    void write_err_rsp(int err);
    void write_rsp(const void *rsp, size_t rsp_size, uint32_t rsp_off);
    void create_hwctx(const struct amdxdna_ccmd_create_ctx_req *req);
    void remove_hwctx(uint32_t handle);
    void config_hwctx(const struct amdxdna_ccmd_config_ctx_req *req);
    void exec_cmd(const struct amdxdna_ccmd_exec_cmd_req *req);
    void wait_cmd(const struct amdxdna_ccmd_wait_cmd_req *req);
    void get_info(const vxdna &device, const struct amdxdna_ccmd_get_info_req *req);
    void read_sysfs(const struct amdxdna_ccmd_read_sysfs_req *req);
    void submit_fence(uint32_t ring_idx, uint64_t fence_id);
    const struct vaccel_callbacks *get_callbacks() const noexcept
    {
        return callbacks;
    }
    void *get_cookie() const noexcept
    {
        return cookie;
    }
    int get_blob(const struct vaccel_create_resource_blob_args *args);
private:
    class vxdna_hwctx {
    public:
        vxdna_hwctx() = delete;
        vxdna_hwctx(const vxdna_hwctx&) = delete;
        vxdna_hwctx& operator=(const vxdna_hwctx&) = delete;
        vxdna_hwctx(vxdna_hwctx&&) = delete;
        vxdna_hwctx& operator=(vxdna_hwctx&&) = delete;
        vxdna_hwctx(const vxdna_context &ctx,
                    const struct amdxdna_ccmd_create_ctx_req *req);
        ~vxdna_hwctx();
        void
        config(const struct amdxdna_ccmd_config_ctx_req *req);
        uint64_t
        exec_cmd(const struct amdxdna_ccmd_exec_cmd_req *req);
        void set_sync_point(uint64_t sync_point_in, int64_t timeout_nsec_in) noexcept
        {
            std::lock_guard<std::mutex> lock(fences_lock);
            sync_point = sync_point_in;
            timeout_nsec = timeout_nsec_in;
            has_sync_point = true;
        }
        uint32_t get_handle() const noexcept
        {
            return hwctx_handle;
        }
        void submit_fence(uint64_t fence_id);
    private:
        void clear_pending();
        void poll_fences();
        void poll_and_retire_pending(std::vector<std::shared_ptr<vaccel_fence>> &&copy_pending_fences);
        void *cookie;
        void (*write_fence_callback)(void *cookie, uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id) = nullptr;
        mutable std::mutex fences_lock;
        uint64_t sync_point;
        int64_t timeout_nsec;
        bool has_sync_point;
        std::condition_variable cv;
        std::vector<std::shared_ptr<vaccel_fence>> pending_fences;
        std::thread polling_thread;
        std::atomic<bool> stop_polling;
        uint32_t hwctx_handle; // it is also ring_idx
        uint32_t syncobj_handle;
        uint32_t ctx_fd;
        uint32_t ctx_id;
    };
    void *cookie;
    const struct vaccel_callbacks *callbacks;
    std::shared_ptr<vaccel_resource> resp_res;
    vaccel_map<uint32_t, std::shared_ptr<vxdna_bo>> bo_table;
    vaccel_map<uint32_t, std::shared_ptr<vxdna_hwctx>> hwctx_table;
};

/**
 * @brief AMDXDNA device class
 *
 * Specialized device implementation inheriting from vaccel<T>.
 */
class vxdna : public vaccel<vxdna, vxdna_context>
{
public:
    vxdna(void *cookie, uint32_t capset_id, const struct vaccel_callbacks *callbacks)
        : vaccel<vxdna, vxdna_context>(cookie, capset_id, callbacks)
    {}
    ~vxdna() = default;
    vxdna(const vxdna&) = delete;
    vxdna& operator=(const vxdna&) = delete;
    vxdna(vxdna&&) = delete;
    vxdna& operator=(vxdna&&) = delete;

    // Example device-specific methods can be added here

    using vaccel<vxdna, vxdna_context>::destroy_resource;
    // Implement required interface for vaccel<T>
    void get_capset_info(uint32_t *max_version, uint32_t *max_size);
    void fill_capset(uint32_t capset_size, void *capset_buf);
    void create_ctx(uint32_t ctx_id, uint32_t ctx_flags, uint32_t nlen, const char *name);
    void destroy_ctx(uint32_t ctx_id);
    void create_resource_from_blob(const struct vaccel_create_resource_blob_args *args);
    void destroy_resource(const std::shared_ptr<vaccel_resource> &res);
    void context_submit_ccmd(const std::shared_ptr<vxdna_context> &ctx, const void *ccmd, uint32_t ccmd_size);
    void submit_fence(uint32_t ctx_id, uint32_t flags, uint32_t ring_idx, uint64_t fence_id);
    void dispatch_ccmd(std::shared_ptr<vxdna_context> &ctx, const struct vdrm_ccmd_req *hdr);
private:
    inline static constexpr struct vaccel_drm_capset capset = {
        .wire_format_version = 1,
        .version_major = 1,
        .version_minor = 0,
        .version_patchlevel = 0,
        .context_type = VIRTACCEL_DRM_CONTEXT_AMDXDNA,
    };
};


/**
 * @brief Get device context from cookie
 *
 * Helper function to retrieve the AMDXDNA device context for a given
 * device cookie.
 *
 * @param cookie Device cookie
 * @return Device context pointer (vxdna_device_ctx), or NULL if not found
 */
void *vxdna_device_get_ctx(void *cookie);

/**
 * @defgroup amdxdna_ccmd Command Processing
 * @brief Functions for processing virtio GPU command buffers
 * @{
 */

/**
 * @brief Process virtio GPU command buffer
 *
 * Processes a command buffer using the registered virtio_gpu_ccmd_process
 * callback. This function validates the device state and dispatches the
 * command to the appropriate handler.
 *
 * @param cookie Device cookie
 * @param cmd_buf Readonly command buffer
 * @param buf_size Size of command buffer in bytes
 * @return 0 on success, negative errno on failure
 * @retval -EINVAL Invalid command buffer or size
 * @retval -ENODEV Device not found
 * @retval -ENOTSUP Command callback not registered
 *
 * @note The command buffer must remain valid for the duration of this call.
 *       The function does not take ownership of the buffer.
 */
int vxdna_device_process_ccmd(void *cookie, const void *cmd_buf, size_t buf_size);

template<typename ContextType, typename F> void
vxdna_ccmd_error_wrap(const std::shared_ptr<ContextType> &ctx, F &&f)
{
    try {
        f();
    } catch (const vaccel_error& e) {
        ctx->write_err_rsp(e.code());
        throw e;
    } catch (const std::exception& e) {
        ctx->write_err_rsp(-EIO);
        throw e;
    } catch (...) {
        ctx->write_err_rsp(-EIO);
        throw std::runtime_error("Unknown exception");
    }
}

/** @} */ /* end of amdxdna_ccmd */


#endif /* VACCEL_AMDXDNA_H */

