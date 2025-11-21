/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/**
 * @file capset_example.c
 * @brief Example demonstrating vaccel capset information retrieval
 *
 * This example shows how to use the vaccel_get_capset_info() API
 * to query virtio vaccel capability set information.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "vaccel_renderer.h"

#define VIRTGPU_DRM_CAPSET_DRM 6

int main(int argc, char *argv[])
{
    void *cookie;
    uint32_t capset_id = VIRTGPU_DRM_CAPSET_DRM;
    uint32_t max_version = 0;
    uint32_t max_size = 0;
    int ret;
    int fd;

    if (argc > 1) {
        capset_id = atoi(argv[1]);
    }

    /* Open a DRM device (use device FD as cookie) */
    fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) {
        perror("Failed to open DRM device");
        return 1;
    }

    /* Create a vaccel device with the FD as cookie */
    cookie = (void *)(intptr_t)fd;
    ret = vaccel_create(cookie);
    if (ret) {
        fprintf(stderr, "Failed to create vaccel device: %d\n", ret);
        close(fd);
        return 1;
    }

    /* Get capset information */
    ret = vaccel_get_capset_info(cookie, capset_id, &max_version, &max_size);
    if (ret) {
        fprintf(stderr, "Failed to get capset info: %d\n", ret);
    } else {
        printf("Capset ID: %u\n", capset_id);
        printf("Max Version: %u\n", max_version);
        printf("Max Size: %u bytes\n", max_size);
    }

    /* Cleanup */
    vaccel_destroy(cookie);
    close(fd);

    return ret ? 1 : 0;
}

