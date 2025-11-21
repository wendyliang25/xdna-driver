/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: MIT
 * See COPYING.MIT for license details.
 *
 * Derived from virglrenderer:
 * https://gitlab.freedesktop.org/virgl/virglrenderer
 * Original file: src/virgl_fence.c
 */

/*
 * Fence Table Management
 * C++ implementation using std::unordered_map
 */

#include "vaccel_internal.h"
#include "../include/vaccel_renderer.h"
#include "../util/os_file.h"

#include <unordered_map>
#include <cerrno>

#ifdef __unix__
#include <unistd.h>
#endif

#define FENCE_HUNG_CHECK_TIME_SEC   10

/* Fence destructor (custom deleter for shared_ptr) */
static void
fence_destroy_func(struct vaccel_fence *fence)
{
    if (!fence)
        return;

#ifdef __unix__
    if (fence->fd >= 0)
        close(fence->fd);
#endif

    free(fence);
}

int
vaccel_fence_table_init(vaccel *device)
{
    // No initialization needed, std::unordered_map is already constructed
    device->num_fences = 0;
    return 0;
}

void
vaccel_fence_table_cleanup(vaccel *device)
{
    /* shared_ptr automatically cleans up when map is cleared */
    device->fence_table.clear();
    device->num_fences = 0;
}

std::shared_ptr<vaccel_fence>
vaccel_fence_lookup(vaccel *device, uint64_t fence_id)
{
    mtx_lock(&device->fence_lock);
    
    auto it = device->fence_table.find(fence_id);
    std::shared_ptr<vaccel_fence> fence = (it != device->fence_table.end()) ? it->second : nullptr;
    
    mtx_unlock(&device->fence_lock);

    return fence;
}

int
vaccel_fence_add(vaccel *device, struct vaccel_fence *fence)
{
    if (!fence)
        return -EINVAL;

    mtx_lock(&device->fence_lock);
    
    try {
        // Create shared_ptr with custom deleter and store in map
        auto shared_fence = std::shared_ptr<vaccel_fence>(fence, fence_destroy_func);
        auto result = device->fence_table.insert({fence->id, shared_fence});
        if (result.second)
            device->num_fences++;
        mtx_unlock(&device->fence_lock);
        return result.second ? 0 : -EEXIST;
    } catch (...) {
        mtx_unlock(&device->fence_lock);
        return -ENOMEM;
    }
}

void
vaccel_fence_remove(vaccel *device, uint64_t fence_id)
{
    mtx_lock(&device->fence_lock);
    
    auto it = device->fence_table.find(fence_id);
    if (it != device->fence_table.end()) {
        // shared_ptr automatically calls deleter when last reference is destroyed
        device->fence_table.erase(it);
        device->num_fences--;
    }
    
    mtx_unlock(&device->fence_lock);
}

int
vaccel_fence_retire(vaccel *device, uint64_t fence_id)
{
    mtx_lock(&device->fence_lock);

    /* Iterate and retire signalled fences */
    for (auto it = device->fence_table.begin(); it != device->fence_table.end(); ) {
        bool retire = true;

        /* For now, always retire fences immediately on Windows */
        /* On Unix, would check fence status here */
        (void)retire;  /* Suppress unused warning */

        if (retire) {
            // shared_ptr automatically calls deleter when erased
            it = device->fence_table.erase(it);
            device->num_fences--;
        } else {
            ++it;
        }
    }

    /* Check if specific fence is retired */
    auto it = device->fence_table.find(fence_id);
    bool found = (it != device->fence_table.end());
    
    mtx_unlock(&device->fence_lock);

    return found ? -EBUSY : 0;
}

/* Public API implementations */

extern "C" {

int
vaccel_get_fence_fd(void *cookie, uint64_t fence_id)
{
    auto device = vaccel_lookup(cookie);
    int fd = -1;

    if (!device)
        return -1;

    mtx_lock(&device->fence_lock);
    
    auto it = device->fence_table.find(fence_id);
    if (it != device->fence_table.end()) {
        struct vaccel_fence *fence = it->second.get();
        if (fence && fence->fd >= 0)
            fd = os_dupfd_cloexec(fence->fd);
    }
    
    mtx_unlock(&device->fence_lock);

    return fd;
}

} // extern "C"

