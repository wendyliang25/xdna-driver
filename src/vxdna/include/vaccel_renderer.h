/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file vaccel_renderer.h
 * @brief vaccel Renderer Public API
 *
 * Multi-device renderer with cookie-based device management, per-device
 * lookup tables for resources, contexts, and fences.
 */

#ifndef VACCEL_RENDERER_H
#define VACCEL_RENDERER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Virtio vaccel capset identifiers enumeration
 */
enum viraccel_capset_id {
    VIRACCEL_CAPSET_ID_AMDXDNA = 0, /**< AMD XDNA virtio capset identifier */
    VIRACCEL_CAPSET_ID_MAX = 1,    /**< Maximum supported capset identifier */
};

/**
 * @brief Virtio vaccel context type enumeration
 */
enum viraccel_context_type {
    VIRACCEL_CONTEXT_AMDXDNA = 0,  /**< AMD XDNA virtio context type */
    VIRACCEL_CONTEXT_MAX = 1,    /**< Maximum supported context type */
};

/**
 * @brief DRM capset structure
 *
 * Contains capability set information including version range
 * and context type.
 */
struct vaccel_drm_capset {
    uint32_t max_version;    /**< Maximum supported version */
    uint32_t min_version;    /**< Minimum supported version */
    uint32_t context_type;   /**< Context type identifier */
};

/**
 * @brief Callback functions structure
 *
 * User-provided callback functions for vaccel operations.
 * This allows customization of device access and other operations.
 */
struct vaccel_callbacks {
    /**
     * @brief Get device file descriptor from cookie
     *
     * Called to retrieve the actual device file descriptor associated
     * with a given cookie. This allows flexible cookie-to-FD mapping.
     *
     * @param cookie Device cookie
     * @param user_data Optional user data pointer (can be NULL)
     * @return Device file descriptor on success, negative errno on failure
     */
    int (*get_device_fd)(void *cookie, void *user_data);
};

/**
 * @brief Create a device with a given cookie
 *
 * Creates a new device instance identified by a unique cookie.
 * The cookie is typically a DRM file descriptor or device handle.
 *
 * @param cookie Unique device identifier (e.g., DRM FD, device handle)
 * @param capset_id Capability set ID (e.g., VIRACCEL_CAPSET_ID_AMDXDNA)
 * @param callbacks Optional callbacks structure (can be NULL for default behavior)
 * @return 0 on success, negative errno on failure
 * @retval 0 Success
 * @retval -EINVAL Invalid arguments or renderer not initialized
 * @retval -EEXIST Device with this cookie already exists
 * @retval -ENOMEM Out of memory
 */
int vaccel_create(void *cookie, uint32_t capset_id, const struct vaccel_callbacks *callbacks);

/**
 * @brief Destroy a device
 *
 * Destroys a device and all associated resources, contexts, and fences.
 *
 * @param cookie Device cookie
 */
void vaccel_destroy(void *cookie);

/**
 * @brief Create a context on a device
 *
 * Creates a rendering context for command submission.
 * Each context represents an independent command stream.
 *
 * @param cookie Device cookie
 * @param ctx_id Context ID (unique per device)
 * @param name Context name (optional, can be NULL)
 * @return 0 on success, negative errno on failure
 * @retval 0 Success
 * @retval -ENODEV Device not found
 * @retval -EEXIST Context with this ID already exists
 * @retval -ENOMEM Out of memory
 */
int vaccel_context_create(void *cookie, uint32_t ctx_id, const char *name);

/**
 * @brief Destroy a context
 *
 * Destroys a context and releases associated resources.
 *
 * @param cookie Device cookie
 * @param ctx_id Context ID
 */
void vaccel_context_destroy(void *cookie, uint32_t ctx_id);

/**
 * @brief Create a resource (GPU buffer/blob)
 *
 * Creates a GPU resource (buffer object) of the specified size.
 * Resources can be exported as DMA-BUF file descriptors.
 *
 * @param cookie Device cookie
 * @param res_id Resource ID (unique per device)
 * @param size Resource size in bytes
 * @param flags Resource creation flags (reserved, use 0)
 * @return 0 on success, negative errno on failure
 * @retval 0 Success
 * @retval -ENODEV Device not found
 * @retval -EEXIST Resource with this ID already exists
 * @retval -ENOMEM Out of memory or resource allocation failed
 */
int vaccel_resource_create(void *cookie, uint32_t res_id, uint64_t size, uint32_t flags);

/**
 * @brief Destroy a resource
 *
 * Destroys a GPU resource and releases associated memory.
 *
 * @param cookie Device cookie
 * @param res_id Resource ID
 */
void vaccel_resource_destroy(void *cookie, uint32_t res_id);

/**
 * @brief Export resource as DMA-BUF file descriptor
 *
 * Exports a resource as a DMA-BUF FD that can be shared with other
 * processes or imported by other drivers.
 *
 * @param cookie Device cookie
 * @param res_id Resource ID
 * @param[out] fd Output file descriptor (caller must close)
 * @return 0 on success, negative errno on failure
 * @retval 0 Success
 * @retval -ENODEV Device not found
 * @retval -ENOENT Resource not found
 * @retval -EINVAL Resource not exportable
 */
int vaccel_resource_export_fd(void *cookie, uint32_t res_id, int *fd);

/**
 * @brief Submit command buffer (CCMD)
 *
 * Submits a command buffer for execution on the GPU.
 * Commands are executed asynchronously in the specified context.
 *
 * @param cookie Device cookie
 * @param ctx_id Context ID
 * @param buffer Command buffer data
 * @param size Command buffer size in bytes
 * @return 0 on success, negative errno on failure
 * @retval 0 Success
 * @retval -ENODEV Device not found
 * @retval -ENOENT Context not found
 */
int vaccel_submit_ccmd(void *cookie, uint32_t ctx_id, const void *buffer, size_t size);

/**
 * @brief Submit fence for timeline synchronization
 *
 * Creates a fence point for GPU timeline synchronization.
 * Fences can be waited on or exported as sync file descriptors.
 *
 * @param cookie Device cookie
 * @param ctx_id Context ID
 * @param fence_id Fence ID (64-bit timeline value)
 * @param ring_idx Ring/timeline index (use 0 for default)
 * @return 0 on success, negative errno on failure
 * @retval 0 Success
 * @retval -ENODEV Device not found
 * @retval -ENOENT Context not found
 * @retval -EEXIST Fence with this ID already exists
 * @retval -ENOMEM Out of memory
 */
int vaccel_submit_fence(void *cookie, uint32_t ctx_id, uint64_t fence_id, uint32_t ring_idx);

/**
 * @brief Get sync file descriptor for a fence
 *
 * Returns a sync file descriptor for the specified fence.
 * The FD can be used with poll() or passed to other drivers.
 *
 * @param cookie Device cookie
 * @param fence_id Fence ID
 * @return Fence FD on success, -1 on failure
 * @note Caller must close the returned file descriptor
 */
int vaccel_get_fence_fd(void *cookie, uint64_t fence_id);

/**
 * @brief Get virtio vaccel capset information
 *
 * Retrieves capability set information for the specified capset ID.
 * This includes the maximum supported version and maximum size of the capset.
 *
 * @param cookie Device cookie
 * @param capset_id Capability set ID
 * @param[out] max_version Maximum supported capset version (can be NULL)
 * @param[out] max_size Maximum capset size in bytes (can be NULL)
 * @return 0 on success, negative errno on failure
 * @retval 0 Success
 * @retval -EINVAL Invalid arguments or renderer not initialized
 * @retval -ENODEV Device not found
 * @retval -ENOTSUP Operation not supported
 */
int vaccel_get_capset_info(void *cookie, uint32_t capset_id,
                            uint32_t *max_version, uint32_t *max_size);

/**
 * @brief Fill capset structure with capability set data
 *
 * Copies the full capability set structure into the provided buffer.
 * The buffer must be large enough to hold the complete capset data.
 *
 * @param cookie Device cookie
 * @param capset_id Capability set ID
 * @param capset_version Requested capset version (currently unused)
 * @param capset_size Size of the provided buffer in bytes
 * @param[out] capset_buf Buffer to receive capset data
 * @return 0 on success, negative errno on failure
 * @retval 0 Success - capset data copied to buffer
 * @retval -EINVAL Invalid arguments (NULL buffer or buffer too small)
 * @retval -ENODEV Device not found
 * @retval -ENOTSUP Unsupported capset ID
 */
int vaccel_fill_capset(void *cookie, uint32_t capset_id, uint32_t capset_version,
                        uint32_t capset_size, void *capset_buf);

#ifdef __cplusplus
}
#endif

#endif /* VACCEL_RENDERER_H */
