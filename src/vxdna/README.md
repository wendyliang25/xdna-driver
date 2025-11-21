# vaccel-renderer

Multi-device renderer with cookie-based device management, inspired by virglrenderer architecture.

## Overview

This library provides a multi-device renderer with per-device lookup tables for resources, contexts, and fences. Each device instance is identified by a "cookie" (typically a DRM FD), allowing multiple independent rendering contexts.

## Architecture

### Cookie-Based Device Management

Each device is registered with a unique cookie that serves as its identifier. All operations require the cookie parameter to identify which device instance to operate on.

### Per-Device Tables

Each device maintains separate hash tables for:

#### Resource Table
- Maps `res_id` (uint32_t) → `vaccel_resource`
- Similar to `virgl_resource.c`
- Tracks GPU buffer objects (BOs) and their DMA-BUF exports
- Thread-safe with per-device locks

#### Context Table
- Maps `ctx_id` (uint32_t) → `vaccel_context`
- Similar to `virgl_context.c`
- Each context represents an independent command stream
- Tracks last submitted fence per context

#### Fence Table
- Maps `fence_id` (uint64_t) → `vaccel_fence`
- Similar to `virgl_fence.c`
- Uses 64-bit fence IDs for timeline synchronization
- Provides sync file FDs for CPU-GPU synchronization

### Local Hash Table Implementation

All hash tables are implemented locally without external dependencies:
- **32-bit hash tables** for resources and contexts (using multiplicative hashing)
- **64-bit hash table** for fences (using FNV-1a hashing)
- **Pointer hash table** for device management
- Dynamic resizing with 0.75 load factor threshold
- Separate chaining for collision resolution

## API Reference

### Device Management

The library initializes automatically when loaded. No explicit initialization required.

```c
int vaccel_create(void *cookie);
void vaccel_destroy(void *cookie);
```

Create/destroy a device instance. The `cookie` is typically a DRM FD cast to `void*`.

### Resource Management

```c
int vaccel_resource_create(void *cookie, uint32_t res_id,
                           uint64_t size, uint32_t flags);
void vaccel_resource_destroy(void *cookie, uint32_t res_id);
int vaccel_resource_export_fd(void *cookie, uint32_t res_id, int *fd);
```

Create GPU resources (buffers/images) and export them as DMA-BUF FDs.

### Context Management

```c
int vaccel_context_create(void *cookie, uint32_t ctx_id, const char *name);
void vaccel_context_destroy(void *cookie, uint32_t ctx_id);
```

Create rendering contexts for command submission.

### Command Submission

```c
int vaccel_submit_ccmd(void *cookie, uint32_t ctx_id,
                       const void *buffer, size_t size);
int vaccel_submit_fence(void *cookie, uint32_t ctx_id,
                        uint64_t fence_id, uint32_t ring_idx);
int vaccel_get_fence_fd(void *cookie, uint64_t fence_id);
```

Submit commands and create fence synchronization points.

### Capability Set Information

```c
int vaccel_get_capset_info(void *cookie, uint32_t capset_id,
                            uint32_t *max_version, uint32_t *max_size);
```

Retrieve virtio vaccel capability set information. This API queries the maximum supported version and size for a given capability set ID. Useful for discovering device capabilities and negotiating protocol versions.

### Logging Utilities

```c
void xvdna_err(const char *fmt, ...);   // Error messages
void xvdna_info(const char *fmt, ...);  // Info messages  
void xvdna_dbg(const char *fmt, ...);   // Debug messages
```

Provides logging with `[XVDNA]` prefix and configurable log levels (ERROR, INFO, DEBUG). Control via `xvdna_set_log_level()` or `XVDNA_LOG_LEVEL` environment variable. See `util/LOGGING.md` for details.

### Fence Timeline Synchronization

Fences use 64-bit IDs and support:
- Export to sync file FDs (`vaccel_get_fence_fd`)
- Timeline-based synchronization with ring indices
- Hung fence detection (10 second timeout)
- Automatic fence retirement

## Implementation Details

### DRM Backend

The DRM backend (`vaccel_drm_backend.c`) implements:
- Resource creation using dumb buffers (example implementation)
- DMA-BUF export via PRIME handles
- Context creation using legacy DRM contexts
- Command submission (stub, driver-specific implementation needed)
- Fence creation using eventfds (placeholder for sync files)

### Thread Safety

All tables are protected by per-device mutexes:
- `device->resource_lock`
- `device->context_lock`
- `device->fence_lock`

### Hash Table Performance

- Initial size: 32 buckets
- Automatic growth at 0.75 load factor
- O(1) average case for insert/lookup/remove
- Separate chaining handles collisions gracefully

## Comparison with virglrenderer

### API Differences

| API Function | virglrenderer Equivalent | Notes |
|--------------|-------------------------|-------|
| `vaccel_renderer_init()` | `virgl_renderer_init()` | No device-specific init |
| `vaccel_create(cookie)` | N/A | **New**: Per-device initialization |
| `vaccel_context_create()` | `virgl_renderer_context_create()` | **+cookie** parameter |
| `vaccel_resource_create()` | `virgl_renderer_resource_create_blob()` | **+cookie** parameter |
| `vaccel_submit_ccmd()` | `virgl_renderer_submit_cmd()` | **+cookie** parameter |
| `vaccel_submit_fence()` | `virgl_renderer_context_create_fence()` | **+cookie** parameter |
| `vaccel_get_fence_fd()` | `virgl_renderer_export_fence()` | **+cookie** parameter |

### Traditional (virglrenderer)
```c
// Single global device
virgl_renderer_init(...);
virgl_renderer_context_create(ctx_id, ...);
virgl_renderer_submit_cmd(ctx_id, ...);
```

### Multi-Device (vaccel-renderer)
```c
// Multiple independent devices
vaccel_renderer_init();
vaccel_create(cookie1);  // Device 1
vaccel_create(cookie2);  // Device 2

// Operations require cookie
vaccel_context_create(cookie1, ctx_id, ...);
vaccel_submit_ccmd(cookie1, ctx_id, ...);

vaccel_context_create(cookie2, ctx_id, ...);  // Same ctx_id, different device!
vaccel_submit_ccmd(cookie2, ctx_id, ...);
```

## Building

### Dependencies

- **libdrm** (>= 2.4.100): DRM/KMS interface
- **threads**: POSIX threads (C11 threads.h)

No external dependencies on virglrenderer or Mesa utilities.

### Build with Meson

```bash
meson setup build
ninja -C build
```

### Build Options

```bash
# Debug build
meson setup build --buildtype=debug

# Release build
meson setup build --buildtype=release

# Install
ninja -C build install
```

## Example Usage

The library automatically initializes when loaded:

```c
#include <vaccel_renderer.h>
#include "util/xvdna_debug.h"

#define VIRTGPU_DRM_CAPSET_DRM 6

int main() {
    // Library initializes automatically - just use the API!

    // Optional: Enable debug logging
    xvdna_set_log_level(XVDNA_LOG_DEBUG);

    // Open DRM device
    int drm_fd = open("/dev/dri/card0", O_RDWR);
    void *cookie = (void *)(intptr_t)drm_fd;

    // Create device instance
    vaccel_create(cookie);
    xvdna_info("Device created successfully");

    // Query capset information
    uint32_t max_version = 0;
    uint32_t max_size = 0;
    if (vaccel_get_capset_info(cookie, VIRTGPU_DRM_CAPSET_DRM, 
                                &max_version, &max_size) == 0) {
        xvdna_info("Capset max version: %u, max size: %u", 
                   max_version, max_size);
    }

    // Create context
    vaccel_context_create(cookie, 1, "main_context");
    xvdna_dbg("Context 1 created");

    // Create resource (1 MB buffer)
    vaccel_resource_create(cookie, 1, 1024 * 1024, 0);
    xvdna_dbg("Resource 1 created (1 MB)");

    // Submit commands
    uint8_t cmd[] = { /* command buffer */ };
    vaccel_submit_ccmd(cookie, 1, cmd, sizeof(cmd));

    // Submit fence
    vaccel_submit_fence(cookie, 1, 100, 0);

    // Get fence FD
    int fence_fd = vaccel_get_fence_fd(cookie, 100);

    // Cleanup happens automatically on library unload
    vaccel_destroy(cookie);
    close(drm_fd);

    xvdna_info("Cleanup complete");
    return 0;
}
```

## Implementation Checklist

- ✅ Cookie-based device management
- ✅ Global device lookup table
- ✅ Per-device resource tables (no external dependencies)
- ✅ Per-device context tables (no external dependencies)
- ✅ Per-device fence tables with 64-bit IDs (no external dependencies)
- ✅ DRM backend for resource/context creation
- ✅ Command submission API
- ✅ Fence timeline synchronization
- ⚠️  Driver-specific command submission (stub implementation)
- ⚠️  Sync file integration (eventfd placeholder)

## License

This project uses a dual-license approach:

- **Apache License 2.0** - Main project license (see `LICENSE`)
- **MIT License** - Files derived from virglrenderer (see `COPYING.MIT`)

For detailed licensing information, see `LICENSING.md`.

### Quick Summary

- Most files: Apache 2.0
- `src/vaccel_fence.c`, `src/vaccel_resource.c`, `src/vaccel_context.c`: MIT (virglrenderer-derived)

**Virglrenderer-derived files** are based on code from:
https://gitlab.freedesktop.org/virgl/virglrenderer

Copyright (c) 2025 Advanced Micro Devices, Inc.

## References

- [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) - Inspiration for architecture
- [libdrm](https://gitlab.freedesktop.org/mesa/drm) - DRM interface library
