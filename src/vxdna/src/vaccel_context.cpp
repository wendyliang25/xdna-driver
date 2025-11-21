/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 * See COPYING.MIT for license details.
 *
 * Derived from virglrenderer:
 * https://gitlab.freedesktop.org/virgl/virglrenderer
 * Original file: src/virgl_context.c
 */

/*
 * Context Table Management
 * C++ implementation using std::unordered_map
 */

#include "vaccel_internal.h"
#include "../include/vaccel_renderer.h"

#include <unordered_map>
#include <cstdlib>
#include <cstring>
#include <cerrno>

/* Context destructor (custom deleter for shared_ptr) */
static void
context_destroy_func(struct vaccel_context *ctx)
{
    if (!ctx)
        return;

    mtx_destroy(&ctx->lock);
    free(ctx->name);
    free(ctx);
}

int
vaccel_context_table_init(vaccel *device)
{
    // No initialization needed, std::unordered_map is already constructed
    device->num_contexts = 0;
    return 0;
}

void
vaccel_context_table_cleanup(vaccel *device)
{
    /* shared_ptr automatically cleans up when map is cleared */
    device->context_table.clear();
    device->num_contexts = 0;
}

std::shared_ptr<vaccel_context>
vaccel_context_lookup(vaccel *device, uint32_t ctx_id)
{
    mtx_lock(&device->context_lock);
    
    auto it = device->context_table.find(ctx_id);
    std::shared_ptr<vaccel_context> ctx = (it != device->context_table.end()) ? it->second : nullptr;
    
    mtx_unlock(&device->context_lock);

    return ctx;
}

int
vaccel_context_add(vaccel *device, struct vaccel_context *ctx)
{
    if (!ctx)
        return -EINVAL;

    mtx_lock(&device->context_lock);
    
    try {
        // Create shared_ptr with custom deleter and store in map
        auto shared_ctx = std::shared_ptr<vaccel_context>(ctx, context_destroy_func);
        auto result = device->context_table.insert({ctx->ctx_id, shared_ctx});
        if (result.second)
            device->num_contexts++;
        mtx_unlock(&device->context_lock);
        return result.second ? 0 : -EEXIST;
    } catch (...) {
        mtx_unlock(&device->context_lock);
        return -ENOMEM;
    }
}

void
vaccel_context_remove(vaccel *device, uint32_t ctx_id)
{
    mtx_lock(&device->context_lock);
    
    auto it = device->context_table.find(ctx_id);
    if (it != device->context_table.end()) {
        // shared_ptr automatically calls deleter when last reference is destroyed
        device->context_table.erase(it);
        device->num_contexts--;
    }
    
    mtx_unlock(&device->context_lock);
}

/* Public API implementations */

extern "C" {

int
vaccel_context_create(void *cookie, uint32_t ctx_id, const char *name)
{
    auto device = vaccel_lookup(cookie);

    if (!device)
        return -ENODEV;

    /* Check if context already exists */
    if (vaccel_context_lookup(device.get(), ctx_id))
        return -EEXIST;

    /* Call DRM backend to create actual context */
    return vaccel_drm_context_create(device.get(), ctx_id, name);
}

void
vaccel_context_destroy(void *cookie, uint32_t ctx_id)
{
    auto device = vaccel_lookup(cookie);

    if (!device)
        return;

    vaccel_drm_context_destroy(device.get(), ctx_id);
}

int
vaccel_submit_ccmd(void *cookie, uint32_t ctx_id, const void *buffer, size_t size)
{
    auto device = vaccel_lookup(cookie);

    if (!device)
        return -ENODEV;

    return vaccel_drm_submit_ccmd(device.get(), ctx_id, buffer, size);
}

int
vaccel_submit_fence(void *cookie, uint32_t ctx_id, uint64_t fence_id, uint32_t ring_idx)
{
    auto device = vaccel_lookup(cookie);

    if (!device)
        return -ENODEV;

    return vaccel_drm_submit_fence(device.get(), ctx_id, fence_id, ring_idx);
}

} // extern "C"

