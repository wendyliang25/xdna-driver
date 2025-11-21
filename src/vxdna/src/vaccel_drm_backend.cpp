/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * DRM Backend Implementation
 * Uses libdrm for CCMD processing and fence timeline synchronization
 */

#include "vaccel_internal.h"
#include "../util/os_file.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#ifdef __unix__
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <time.h>
#include <xf86drm.h>
#include <drm/drm.h>
#else
/* Windows platform - stub implementations */
#define DRM_IOCTL_MODE_CREATE_DUMB 0
#define DRM_IOCTL_PRIME_HANDLE_TO_FD 0
#define DRM_IOCTL_MODE_DESTROY_DUMB 0
#define DRM_IOCTL_ADD_CTX 0
#define DRM_IOCTL_RM_CTX 0
#define DRM_CLOEXEC 0
#define DRM_RDWR 0
#define EFD_CLOEXEC 0
#define EFD_NONBLOCK 0
static inline int close(int fd) { (void)fd; return 0; }
static inline int ioctl(int, unsigned long, ...) { return -1; }
static inline int eventfd(unsigned int, int) { return -1; }
static inline int write(int, const void*, size_t) { return -1; }
struct drm_mode_create_dumb { uint32_t height; uint32_t width; uint32_t bpp; uint32_t handle; uint32_t pitch; uint64_t size; };
struct drm_prime_handle { uint32_t handle; uint32_t flags; int fd; };
struct drm_mode_destroy_dumb { uint32_t handle; };
struct drm_ctx { uint32_t handle; };
#endif

/*
 * Resource Management
 */

int
vaccel_drm_resource_create(struct vaccel *device, uint32_t res_id,
                           uint64_t size, uint32_t flags)
{
    struct vaccel_resource *res;
    struct drm_mode_create_dumb create_dumb = {0};
    struct drm_prime_handle prime_handle = {0};
    int ret;

    res = static_cast<struct vaccel_resource*>(calloc(1, sizeof(*res)));
    if (!res)
        return -ENOMEM;

    res->res_id = res_id;
    res->size = size;
    res->flags = flags;
    res->fd = -1;
    res->map_addr = NULL;

    /* Create dumb buffer as example BO */
    create_dumb.width = (size + 4095) / 4096;
    create_dumb.height = 1;
    create_dumb.bpp = 32;

    ret = ioctl(device->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
    if (ret < 0) {
        ret = -errno;
        fprintf(stderr, "vaccel_drm: failed to create dumb buffer: %d\n", errno);
        free(res);
        return ret;
    }

    res->bo_handle = create_dumb.handle;

    /* Export as DMABUF */
    prime_handle.handle = res->bo_handle;
    prime_handle.flags = DRM_CLOEXEC | DRM_RDWR;
    prime_handle.fd = -1;

    ret = ioctl(device->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
    if (ret < 0) {
        fprintf(stderr, "vaccel_drm: failed to export dma-buf: %d\n", errno);
        /* Continue without FD export */
    } else {
        res->fd = prime_handle.fd;
    }

    /* Add to resource table */
    ret = vaccel_resource_add(device, res);
    if (ret) {
        if (res->fd >= 0)
            close(res->fd);

        struct drm_mode_destroy_dumb destroy_dumb = {
            .handle = res->bo_handle
        };
        ioctl(device->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
        free(res);
        return ret;
    }

    return 0;
}

void
vaccel_drm_resource_destroy(struct vaccel *device, uint32_t res_id)
{
    auto res = vaccel_resource_lookup(device, res_id);

    if (!res)
        return;

    /* Destroy dumb buffer */
    struct drm_mode_destroy_dumb destroy_dumb = {
        .handle = res->bo_handle
    };
    ioctl(device->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

    /* Remove from table (will call destructor) */
    vaccel_resource_remove(device, res_id);
}

int
vaccel_drm_resource_export_fd(struct vaccel *device, uint32_t res_id, int *fd)
{
    auto res = vaccel_resource_lookup(device, res_id);

    if (!res)
        return -ENOENT;

    if (res->fd < 0)
        return -EINVAL;

    *fd = os_dupfd_cloexec(res->fd);
    if (*fd < 0)
        return -errno;

    return 0;
}

/*
 * Context Management
 */

int
vaccel_drm_context_create(struct vaccel *device, uint32_t ctx_id, const char *name)
{
    struct vaccel_context *ctx;
    struct drm_ctx drm_ctx = {0};
    int ret;

    ctx = static_cast<struct vaccel_context*>(calloc(1, sizeof(*ctx)));
    if (!ctx)
        return -ENOMEM;

    ctx->ctx_id = ctx_id;
    ctx->device = device;
    ctx->last_fence_id = 0;

    if (name)
        ctx->name = strdup(name);

    mtx_init(&ctx->lock, mtx_plain);

    /* Create DRM context (legacy, may not be supported on all drivers) */
    drm_ctx.handle = ctx_id;
    ret = ioctl(device->drm_fd, DRM_IOCTL_ADD_CTX, &drm_ctx);
    if (ret < 0) {
        /* Context creation not supported, use dummy handle */
        ctx->hw_ctx_handle = ctx_id;
    } else {
        ctx->hw_ctx_handle = drm_ctx.handle;
    }

    /* Add to context table */
    ret = vaccel_context_add(device, ctx);
    if (ret) {
        mtx_destroy(&ctx->lock);
        free(ctx->name);
        free(ctx);
        return ret;
    }

    return 0;
}

void
vaccel_drm_context_destroy(struct vaccel *device, uint32_t ctx_id)
{
    auto ctx = vaccel_context_lookup(device, ctx_id);

    if (!ctx)
        return;

    /* Destroy DRM context */
    struct drm_ctx drm_ctx = {
        .handle = ctx->hw_ctx_handle
    };
    ioctl(device->drm_fd, DRM_IOCTL_RM_CTX, &drm_ctx);

    /* Remove from table (will call destructor) */
    vaccel_context_remove(device, ctx_id);
}

/*
 * Command Submission
 */

int
vaccel_drm_submit_ccmd(struct vaccel *device, uint32_t ctx_id,
                       const void *buffer, size_t size)
{
    auto ctx = vaccel_context_lookup(device, ctx_id);

    if (!ctx)
        return -ENOENT;

    /* TODO: Implement actual command submission using DRM
     * This would typically use driver-specific ioctls like:
     * - DRM_IOCTL_AMDGPU_CS (for AMDGPU)
     * - DRM_IOCTL_MSM_GEM_SUBMIT (for MSM)
     * - etc.
     *
     * For now, just acknowledge the submission
     */

    mtx_lock(&ctx->lock);
    device->num_ccmd_submissions++;
    mtx_unlock(&ctx->lock);

    return 0;
}

/*
 * Fence Submission and Timeline Sync
 */

int
vaccel_drm_submit_fence(struct vaccel *device, uint32_t ctx_id,
                        uint64_t fence_id, uint32_t ring_idx)
{
    auto ctx = vaccel_context_lookup(device, ctx_id);
    struct vaccel_fence *fence;
    int sync_fd = -1;
    int ret;

    if (!ctx)
        return -ENOENT;

    /* Check if fence already exists */
    if (vaccel_fence_lookup(device, fence_id))
        return -EEXIST;

    fence = static_cast<struct vaccel_fence*>(calloc(1, sizeof(*fence)));
    if (!fence)
        return -ENOMEM;

    fence->id = fence_id;
    fence->ring_idx = ring_idx;
    fence->fd = -1;
#ifdef __unix__
    clock_gettime(CLOCK_MONOTONIC, &fence->timestamp);
#else
    fence->timestamp.tv_sec = 0;
    fence->timestamp.tv_nsec = 0;
#endif

    /* Create sync file for fence
     * TODO: Get actual sync FD from DRM driver after command submission
     * For now, create an eventfd as placeholder
     */
#ifndef WIN32
    sync_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (sync_fd < 0) {
        ret = -errno;
        free(fence);
        return ret;
    }

    /* Signal immediately for now (async submission would set this later) */
    uint64_t val = 1;
    write(sync_fd, &val, sizeof(val));
#endif

    fence->fd = sync_fd;

    /* Add to fence table */
    ret = vaccel_fence_add(device, fence);
    if (ret) {
        if (fence->fd >= 0)
            close(fence->fd);
        free(fence);
        return ret;
    }

    mtx_lock(&ctx->lock);
    ctx->last_fence_id = fence_id;
    mtx_unlock(&ctx->lock);

    return 0;
}
