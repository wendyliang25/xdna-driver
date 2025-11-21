/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file callbacks_example.c
 * @brief Example demonstrating vaccel_callbacks usage
 *
 * This example shows how to use custom callbacks, particularly
 * the get_device_fd() callback for flexible cookie-to-FD mapping.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#ifdef __unix__
#include <unistd.h>
#endif

#include "vaccel_renderer.h"
#include "../util/xvdna_debug.h"

/* Custom device context */
struct my_device_context {
    int device_fd;
    char device_path[256];
    int ref_count;
};

/**
 * @brief Custom get_device_fd callback
 *
 * This callback demonstrates how to implement custom cookie-to-FD mapping.
 * In this example, the cookie is a pointer to a custom device context.
 */
static int
my_get_device_fd(void *cookie, void *user_data)
{
    struct my_device_context *ctx = (struct my_device_context *)cookie;
    
    (void)user_data;  /* Unused in this example */
    
    if (!ctx) {
        fprintf(stderr, "Invalid cookie (NULL)\n");
        return -EINVAL;
    }
    
    printf("get_device_fd callback: device_path='%s', fd=%d\n",
           ctx->device_path, ctx->device_fd);
    
    /* Increment reference count */
    ctx->ref_count++;
    
    return ctx->device_fd;
}

int main(int argc, char *argv[])
{
    struct my_device_context dev_ctx;
    struct vaccel_callbacks callbacks;
    struct vaccel_drm_capset capset;
    uint32_t max_version, max_size;
    void *cookie;
    int ret;

    printf("=== XVDNA Callbacks Example ===\n\n");

    /* Enable debug logging */
    xvdna_set_log_level(XVDNA_LOG_DEBUG);

    /* Initialize device context */
    snprintf(dev_ctx.device_path, sizeof(dev_ctx.device_path),
             "/dev/dri/renderD128");
    dev_ctx.device_fd = open(dev_ctx.device_path, O_RDWR);
    dev_ctx.ref_count = 0;

    if (dev_ctx.device_fd < 0) {
        perror("Failed to open DRM device");
        return 1;
    }

    printf("1. Setup device context:\n");
    printf("   Path: %s\n", dev_ctx.device_path);
    printf("   FD: %d\n\n", dev_ctx.device_fd);

    /* Setup callbacks */
    callbacks.get_device_fd = my_get_device_fd;
    callbacks.user_data = NULL;  /* Could pass additional context here */

    /* Use device context as cookie */
    cookie = &dev_ctx;

    printf("2. Create vaccel device with callbacks:\n");
    ret = vaccel_create(cookie, VIRACCEL_CAPSET_ID_AMDXDNA, &callbacks);
    if (ret) {
        fprintf(stderr, "Failed to create vaccel device: %d\n", ret);
        close(dev_ctx.device_fd);
        return 1;
    }
    printf("   Device created successfully\n");
    printf("   Reference count: %d\n\n", dev_ctx.ref_count);

    printf("3. Query capset information:\n");
    ret = vaccel_get_capset_info(cookie, VIRACCEL_CAPSET_ID_AMDXDNA,
                                  &max_version, &max_size);
    if (ret) {
        fprintf(stderr, "Failed to get capset info: %d\n", ret);
    } else {
        printf("   Max Version: %u\n", max_version);
        printf("   Max Size: %u bytes\n\n", max_size);
    }

    printf("4. Fill capset structure:\n");
    ret = vaccel_fill_capset(cookie, VIRACCEL_CAPSET_ID_AMDXDNA,
                              max_version, sizeof(capset), &capset);
    if (ret) {
        fprintf(stderr, "Failed to fill capset: %d\n", ret);
    } else {
        printf("   Capset filled successfully\n");
        printf("   Max version: %u\n", capset.max_version);
        printf("   Min version: %u\n", capset.min_version);
        printf("   Context type: %u\n\n", capset.context_type);
    }

    printf("5. Cleanup:\n");
    vaccel_destroy(cookie);
    printf("   Device destroyed\n");
    printf("   Final reference count: %d\n", dev_ctx.ref_count);

    close(dev_ctx.device_fd);
    printf("   Device FD closed\n\n");

    printf("Example complete!\n");
    return 0;
}

