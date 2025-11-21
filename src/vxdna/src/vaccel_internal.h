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

#include <stdint.h>
#include <stddef.h>
#include <threads.h>
#include <unordered_map>
#include <memory>

/* Forward declarations */
class vaccel;
struct vaccel_resource;
struct vaccel_context;
struct vaccel_fence;

/**
 * @brief GPU resource (buffer object) structure
 *
 * Represents a GPU buffer that can be accessed by rendering commands.
 * Resources can be exported as DMA-BUF file descriptors for sharing.
 */
struct vaccel_resource {
    uint32_t res_id;           /**< Resource ID (unique per device) */
    uint64_t size;             /**< Resource size in bytes */
    uint32_t flags;            /**< Resource creation flags */
    int fd;                    /**< DMA-BUF FD or -1 if not exported */
    uint32_t bo_handle;        /**< libdrm buffer object handle */
    void *map_addr;            /**< Mapped address (NULL if not mapped) */
};

/**
 * @brief Rendering context structure
 *
 * Represents an independent command stream for GPU operations.
 * Each context maintains its own command queue and fence timeline.
 */
struct vaccel_context {
    uint32_t ctx_id;           /**< Context ID (unique per device) */
    char *name;                /**< Context name (optional, can be NULL) */
    struct vaccel *device;     /**< Parent device pointer */
    uint32_t hw_ctx_handle;    /**< Hardware context handle */
    uint64_t last_fence_id;    /**< Last submitted fence ID */
    mtx_t lock;                /**< Context lock for thread safety */
};

/**
 * @brief Fence synchronization structure
 *
 * Represents a synchronization point in the GPU timeline.
 * Fences can be waited on or exported as sync file descriptors.
 */
struct vaccel_fence {
    uint64_t id;               /**< Fence ID (64-bit timeline value) */
    int fd;                    /**< Sync file FD or -1 */
    uint32_t ring_idx;         /**< Timeline/ring index */
    struct timespec timestamp; /**< Creation time for hung detection */
};

/**
 * @brief Device instance class
 *
 * Represents a single device instance with its own resource, context,
 * and fence tables. Multiple devices can coexist independently.
 */
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
    vaccel(void *cookie, uint32_t capset_id, const struct vaccel_callbacks *callbacks);

    /**
     * @brief Destructor
     *
     * Cleans up all resources, contexts, and fences.
     */
    ~vaccel();

    // Disable copy and move
    vaccel(const vaccel&) = delete;
    vaccel& operator=(const vaccel&) = delete;
    vaccel(vaccel&&) = delete;
    vaccel& operator=(vaccel&&) = delete;

    // Public members for C API compatibility
    void *cookie;              /**< Device cookie (e.g., DRM FD) */
    int drm_fd;                /**< Actual DRM file descriptor */
    uint32_t capset_id;        /**< Capability set ID */
    const struct vaccel_callbacks *callbacks; /**< User callbacks */
    void *device_ctx;          /**< Device-specific context (e.g., AMDXDNA context) */

    /**
     * @brief Process virtio GPU command buffer
     *
     * Callback to process command buffer for virtio GPU operations.
     *
     * @param ctx Context pointer (device-specific)
     * @param cmd_buf Readonly command buffer
     * @param buf_size Size of command buffer in bytes
     * @return 0 on success, negative errno on failure
     */
    int (*virtio_gpu_ccmd_process)(void *ctx, const void *cmd_buf, size_t buf_size);

    /** @name Lookup tables
     * C++ std::unordered_map storing shared_ptr for automatic reference counting
     * @{
     */
    std::unordered_map<uint32_t, std::shared_ptr<vaccel_resource>> resource_table;
    std::unordered_map<uint32_t, std::shared_ptr<vaccel_context>> context_table;
    std::unordered_map<uint64_t, std::shared_ptr<vaccel_fence>> fence_table;
    /** @} */

    /** @name Thread safety
     * Mutexes for protecting lookup tables
     * @{
     */
    mtx_t resource_lock;       /**< Resource table lock */
    mtx_t context_lock;        /**< Context table lock */
    mtx_t fence_lock;          /**< Fence table lock */
    /** @} */

    /** @name Statistics
     * Per-device usage counters
     * @{
     */
    uint64_t num_resources;    /**< Number of active resources */
    uint64_t num_contexts;     /**< Number of active contexts */
    uint64_t num_fences;       /**< Number of active fences */
    uint64_t num_ccmd_submissions; /**< Total command submissions */
      /** @} */
};

/**
 * @brief Look up a device by its cookie
 *
 * @param cookie Device cookie
 * @return Shared pointer to device if found, nullptr otherwise
 */
std::shared_ptr<vaccel> vaccel_lookup(void *cookie);

/**
 * @defgroup resource_table Resource Table Management
 * @brief Per-device resource lookup table
 * @{
 */

/**
 * @brief Initialize resource table for a device
 *
 * @param device Device instance
 * @return 0 on success, negative errno on failure
 */
int vaccel_resource_table_init(struct vaccel *device);

/**
 * @brief Cleanup resource table and free all resources
 *
 * @param device Device instance
 */
void vaccel_resource_table_cleanup(struct vaccel *device);

/**
 * @brief Look up a resource by ID
 *
 * @param device Device instance
 * @param res_id Resource ID
 * @return Shared pointer to resource if found, nullptr otherwise
 */
std::shared_ptr<vaccel_resource> vaccel_resource_lookup(vaccel *device, uint32_t res_id);

/**
 * @brief Add a resource to the table
 *
 * @param device Device instance
 * @param res Resource to add
 * @return 0 on success, negative errno on failure
 */
int vaccel_resource_add(struct vaccel *device, struct vaccel_resource *res);

/**
 * @brief Remove a resource from the table
 *
 * @param device Device instance
 * @param res_id Resource ID
 */
void vaccel_resource_remove(struct vaccel *device, uint32_t res_id);

/** @} */ /* end of resource_table */

/**
 * @defgroup context_table Context Table Management
 * @brief Per-device context lookup table
 * @{
 */

/**
 * @brief Initialize context table for a device
 *
 * @param device Device instance
 * @return 0 on success, negative errno on failure
 */
int vaccel_context_table_init(struct vaccel *device);

/**
 * @brief Cleanup context table and free all contexts
 *
 * @param device Device instance
 */
void vaccel_context_table_cleanup(struct vaccel *device);

/**
 * @brief Look up a context by ID
 *
 * @param device Device instance
 * @param ctx_id Context ID
 * @return Shared pointer to context if found, nullptr otherwise
 */
std::shared_ptr<vaccel_context> vaccel_context_lookup(vaccel *device, uint32_t ctx_id);

/**
 * @brief Add a context to the table
 *
 * @param device Device instance
 * @param ctx Context to add
 * @return 0 on success, negative errno on failure
 */
int vaccel_context_add(struct vaccel *device, struct vaccel_context *ctx);

/**
 * @brief Remove a context from the table
 *
 * @param device Device instance
 * @param ctx_id Context ID
 */
void vaccel_context_remove(struct vaccel *device, uint32_t ctx_id);

/** @} */ /* end of context_table */

/**
 * @defgroup fence_table Fence Table Management
 * @brief Per-device fence lookup table with 64-bit IDs
 * @{
 */

/**
 * @brief Initialize fence table for a device
 *
 * @param device Device instance
 * @return 0 on success, negative errno on failure
 */
int vaccel_fence_table_init(struct vaccel *device);

/**
 * @brief Cleanup fence table and free all fences
 *
 * @param device Device instance
 */
void vaccel_fence_table_cleanup(struct vaccel *device);

/**
 * @brief Look up a fence by ID
 *
 * @param device Device instance
 * @param fence_id Fence ID (64-bit)
 * @return Shared pointer to fence if found, nullptr otherwise
 */
std::shared_ptr<vaccel_fence> vaccel_fence_lookup(vaccel *device, uint64_t fence_id);

/**
 * @brief Add a fence to the table
 *
 * @param device Device instance
 * @param fence Fence to add
 * @return 0 on success, negative errno on failure
 */
int vaccel_fence_add(struct vaccel *device, struct vaccel_fence *fence);

/**
 * @brief Remove a fence from the table
 *
 * @param device Device instance
 * @param fence_id Fence ID
 */
void vaccel_fence_remove(struct vaccel *device, uint64_t fence_id);

/**
 * @brief Retire signaled fences and check specific fence status
 *
 * @param device Device instance
 * @param fence_id Fence ID to check
 * @return 0 if fence is retired, -EBUSY if still pending
 */
int vaccel_fence_retire(struct vaccel *device, uint64_t fence_id);

/** @} */ /* end of fence_table */

/**
 * @defgroup drm_backend DRM Backend
 * @brief DRM/KMS backend implementation
 * @{
 */

/**
 * @brief Create a DRM resource (buffer object)
 *
 * @param device Device instance
 * @param res_id Resource ID
 * @param size Resource size in bytes
 * @param flags Creation flags
 * @return 0 on success, negative errno on failure
 */
int vaccel_drm_resource_create(struct vaccel *device, uint32_t res_id,
                                uint64_t size, uint32_t flags);

/**
 * @brief Export DRM resource as DMA-BUF file descriptor
 *
 * @param device Device instance
 * @param res_id Resource ID
 * @param[out] fd Output file descriptor
 * @return 0 on success, negative errno on failure
 */
int vaccel_drm_resource_export_fd(struct vaccel *device, uint32_t res_id, int *fd);

/**
 * @brief Destroy a DRM resource
 *
 * @param device Device instance
 * @param res_id Resource ID
 */
void vaccel_drm_resource_destroy(struct vaccel *device, uint32_t res_id);

/**
 * @brief Create a DRM context
 *
 * @param device Device instance
 * @param ctx_id Context ID
 * @param name Context name (optional)
 * @return 0 on success, negative errno on failure
 */
int vaccel_drm_context_create(struct vaccel *device, uint32_t ctx_id, const char *name);

/**
 * @brief Destroy a DRM context
 *
 * @param device Device instance
 * @param ctx_id Context ID
 */
void vaccel_drm_context_destroy(struct vaccel *device, uint32_t ctx_id);

/**
 * @brief Submit command buffer to DRM
 *
 * @param device Device instance
 * @param ctx_id Context ID
 * @param buffer Command buffer
 * @param size Buffer size in bytes
 * @return 0 on success, negative errno on failure
 */
int vaccel_drm_submit_ccmd(struct vaccel *device, uint32_t ctx_id,
                            const void *buffer, size_t size);

/**
 * @brief Submit fence to DRM for timeline synchronization
 *
 * @param device Device instance
 * @param ctx_id Context ID
 * @param fence_id Fence ID
 * @param ring_idx Timeline/ring index
 * @return 0 on success, negative errno on failure
 */
int vaccel_drm_submit_fence(struct vaccel *device, uint32_t ctx_id,
                             uint64_t fence_id, uint32_t ring_idx);

/** @} */ /* end of drm_backend */

/**
 * @defgroup amdxdna_device AMDXDNA Device
 * @brief AMDXDNA-specific device initialization and management
 * @{
 */

/**
 * @brief Initialize AMDXDNA device
 *
 * @param cookie Device cookie
 * @return Device context pointer on success, NULL on failure
 */
void *vxdna_device_init(void *cookie);

/**
 * @brief Cleanup AMDXDNA device context
 *
 * @param ctx Device context pointer
 */
void vxdna_device_cleanup(void *ctx);

/**
 * @brief Get device context from cookie
 *
 * @param cookie Device cookie
 * @return Device context pointer, or NULL if not found
 */
void *vxdna_device_get_ctx(void *cookie);

/**
 * @brief Process virtio GPU command buffer
 *
 * @param cookie Device cookie
 * @param cmd_buf Readonly command buffer
 * @param buf_size Size of command buffer in bytes
 * @return 0 on success, negative errno on failure
 */
int vxdna_device_process_ccmd(void *cookie, const void *cmd_buf, size_t buf_size);

/** @} */ /* end of amdxdna_device */

#endif /* VACCEL_INTERNAL_H */
