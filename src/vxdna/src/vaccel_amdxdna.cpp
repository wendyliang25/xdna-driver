/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * AMDXDNA Device Initialization
 * Handles device-specific initialization for AMDXDNA capset
 */

#include "vaccel_internal.h"
#include "../include/vaccel_renderer.h"
#include "../util/xvdna_debug.h"

#include <stdlib.h>
#include <errno.h>

/**
 * @brief AMDXDNA device context structure
 *
 * Holds device-specific data for AMDXDNA vaccel devices
 */
struct vxdna_device_ctx {
    void *cookie;              /**< Device cookie */
    int drm_fd;                /**< DRM file descriptor */
    uint32_t capset_id;        /**< Capability set ID */
    struct vaccel *device;     /**< Parent device pointer */
};

/**
 * @brief Initialize AMDXDNA device
 *
 * Creates and initializes device-specific context for AMDXDNA capset.
 *
 * @param cookie Device cookie
 * @return Device context pointer on success, NULL on failure
 */
void *
vxdna_device_init(void *cookie)
{
    auto device = vaccel_lookup(cookie);
    struct vxdna_device_ctx *ctx;

    xvdna_dbg("Initializing AMDXDNA device for cookie=%p", cookie);

    /* Lookup device by cookie */
    if (!device) {
        xvdna_err("Device not found for cookie %p", cookie);
        return NULL;
    }

    /* Validate capset ID */
    if (device->capset_id != VIRACCEL_CAPSET_ID_AMDXDNA) {
        xvdna_err("Invalid capset ID: %u (expected AMDXDNA=%u)",
                  device->capset_id, VIRACCEL_CAPSET_ID_AMDXDNA);
        return NULL;
    }

    /* Allocate device context */
    ctx = static_cast<struct vxdna_device_ctx*>(calloc(1, sizeof(*ctx)));
    if (!ctx) {
        xvdna_err("Failed to allocate device context");
        return NULL;
    }

    /* Initialize context */
    ctx->cookie = cookie;
    ctx->drm_fd = device->drm_fd;
    ctx->capset_id = device->capset_id;
    ctx->device = device.get();

    /* Store context in device */
    device->device_ctx = ctx;

    xvdna_info("AMDXDNA device initialized successfully: fd=%d, capset_id=%u",
               ctx->drm_fd, ctx->capset_id);

    return ctx;
}

/**
 * @brief Cleanup AMDXDNA device context
 *
 * Frees device-specific context resources.
 *
 * @param ctx Device context pointer
 */
void
vxdna_device_cleanup(void *ctx)
{
    struct vxdna_device_ctx *dev_ctx = (struct vxdna_device_ctx *)ctx;

    if (!dev_ctx)
        return;

    xvdna_dbg("Cleaning up AMDXDNA device context");

    free(dev_ctx);
}

/**
 * @brief Get device context from cookie
 *
 * Helper function to retrieve device context.
 *
 * @param cookie Device cookie
 * @return Device context pointer, or NULL if not found
 */
void *
vxdna_device_get_ctx(void *cookie)
{
    auto device = vaccel_lookup(cookie);
    if (!device) {
        xvdna_err("Device not found for cookie %p", cookie);
        return NULL;
    }

    return device->device_ctx;
}

/**
 * @brief Process virtio GPU command buffer
 *
 * Processes a command buffer using the registered callback.
 *
 * @param cookie Device cookie
 * @param cmd_buf Readonly command buffer
 * @param buf_size Size of command buffer in bytes
 * @return 0 on success, negative errno on failure
 */
int
vxdna_device_process_ccmd(void *cookie, const void *cmd_buf, size_t buf_size)
{
    auto device = vaccel_lookup(cookie);
    struct vxdna_device_ctx *ctx;

    xvdna_dbg("Processing command buffer: cookie=%p, size=%zu", cookie, buf_size);

    /* Validate inputs */
    if (!cmd_buf || buf_size == 0) {
        xvdna_err("Invalid command buffer: buf=%p, size=%zu", cmd_buf, buf_size);
        return -EINVAL;
    }

    /* Lookup device */
    if (!device) {
        xvdna_err("Device not found for cookie %p", cookie);
        return -ENODEV;
    }

    /* Get device context */
    ctx = static_cast<struct vxdna_device_ctx*>(device->device_ctx);
    if (!ctx) {
        xvdna_err("Device context not initialized");
        return -EINVAL;
    }

    /* Check if callback is registered */
    if (!device->virtio_gpu_ccmd_process) {
        xvdna_err("virtio_gpu_ccmd_process callback not registered");
        return -ENOTSUP;
    }

    /* Call the callback */
    xvdna_dbg("Calling virtio_gpu_ccmd_process callback");
    int ret = device->virtio_gpu_ccmd_process(ctx, cmd_buf, buf_size);

    if (ret) {
        xvdna_err("Command processing failed: %d", ret);
    } else {
        xvdna_dbg("Command processed successfully");
    }

    return ret;
}

