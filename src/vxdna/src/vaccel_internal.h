/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file vaccel_internal.h
 * @brief Internal API and data structures for vaccel renderer
 *
 * This header defines the internal structures and APIs used by the
 * vaccel renderer implementation. Not part of the public API.
 */

#ifndef VACCEL_INTERNAL_H
#define VACCEL_INTERNAL_H

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <thread>
#ifdef __unix__
#include <unistd.h>
#endif
#include <vector>

#include "vaccel_error.h"
#include "../include/vaccel_renderer.h"

/**
 * @brief Thread-safe wrapper around std::unordered_map
 *
 * vaccel_map provides synchronized access to an unordered_map using std::mutex.
 * Supports lookup, insertion (by value, lvalue ref, and rvalue), remove, and clear.
 */
template<typename Key, typename Value>
class vaccel_map {
public:
    vaccel_map() = default;

    // Disable copy and move
    vaccel_map(const vaccel_map&) = delete;
    vaccel_map& operator=(const vaccel_map&) = delete;

    // Lookup: returns a shared pointer to the value if present, nullptr otherwise
    Value lookup(const Key& key) const {
        std::lock_guard<std::mutex> lock(_mtx);
        auto it = _map.find(key);
        if (it != _map.end())
            return it->second;
        return Value();
    }

    // Insert by const reference
    bool insert(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(_mtx);
        return _map.emplace(key, value).second;
    }

    // Insert by rvalue (move value in)
    bool insert(const Key& key, Value&& value) {
        std::lock_guard<std::mutex> lock(_mtx);
        return _map.emplace(key, std::move(value)).second;
    }

    // Insert with rvalue key and value
    bool insert(Key&& key, Value&& value) {
        std::lock_guard<std::mutex> lock(_mtx);
        return _map.emplace(std::move(key), std::move(value)).second;
    }

    // Remove element by key
    bool erase(const Key& key) {
        std::lock_guard<std::mutex> lock(_mtx);
        return _map.erase(key) > 0;
    }

    // Clear all elements
    void clear() {
        std::lock_guard<std::mutex> lock(_mtx);
        _map.clear();
    }

    // Check if map contains key
    bool contains(const Key& key) {
        std::lock_guard<std::mutex> lock(_mtx);
        return _map.find(key) != _map.end();
    }

    // Optional: Map size (thread-safe)
    size_t size() const {
        std::lock_guard<std::mutex> lock(_mtx);
        return _map.size();
    }

private:
    std::unordered_map<Key, Value> _map;
    mutable std::mutex _mtx;
};

/**
 * @brief GPU resource (buffer object) structure
 *
 * Represents a GPU buffer that can be accessed by rendering commands.
 * Resources can be exported as DMA-BUF file descriptors for sharing.
 */
class vaccel_resource {
public:
    vaccel_resource(uint32_t res_id_in, uint64_t size_in, uint32_t flags_in,
        const struct vaccel_iovec *iovecs_in, uint32_t num_iovecs_in, uint32_t ctx_id_in)
        : res_id(res_id_in)
        , size(size_in)
        , flags(flags_in)
        , map_addr(NULL)
        , map_info(0)
        , iovecs(iovecs_in)
        , num_iovecs(num_iovecs_in)
        , ctx_id(ctx_id_in)
        , opaque_handle(0)

    {}

    vaccel_resource(uint32_t res_id_in, uint64_t size_in, int opaque_handle_in, uint32_t ctx_id_in)
    : res_id(res_id_in)
    , size(size_in)
    , flags(0)
    , map_addr(NULL)
    , map_info(0)
    , iovecs(NULL)
    , num_iovecs(0)
    , ctx_id(ctx_id_in)
    , opaque_handle(opaque_handle_in)
    {}

    ~vaccel_resource()
    {
        munmap();
    }

    uint32_t
    get_res_id() const noexcept
    {
        return res_id;
    }

    uint64_t
    get_size() const noexcept
    {
        return size;
    }

    uint32_t
    get_flags() const noexcept
    {
        return flags;
    }

    void *
    get_map_addr() const noexcept
    {
        return map_addr;
    }

    uint32_t
    write(uint32_t offset, const void *buf, uint32_t size)
    {
        uint32_t bytes_written = 0;

        for (uint32_t i = 0; i < num_iovecs; i++) {
            if (offset >= iovecs[i].iov_len) {
                offset -= iovecs[i].iov_len;
                continue;
            }

            uint32_t len = size;
            if (len > iovecs[i].iov_len - offset)
                 len = iovecs[i].iov_len - offset;
            void *dst = static_cast<void *>(static_cast<uint8_t *>(iovecs[i].iov_base) + offset);
            std::memcpy(dst, buf, len);

            buf = static_cast<const void *>(static_cast<const uint8_t *>(buf) + len);
            size -= len;
            bytes_written += len;
            offset = 0;
        }

        if (size > 0)
            VACCEL_THROW_MSG(-EINVAL, "buffer to res is too big, %u bytes remaining, %u bytes written",
                             size, bytes_written);

        return bytes_written;
    }

    uint32_t
    read(uint32_t offset, void *buf, uint32_t size)
    {
        uint32_t bytes_read = 0;
        for (uint32_t i = 0; i < num_iovecs; i++) {
            if (offset >= iovecs[i].iov_len) {
                offset -= iovecs[i].iov_len;
                continue;
            }
            uint32_t len = size;
            if (len > iovecs[i].iov_len - offset)
                 len = iovecs[i].iov_len - offset;
            void *src = static_cast<void *>(static_cast<uint8_t *>(iovecs[i].iov_base) + offset);
            std::memcpy(buf, src, len);

            buf = static_cast<void *>(static_cast<uint8_t *>(buf) + len);
            size -= len;
            bytes_read += len;
            offset = 0;
        }

        if (size > 0)
            VACCEL_THROW_MSG(-EINVAL, "buffer from res is too big, %u bytes remaining, %u bytes read",
                             size, bytes_read);

        return bytes_read;
    }

    uint32_t
    get_iovecs(const struct vaccel_iovec **iovecs_out) const noexcept
    {
        *iovecs_out = iovecs;
        return num_iovecs;
    }

    uint32_t
    get_ctx_id() const noexcept
    {
        return ctx_id;
    }

    int
    get_opaque_handle() const noexcept
    {
        return opaque_handle;
    }

    uint32_t
    get_map_info() const noexcept
    {
        return map_info;
    }

    void *
    mmap(int fd);

    void
    munmap()
    {
        vxdna_dbg("vaccel_resource::munmap: line %d, ctx_id=%u, res_id=%u, opaque_handle=%d, map_addr=%p",
                  __LINE__, ctx_id, res_id, opaque_handle, map_addr);
        if (map_addr)
            ::munmap(map_addr, size);
        map_addr = NULL;
    }
private:
    uint32_t res_id;           /**< Resource ID (unique per device) */
    uint64_t size;             /**< Resource size in bytes */
    uint32_t flags;            /**< Resource creation flags */
    void *map_addr;            /**< Mapped address (NULL if not mapped) */
    uint32_t map_info;         /**< Map information */
    const struct vaccel_iovec *iovecs; /**< IO vectors */
    uint32_t num_iovecs; /**< Number of IO vectors */
    uint32_t ctx_id; /**< Context ID */
    int opaque_handle; /**< Opaque handle */
};

/**
 * @brief Rendering context structure
 *
 * Represents an independent command stream for GPU operations.
 * Each context maintains its own command queue and fence timeline.
 */
template <typename ContextType>
class vaccel_context {
public:
    vaccel_context(uint32_t ctx_id_in, int fd_in, uint32_t ccmd_align_in)
        : ctx_id(ctx_id_in)
        , fd(fd_in)
        , ccmd_align(ccmd_align_in)
    {}

    int
    get_fd() const noexcept
    {
        return fd;
    }

    uint32_t
    get_id() const noexcept
    {
        return ctx_id;
    }

    uint32_t
    get_ccmd_align() const noexcept
    {
        return ccmd_align;
    }

    int
    get_blob(const struct vaccel_create_resource_blob_args *args)
    {
        return static_cast<ContextType*>(this)->get_blob(args);
    }

private:
    uint32_t ctx_id;           /**< Context ID (unique per device) */
    int fd;                    /**< Context file descriptor */
    uint32_t ccmd_align;       /**< Command buffer alignment */
    std::mutex lock;           /**< Context lock for thread safety */
};

/**
 * @brief Fence synchronization structure
 *
 * Represents a synchronization point in the GPU timeline.
 * Fences can be waited on or exported as sync file descriptors.
 */
class vaccel_fence {
public:
    vaccel_fence(uint64_t id_in, uint64_t sync_point_in,
                 uint32_t syncobj_handle_in, uint32_t ring_idx_in,
                 int64_t timeout_nsec_in)
        : id(id_in)
        , sync_point(sync_point_in)
        , syncobj_handle(syncobj_handle_in)
        , ring_idx(ring_idx_in)
        , timeout_nsec(timeout_nsec_in)
    {}

    uint64_t
    get_sync_point() const noexcept
    {
        return sync_point;
    }

    uint32_t
    get_syncobj_handle() const noexcept
    {
        return syncobj_handle;
    }

    uint32_t
    get_ring_idx() const noexcept
    {
        return ring_idx;
    }

    uint64_t
    get_id() const noexcept
    {
        return id;
    }

    int64_t
    get_timeout_nsec() const noexcept
    {
        return timeout_nsec;
    }
private:
    uint64_t id;               /**< Fence ID (64-bit timeline value) */
    uint64_t sync_point;       /**< Sync point */
    uint32_t syncobj_handle;   /**< Syncobj handle */
    uint32_t ring_idx;         /**< Timeline/ring index */
    int64_t timeout_nsec;      /**< Timeout in nanoseconds */
};

template <typename M>
void vaccel_map_cleanup(M &map, std::mutex &lock) 
{
    std::lock_guard<std::mutex> guard(lock);
    map.clear();
}

/**
 * @brief Device instance class
 *
 * Represents a single device instance with its own resource, context,
 * and fence tables. Multiple devices can coexist independently.
 */
template <typename T, typename ContextType>
class vaccel {
public:
    /**
     * @brief Constructor
     *
     * Initializes a new vaccel device instance.
     *
     * @param cookie Device cookie (e.g., DRM FD)
     * @param capset_id Capability set ID
     * @param callbacks User callbacks
     */
    vaccel(void *cookie, uint32_t capset_id, const struct vaccel_callbacks *callbacks)
        : cookie(cookie)
        , drm_fd(-1)
        , capset_id(capset_id)
        , callbacks(callbacks)
    {}

    /**
     * @brief Destructor
     *
     * Cleans up all resources, contexts, and fences.
     */
    ~vaccel()
    {
        /* Close DRM FD if needed */
        if (drm_fd >= 0) {
            close(drm_fd);
        }
    }

    // Disable copy and move (contains non-copyable/non-movable members)
    vaccel(const vaccel&) = delete;
    vaccel& operator=(const vaccel&) = delete;
    vaccel(vaccel&&) = delete;
    vaccel& operator=(vaccel&&) = delete;

    void
    get_capset_info(uint32_t *max_version, uint32_t *max_size)
    {
        static_cast<T*>(this)->get_capset_info(max_version, max_size);
    }

    void
    fill_capset(uint32_t capset_size, void *capset_buf) {
        static_cast<T*>(this)->fill_capset(capset_size, capset_buf);
    }

    std::shared_ptr<ContextType>
    get_ctx(uint32_t ctx_id)
    {
        return context_table.lookup(ctx_id);
    }

    void
    add_ctx(uint32_t ctx_id, std::shared_ptr<ContextType> &&ctx)
    {
       context_table.insert(ctx_id, std::move(ctx));
    }

    void
    remove_ctx(uint32_t ctx_id)
    {
        context_table.erase(ctx_id);
    }

    void
    create_ctx(uint32_t ctx_id, uint32_t ctx_flags, uint32_t nlen, const char *name)
    {
        auto ctx = get_ctx(ctx_id);
        if (ctx)
            VACCEL_THROW_MSG(-EEXIST, "Context already exists: ctx_id=%u", ctx_id);
        static_cast<T*>(this)->create_ctx(ctx_id, ctx_flags, nlen, name);
    }

    void
    destroy_ctx(uint32_t ctx_id)
    {
        auto ctx = get_ctx(ctx_id);
        if (!ctx)
            VACCEL_THROW_MSG(-ENOENT, "Context not found: ctx_id=%u", ctx_id);
        static_cast<T*>(this)->destroy_ctx(ctx_id);
    }

    std::shared_ptr<vaccel_resource>
    get_resource(uint32_t res_id) const
    {
        return resource_table.lookup(res_id);
    }

    void
    add_resource(uint32_t res_id, std::shared_ptr<vaccel_resource> &&res)
    {
        resource_table.insert(res_id, std::move(res));
    }

    void
    create_resource(const struct vaccel_create_resource_blob_args *args)
    {
        auto res = std::make_shared<vaccel_resource>(args->res_handle, args->size,
                                                     args->blob_flags, args->iovecs,
                                                     args->num_iovs, args->ctx_id);
        add_resource(args->res_handle, std::move(res));
    }

    void
    create_resource_from_blob(const struct vaccel_create_resource_blob_args *args)
    {
        static_cast<T*>(this)->create_resource_from_blob(args);
    }

    void
    destroy_resource(uint32_t res_id)
    {
        auto res = get_resource(res_id);
        if (!res)
            VACCEL_THROW_MSG(-ENOENT, "Resource not found: res_id=%u", res_id);
        static_cast<T*>(this)->destroy_resource(res);
        resource_table.erase(res_id);
    }

    int
    export_resource_fd(uint32_t res_id)
    {
        auto res = get_resource(res_id);
        if (!res)
            VACCEL_THROW_MSG(-ENOENT, "Resource not found: res_id=%u", res_id);
        if (res->get_opaque_handle() < 0)
            VACCEL_THROW_MSG(-EINVAL, "Resource is not opaque");
        auto ctx_id = res->get_ctx_id();
        auto ctx = get_ctx(ctx_id);
        if (!ctx)
            VACCEL_THROW_MSG(-ENOENT, "Context not found: ctx_id=%u", ctx_id);
        return static_cast<ContextType*>(ctx.get())->export_resource_fd(res);
    }

    int
    get_drm_fd() const noexcept
    {
        return callbacks->get_device_fd(get_cookie());
    }

    void
    set_drm_fd(int fd) noexcept
    {
        drm_fd = fd;
    }

    uint32_t
    get_capset_id() const noexcept
    {
        return capset_id;
    }

    void *
    get_cookie() const noexcept
    {
        return cookie;
    }

    std::shared_ptr<vaccel_fence>
    get_fence(uint32_t fence_id)
    {
        return fence_table.lookup(fence_id);
    }

    void add_fence(uint32_t fence_id, std::shared_ptr<vaccel_fence> &&fence)
    {
        fence_table.insert(fence_id, std::move(fence));
    }

    void remove_fence(uint32_t fence_id)
    {
        fence_table.erase(fence_id);
    }

    void submit_fence(uint32_t ctx_id, uint32_t flags, uint32_t ring_idx, uint64_t fence_id)
    {
        static_cast<T*>(this)->submit_fence(ctx_id, flags, ring_idx, fence_id); 
    }

    void
    destroy_fence(uint32_t fence_id)
    {
        static_cast<T*>(this)->destroy_fence(fence_id);
    }

    void
    dispatch_ccmd(std::shared_ptr<ContextType> &ctx, const struct vdrm_ccmd_req *hdr)
    {
        static_cast<T*>(this)->dispatch_ccmd(ctx, hdr);
    }

    const struct vaccel_callbacks *
    get_callbacks() const noexcept
    {
        return callbacks;
    }
private:
    // Public members for C API compatibility
    void *cookie;              /**< Device cookie (e.g., DRM FD) */
    int drm_fd;                /**< Actual DRM file descriptor */
    uint32_t capset_id;        /**< Capability set ID */
    const struct vaccel_callbacks *callbacks; /**< User callbacks */

    /** @name Lookup tables
     * C++ std::unordered_map storing shared_ptr for automatic reference counting
     * @{
     */
    vaccel_map<uint32_t, std::shared_ptr<vaccel_resource>> resource_table;
    vaccel_map<uint32_t, std::shared_ptr<ContextType>> context_table;
    vaccel_map<uint64_t, std::shared_ptr<vaccel_fence>> fence_table;
    /** @} */
};

#endif /* VACCEL_INTERNAL_H */
