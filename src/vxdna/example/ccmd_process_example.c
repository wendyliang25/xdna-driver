/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file ccmd_process_example.c
 * @brief Example demonstrating virtio_gpu_ccmd_process callback
 *
 * This example shows how to register and use the command processing
 * callback with AMDXDNA device initialization.
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

/* Example command structure */
struct example_cmd {
    uint32_t cmd_type;
    uint32_t cmd_size;
    uint8_t data[64];
};

/* Application context */
struct app_context {
    int cmd_count;
    int error_count;
};

/**
 * @brief Custom command processing callback
 *
 * This callback demonstrates how to process virtio GPU commands.
 */
static int
my_ccmd_process(void *ctx, const void *cmd_buf, size_t buf_size)
{
    const struct example_cmd *cmd = (const struct example_cmd *)cmd_buf;
    struct app_context *app = (struct app_context *)ctx;

    printf("  [CALLBACK] Processing command:\n");
    printf("    Context: %p\n", ctx);
    printf("    Buffer size: %zu bytes\n", buf_size);

    /* Validate buffer size */
    if (buf_size < sizeof(struct example_cmd)) {
        printf("    ERROR: Buffer too small\n");
        if (app)
            app->error_count++;
        return -EINVAL;
    }

    /* Process the command */
    printf("    Command type: 0x%x\n", cmd->cmd_type);
    printf("    Command size: %u\n", cmd->cmd_size);
    printf("    Data: %02x %02x %02x %02x...\n",
           cmd->data[0], cmd->data[1], cmd->data[2], cmd->data[3]);

    /* Update statistics */
    if (app)
        app->cmd_count++;

    printf("    SUCCESS: Command processed\n");
    return 0;
}

/**
 * @brief Helper function to call command processing
 */
static int
process_command_example(void *cookie, const struct example_cmd *cmd)
{
    /* This would normally call the internal vxdna_device_process_ccmd */
    /* For demonstration, we'll call the callback directly */
    struct app_context app_ctx = { .cmd_count = 0, .error_count = 0 };
    
    printf("\n=== Processing Example Command ===\n");
    int ret = my_ccmd_process(&app_ctx, cmd, sizeof(*cmd));
    printf("Result: %d\n", ret);
    printf("Statistics: %d commands, %d errors\n\n",
           app_ctx.cmd_count, app_ctx.error_count);
    
    return ret;
}

int main(int argc, char *argv[])
{
    struct vaccel_callbacks callbacks;
    struct example_cmd cmd;
    void *cookie;
    int fd;
    int ret;

    (void)argc;
    (void)argv;

    printf("=== XVDNA Command Processing Example ===\n\n");

    /* Enable debug logging */
    xvdna_set_log_level(XVDNA_LOG_DEBUG);

    /* Open DRM device */
    printf("1. Opening DRM device:\n");
#ifdef __unix__
    fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) {
        perror("   Failed to open DRM device");
        printf("   Note: This is normal if device doesn't exist\n");
        fd = 5;  /* Use dummy FD for demo */
    } else {
        printf("   Device opened: fd=%d\n", fd);
    }
#else
    fd = 5;  /* Dummy FD for non-Unix */
    printf("   Using dummy FD for demo: fd=%d\n", fd);
#endif

    printf("\n");

    /* Setup callbacks (for get_device_fd if needed) */
    printf("2. Setting up callbacks:\n");
    memset(&callbacks, 0, sizeof(callbacks));
    printf("   Note: virtio_gpu_ccmd_process is set directly on vaccel struct\n");
    printf("\n");

    /* Create device */
    printf("3. Creating vaccel device:\n");
    cookie = (void *)(intptr_t)fd;
    ret = vaccel_create(cookie, VIRACCEL_CAPSET_ID_AMDXDNA, NULL);
    if (ret) {
        fprintf(stderr, "   Failed to create vaccel device: %d\n", ret);
        /* Continue for demonstration purposes */
    } else {
        printf("   Device created successfully\n");
    }
    printf("\n");

    /* Prepare example command */
    printf("4. Preparing example command:\n");
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd_type = 0x1234;
    cmd.cmd_size = sizeof(cmd);
    cmd.data[0] = 0xAA;
    cmd.data[1] = 0xBB;
    cmd.data[2] = 0xCC;
    cmd.data[3] = 0xDD;
    printf("   Command type: 0x%x\n", cmd.cmd_type);
    printf("   Command size: %u\n", cmd.cmd_size);
    printf("\n");

    /* Process command */
    printf("5. Processing command via callback:\n");
    ret = process_command_example(cookie, &cmd);
    if (ret) {
        fprintf(stderr, "   Command processing failed: %d\n", ret);
    }

    /* Test with invalid buffer size */
    printf("6. Testing error handling (buffer too small):\n");
    struct example_cmd small_cmd;
    memset(&small_cmd, 0, sizeof(small_cmd));
    small_cmd.cmd_type = 0x5678;
    
    /* Simulate small buffer by passing smaller size */
    printf("   Passing buffer size of 4 bytes (expected: %zu)\n", sizeof(small_cmd));
    struct app_context app_ctx = { .cmd_count = 0, .error_count = 0 };
    ret = my_ccmd_process(&app_ctx, &small_cmd, 4);
    printf("   Result: %d (expected: %d)\n\n", ret, -EINVAL);

    /* Cleanup */
    printf("7. Cleanup:\n");
    if (ret == 0) {
        vaccel_destroy(cookie);
        printf("   Device destroyed\n");
    }

#ifdef __unix__
    if (fd >= 0 && fd != 5) {
        close(fd);
        printf("   Device FD closed\n");
    }
#endif

    printf("\nExample complete!\n");
    printf("\nKey Points:\n");
    printf("- virtio_gpu_ccmd_process is a member of vaccel struct (not vaccel_callbacks)\n");
    printf("- Callback signature: int (*)(void *ctx, const void *cmd_buf, size_t buf_size)\n");
    printf("- Returns 0 for success, negative errno for errors\n");
    printf("- vxdna_device_init() is called automatically by vaccel_create()\n");
    printf("- Device context is stored and passed to callback\n");

    return 0;
}

