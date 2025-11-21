/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 * See COPYING.MIT for license details.
 *
 * Derived from virglrenderer:
 * https://gitlab.freedesktop.org/virgl/virglrenderer
 * Original file: src/virgl_resource.c
 */

/*
 * Resource Table Management
 * C++ implementation using std::unordered_map
 */

#include "vaccel_internal.h"
#include "../include/vaccel_renderer.h"

#include <unordered_map>
#include <cstdlib>
#include <cerrno>

#ifdef __unix__
#include <unistd.h>
#endif

/* Resource destructor (custom deleter for shared_ptr) */
static void
resource_destroy_func(struct vaccel_resource *res)
{
    if (!res)
        return;

#ifdef __unix__
    if (res->fd >= 0)
        close(res->fd);
#endif

    free(res);
}

int
vaccel_resource_table_init(vaccel *device)
{
    // No initialization needed, std::unordered_map is already constructed
    device->num_resources = 0;
    return 0;
}

void
vaccel_resource_table_cleanup(vaccel *device)
{
    /* shared_ptr automatically cleans up when map is cleared */
    device->resource_table.clear();
    device->num_resources = 0;
}

std::shared_ptr<vaccel_resource>
vaccel_resource_lookup(vaccel *device, uint32_t res_id)
{
    mtx_lock(&device->resource_lock);
    
    auto it = device->resource_table.find(res_id);
    std::shared_ptr<vaccel_resource> res = (it != device->resource_table.end()) ? it->second : nullptr;
    
    mtx_unlock(&device->resource_lock);

    return res;
}

int
vaccel_resource_add(vaccel *device, struct vaccel_resource *res)
{
    if (!res)
        return -EINVAL;

    mtx_lock(&device->resource_lock);
    
    try {
        // Create shared_ptr with custom deleter and store in map
        auto shared_res = std::shared_ptr<vaccel_resource>(res, resource_destroy_func);
        auto result = device->resource_table.insert({res->res_id, shared_res});
        if (result.second)
            device->num_resources++;
        mtx_unlock(&device->resource_lock);
        return result.second ? 0 : -EEXIST;
    } catch (...) {
        mtx_unlock(&device->resource_lock);
        return -ENOMEM;
    }
}

void
vaccel_resource_remove(vaccel *device, uint32_t res_id)
{
    mtx_lock(&device->resource_lock);
    
    auto it = device->resource_table.find(res_id);
    if (it != device->resource_table.end()) {
        // shared_ptr automatically calls deleter when last reference is destroyed
        device->resource_table.erase(it);
        device->num_resources--;
    }
    
    mtx_unlock(&device->resource_lock);
}

/* Public API implementations */

extern "C" {

int
vaccel_resource_create(void *cookie, uint32_t res_id, uint64_t size, uint32_t flags)
{
    auto device = vaccel_lookup(cookie);

    if (!device)
        return -ENODEV;

    /* Check if resource already exists */
    if (vaccel_resource_lookup(device.get(), res_id))
        return -EEXIST;

    /* Call DRM backend to create actual resource */
    return vaccel_drm_resource_create(device.get(), res_id, size, flags);
}

void
vaccel_resource_destroy(void *cookie, uint32_t res_id)
{
    auto device = vaccel_lookup(cookie);

    if (!device)
        return;

    vaccel_drm_resource_destroy(device.get(), res_id);
}

int
vaccel_resource_export_fd(void *cookie, uint32_t res_id, int *fd)
{
    auto device = vaccel_lookup(cookie);

    if (!device)
        return -ENODEV;

    return vaccel_drm_resource_export_fd(device.get(), res_id, fd);
}

} // extern "C"

