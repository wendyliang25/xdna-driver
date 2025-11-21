/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * vaccel Renderer Example
 * Demonstrates multi-device usage with resources, contexts, and fences
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "../include/vaccel_renderer.h"

#define DRM_DEVICE1  "/dev/dri/renderD128"
#define DRM_DEVICE2  "/dev/dri/renderD129"

int main(void)
{
    int ret;
    int drm_fd1 = -1, drm_fd2 = -1;
    void *cookie1, *cookie2;

    printf("=== vaccel Renderer Example ===\n\n");
    printf("Note: Library initializes automatically\n\n");

    /* Step 1: Open DRM devices */
    printf("1. Opening DRM devices...\n");
    drm_fd1 = open(DRM_DEVICE1, O_RDWR);
    if (drm_fd1 < 0) {
        fprintf(stderr, "Failed to open %s: %d\n", DRM_DEVICE1, errno);
        goto cleanup;
    }
    printf("   ✓ Opened %s (fd=%d)\n", DRM_DEVICE1, drm_fd1);

    drm_fd2 = open(DRM_DEVICE2, O_RDWR);
    if (drm_fd2 < 0) {
        fprintf(stderr, "Warning: Failed to open %s, using single device\n", DRM_DEVICE2);
        drm_fd2 = -1;
    } else {
        printf("   ✓ Opened %s (fd=%d)\n", DRM_DEVICE2, drm_fd2);
    }
    printf("\n");

    /* Step 2: Create devices */
    printf("2. Creating devices...\n");
    cookie1 = (void *)(intptr_t)drm_fd1;
    ret = vaccel_create(cookie1);
    if (ret < 0) {
        fprintf(stderr, "Failed to create device1: %d\n", ret);
        goto cleanup;
    }
    printf("   ✓ Created device1 (cookie=%p)\n", cookie1);

    if (drm_fd2 >= 0) {
        cookie2 = (void *)(intptr_t)drm_fd2;
        ret = vaccel_create(cookie2);
        if (ret < 0) {
            fprintf(stderr, "Failed to create device2: %d\n", ret);
            goto cleanup;
        }
        printf("   ✓ Created device2 (cookie=%p)\n", cookie2);
    }
    printf("\n");

    /* Step 3: Create contexts on both devices */
    printf("3. Creating contexts...\n");
    ret = vaccel_context_create(cookie1, 1, "device1_context");
    if (ret < 0) {
        fprintf(stderr, "Failed to create context on device1: %d\n", ret);
        goto cleanup;
    }
    printf("   ✓ Created context 1 on device1\n");

    if (drm_fd2 >= 0) {
        ret = vaccel_context_create(cookie2, 1, "device2_context");
        if (ret < 0) {
            fprintf(stderr, "Failed to create context on device2: %d\n", ret);
            goto cleanup;
        }
        printf("   ✓ Created context 1 on device2\n");
    }
    printf("\n");

    /* Step 4: Create resources on both devices */
    printf("4. Creating resources (4MB each)...\n");
    uint32_t res_id = 100;
    uint64_t size = 4 * 1024 * 1024;

    ret = vaccel_resource_create(cookie1, res_id, size, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create resource on device1: %d\n", ret);
        goto cleanup;
    }
    printf("   ✓ Created resource %u on device1 (size=%lu)\n", res_id, size);

    if (drm_fd2 >= 0) {
        ret = vaccel_resource_create(cookie2, res_id, size, 0);
        if (ret < 0) {
            fprintf(stderr, "Failed to create resource on device2: %d\n", ret);
            goto cleanup;
        }
        printf("   ✓ Created resource %u on device2 (size=%lu)\n", res_id, size);
    }
    printf("\n");

    /* Step 5: Export resource FDs */
    printf("5. Exporting resource FDs...\n");
    int res_fd1 = -1, res_fd2 = -1;

    ret = vaccel_resource_export_fd(cookie1, res_id, &res_fd1);
    if (ret == 0) {
        printf("   ✓ Device1 resource FD: %d\n", res_fd1);
    } else {
        fprintf(stderr, "   ✗ Failed to export resource from device1: %d\n", ret);
    }

    if (drm_fd2 >= 0) {
        ret = vaccel_resource_export_fd(cookie2, res_id, &res_fd2);
        if (ret == 0) {
            printf("   ✓ Device2 resource FD: %d\n", res_fd2);
        } else {
            fprintf(stderr, "   ✗ Failed to export resource from device2: %d\n", ret);
        }
    }
    printf("\n");

    /* Step 6: Submit commands */
    printf("6. Submitting commands...\n");
    struct {
        uint32_t cmd_type;
        uint32_t resource_id;
    } cmd = { .cmd_type = 1, .resource_id = res_id };

    ret = vaccel_submit_ccmd(cookie1, 1, &cmd, sizeof(cmd));
    if (ret < 0) {
        fprintf(stderr, "Failed to submit command to device1: %d\n", ret);
    } else {
        printf("   ✓ Submitted command to device1\n");
    }

    if (drm_fd2 >= 0) {
        ret = vaccel_submit_ccmd(cookie2, 1, &cmd, sizeof(cmd));
        if (ret < 0) {
            fprintf(stderr, "Failed to submit command to device2: %d\n", ret);
        } else {
            printf("   ✓ Submitted command to device2\n");
        }
    }
    printf("\n");

    /* Step 7: Submit fences */
    printf("7. Submitting fences...\n");
    uint64_t fence_id = 1;
    uint32_t ring_idx = 0;

    ret = vaccel_submit_fence(cookie1, 1, fence_id, ring_idx);
    if (ret < 0) {
        fprintf(stderr, "Failed to submit fence to device1: %d\n", ret);
    } else {
        printf("   ✓ Submitted fence %lu to device1 (ring=%u)\n", fence_id, ring_idx);
    }

    if (drm_fd2 >= 0) {
        ret = vaccel_submit_fence(cookie2, 1, fence_id, ring_idx);
        if (ret < 0) {
            fprintf(stderr, "Failed to submit fence to device2: %d\n", ret);
        } else {
            printf("   ✓ Submitted fence %lu to device2 (ring=%u)\n", fence_id, ring_idx);
        }
    }
    printf("\n");

    /* Step 8: Get fence FDs */
    printf("8. Getting fence FDs...\n");
    int fence_fd1 = vaccel_get_fence_fd(cookie1, fence_id);
    if (fence_fd1 >= 0) {
        printf("   ✓ Device1 fence FD: %d\n", fence_fd1);
        close(fence_fd1);
    } else {
        fprintf(stderr, "   ✗ Failed to get fence FD from device1\n");
    }

    if (drm_fd2 >= 0) {
        int fence_fd2 = vaccel_get_fence_fd(cookie2, fence_id);
        if (fence_fd2 >= 0) {
            printf("   ✓ Device2 fence FD: %d\n", fence_fd2);
            close(fence_fd2);
        } else {
            fprintf(stderr, "   ✗ Failed to get fence FD from device2\n");
        }
    }
    printf("\n");

    /* Step 10: Cleanup */
    printf("10. Cleaning up...\n");

    if (res_fd1 >= 0)
        close(res_fd1);
    if (res_fd2 >= 0)
        close(res_fd2);

    vaccel_resource_destroy(cookie1, res_id);
    printf("   ✓ Destroyed resource on device1\n");

    if (drm_fd2 >= 0) {
        vaccel_resource_destroy(cookie2, res_id);
        printf("   ✓ Destroyed resource on device2\n");
    }

    vaccel_context_destroy(cookie1, 1);
    printf("   ✓ Destroyed context on device1\n");

    if (drm_fd2 >= 0) {
        vaccel_context_destroy(cookie2, 1);
        printf("   ✓ Destroyed context on device2\n");
    }

    vaccel_destroy(cookie1);
    printf("   ✓ Destroyed device1\n");

    if (drm_fd2 >= 0) {
        vaccel_destroy(cookie2);
        printf("   ✓ Destroyed device2\n");
    }

    printf("   ✓ Library cleanup automatic on exit\n");
    printf("\n");

cleanup:
    if (drm_fd1 >= 0)
        close(drm_fd1);
    if (drm_fd2 >= 0)
        close(drm_fd2);

    printf("=== Example completed ===\n");
    return 0;
}


