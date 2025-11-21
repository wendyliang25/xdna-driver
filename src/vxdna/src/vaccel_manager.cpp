/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 * See LICENSE for license details.
 */

/*
 * Device Manager
 * Manages per-device instances with cookie-based lookup
 * C++ implementation using std::unordered_map
 */

#include "vaccel_internal.h"
#include "../include/vaccel_renderer.h"
#include "../util/xvdna_debug.h"

#include <unordered_map>
#include <cerrno>

#ifdef __unix__
#include <unistd.h>
#endif

/* Forward declarations */
static void vaccel_renderer_cleanup(void);

/* Global device table: cookie -> shared_ptr<vaccel> */
static std::unordered_map<void*, std::shared_ptr<vaccel>> device_table;
static mtx_t device_table_lock;
static bool initialized = false;

// vaccel class constructor
vaccel::vaccel(void *cookie, uint32_t capset_id, const struct vaccel_callbacks *callbacks)
    : cookie(cookie)
    , drm_fd((int)(intptr_t)cookie)
    , capset_id(capset_id)
    , callbacks(callbacks)
    , device_ctx(nullptr)
    , virtio_gpu_ccmd_process(nullptr)
    , num_resources(0)
    , num_contexts(0)
    , num_fences(0)
    , num_ccmd_submissions(0)
{
    /* Initialize locks */
    mtx_init(&resource_lock, mtx_plain);
    mtx_init(&context_lock, mtx_plain);
    mtx_init(&fence_lock, mtx_plain);
}

// vaccel class destructor
vaccel::~vaccel()
{
    /* Cleanup device-specific context */
    if (device_ctx) {
        switch (capset_id) {
        case VIRACCEL_CAPSET_ID_AMDXDNA:
            vxdna_device_cleanup(device_ctx);
            break;
        default:
            xvdna_err("accel cleanup failed, Unsupported capset ID: %u", capset_id);
            break;
        }
        device_ctx = nullptr;
    }

    /* Cleanup tables */
    vaccel_fence_table_cleanup(this);
    vaccel_context_table_cleanup(this);
    vaccel_resource_table_cleanup(this);

    /* Cleanup locks */
    mtx_destroy(&fence_lock);
    mtx_destroy(&context_lock);
    mtx_destroy(&resource_lock);

    /* Close DRM FD if needed */
    if (drm_fd >= 0) {
#ifdef __unix__
        close(drm_fd);
#endif
    }
}

static int
vaccel_renderer_init(void)
{
    if (initialized)
        return 0;

    mtx_init(&device_table_lock, mtx_plain);
    initialized = true;

    return 0;
}

/**
 * @brief Automatic initialization when library is loaded
 *
 * This constructor function is called automatically when the shared library
 * is loaded into a process. It ensures vaccel_renderer_init() is called
 * without requiring explicit initialization by the user.
 *
 * Platform support:
 * - Linux/Unix: GCC/Clang __attribute__((constructor))
 */
static void
vaccel_renderer_auto_init(void)
{
    vaccel_renderer_init();
}

/**
 * @brief Automatic cleanup when library is unloaded
 *
 * This destructor function is called automatically when the shared library
 * is unloaded from a process. It ensures proper cleanup of all resources.
 *
 * Platform support:
 * - Linux/Unix: GCC/Clang __attribute__((destructor))
 */
static void
vaccel_renderer_auto_cleanup(void)
{
    vaccel_renderer_cleanup();
}

/* Platform-specific auto-initialization setup */
#if defined(__GNUC__) || defined(__clang__)
    /* GCC and Clang */
    __attribute__((constructor))
    static void _vaccel_constructor(void) {
        vaccel_renderer_auto_init();
    }

    __attribute__((destructor))
    static void _vaccel_destructor(void) {
        vaccel_renderer_auto_cleanup();
    }
#else
    #error "Unsupported compiler. This library requires GCC, Clang."
#endif

static void
vaccel_renderer_cleanup(void)
{
    if (!initialized)
        return;

    mtx_lock(&device_table_lock);
    
    // Map automatically destroys all vaccel objects
    device_table.clear();
    
    mtx_unlock(&device_table_lock);

    mtx_destroy(&device_table_lock);
    initialized = false;
}


/**
 * @brief Add a device to the global device table
 *
 * @param device Shared pointer to device (moved into table)
 * @return 0 on success, negative errno on failure
 */
static int
vaccel_add(std::shared_ptr<vaccel>&& device)
{
    if (!initialized)
        return -EINVAL;

    if (!device)
        return -EINVAL;

    void *cookie = device->cookie;

    mtx_lock(&device_table_lock);
    
    try {
        // Move shared_ptr into map (transfers ownership)
        auto result = device_table.try_emplace(cookie, std::move(device));
        mtx_unlock(&device_table_lock);
        return result.second ? 0 : -EEXIST;
    } catch (...) {
        mtx_unlock(&device_table_lock);
        return -ENOMEM;
    }
}


/**
 * @brief Remove a device from the global device table
 *
 * @param cookie Device cookie
 */
static void
vaccel_remove(void *cookie)
{
    if (!initialized)
        return;

    mtx_lock(&device_table_lock);
    
    // Erasing from map automatically calls destructor
    device_table.erase(cookie);
    
    mtx_unlock(&device_table_lock);
}


/**
 * @brief Look up a device by its cookie
 *
 * @param cookie Device cookie
 * @return Shared pointer to device if found, nullptr otherwise
 */
std::shared_ptr<vaccel>
vaccel_lookup(void *cookie)
{
    if (!initialized)
        return nullptr;

    mtx_lock(&device_table_lock);
    
    auto it = device_table.find(cookie);
    std::shared_ptr<vaccel> device = (it != device_table.end()) ? it->second : nullptr;
    
    mtx_unlock(&device_table_lock);

    return device;
}

extern "C" {

int
vaccel_create(void *cookie, uint32_t capset_id, const struct vaccel_callbacks *callbacks)
{
    int ret;

    if (!initialized) {
        xvdna_err("Renderer not initialized");
        return -EINVAL;
    }
    if (capset_id != VIRACCEL_CAPSET_ID_AMDXDNA) {
        xvdna_err("Unsupported capset ID: %u", capset_id);
        return -EINVAL;
    }

    /* Check if device already exists */
    if (vaccel_lookup(cookie)) {
        xvdna_err("Device already exists for cookie %p", cookie);
        return -EEXIST;
    }

    /* Create device as shared_ptr */
    auto device = std::make_shared<vaccel>(cookie, capset_id, callbacks);

    /* Add to global table (moves shared_ptr into map) */
    ret = vaccel_add(std::move(device));
    if (ret) {
        return ret;
    }

    /* Get shared pointer to device in map for initialization */
    auto device_ptr = vaccel_lookup(cookie);
    if (!device_ptr) {
        xvdna_err("Failed to lookup device after creation");
        return -EINVAL;
    }

    /* Initialize device-specific context */
    switch (capset_id) {
    case VIRACCEL_CAPSET_ID_AMDXDNA:
        device_ptr->device_ctx = vxdna_device_init(cookie);
        if (!device_ptr->device_ctx) {
            xvdna_err("Failed to initialize AMDXDNA device");
            vaccel_remove(cookie);
            return -EINVAL;
        }
        xvdna_info("AMDXDNA device context initialized");
        break;
    default:
        device_ptr->device_ctx = nullptr;
        break;
    }

    xvdna_info("Device created successfully: cookie=%p, capset_id=%u, fd=%d",
               cookie, capset_id, device_ptr->drm_fd);

    return 0;
}

void
vaccel_destroy(void *cookie)
{
    if (!vaccel_lookup(cookie))
        return;

    /* Remove from global table (automatically calls destructor) */
    vaccel_remove(cookie);
}

} // extern "C"

