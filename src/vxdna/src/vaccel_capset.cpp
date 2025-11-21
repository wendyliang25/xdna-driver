/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * Capset Management
 * Handles virtio vaccel capability set information retrieval
 */

#include "vaccel_internal.h"
#include "../include/vaccel_renderer.h"
#include "../util/xvdna_debug.h"

#include <errno.h>
#include <string.h>

/*
 * Static capset instance for AMDXDNA vaccel
 * This defines the supported capability set for the vaccel renderer
 */
static const struct vaccel_drm_capset vaccel_capset_instance = {
    .max_version = 1,
    .min_version = 1,
    .context_type = VIRACCEL_CONTEXT_AMDXDNA,
};

int
vaccel_get_capset_info(void *cookie, uint32_t capset_id,
                           uint32_t *max_version, uint32_t *max_size)
{
    xvdna_dbg("Getting capset info for capset_id=%u, cookie=%p", capset_id, cookie);

    /* Lookup device by cookie */
    auto device = vaccel_lookup(cookie);
    if (!device) {
        xvdna_err("Device not found for cookie %p", cookie);
        return -ENODEV;
    }

    /* Validate capset ID */
    if (capset_id != VIRACCEL_CAPSET_ID_AMDXDNA) {
        xvdna_err("Unsupported capset ID: %u (expected %u)",
                  capset_id, VIRACCEL_CAPSET_ID_AMDXDNA);
        return -ENOTSUP;
    }

    /* Return max version if requested */
    if (max_version) {
        *max_version = vaccel_capset_instance.max_version;
        xvdna_dbg("Returning max_version=%u", *max_version);
    }

    /* Return max size if requested */
    if (max_size) {
        /*
         * The size represents the size of the capset structure
         */
        *max_size = sizeof(struct vaccel_drm_capset);
        xvdna_dbg("Returning max_size=%u", *max_size);
    }

    xvdna_info("Capset info retrieved successfully for capset_id=%u", capset_id);
    return 0;
}

int
vaccel_fill_capset(void *cookie, uint32_t capset_id, uint32_t capset_version,
                       uint32_t capset_size, void *capset_buf)
{
    xvdna_dbg("Filling capset for capset_id=%u, capset_version=%u, cookie=%p, capset_size=%u",
              capset_id, capset_version, cookie, capset_size);

    /* Lookup device by cookie */
    auto device = vaccel_lookup(cookie);
    if (!device) {
        xvdna_err("Device not found for cookie %p", cookie);
        return -ENODEV;
    }

    /* Validate capset ID */
    if (capset_id != VIRACCEL_CAPSET_ID_AMDXDNA) {
        xvdna_err("Unsupported capset ID: %u (expected %u)",
                  capset_id, VIRACCEL_CAPSET_ID_AMDXDNA);
        return -ENOTSUP;
    }

    /* Validate buffer and size */
    if (!capset_buf) {
        xvdna_err("capset_buf is NULL");
        return -EINVAL;
    }
    if (capset_size < sizeof(struct vaccel_drm_capset)) {
        xvdna_err("Provided capset_size (%u) is smaller than expected (%zu)",
                  capset_size, sizeof(struct vaccel_drm_capset));
        return -EINVAL;
    }

    /* Copy the capset structure to user buffer */
    memcpy(capset_buf, &vaccel_capset_instance, sizeof(struct vaccel_drm_capset));
    xvdna_info("Capset structure filled for capset_id=%u, version=%u", capset_id, capset_version);

    return 0;
}
