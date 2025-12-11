# vaccel_renderer Unit Tests

This directory contains comprehensive unit tests for the `vaccel_renderer.h` API.

## Overview

The test suite covers all public APIs exposed in `vaccel_renderer.h`:

- **vaccel_create()** - Device creation with cookie-based identification
- **vaccel_destroy()** - Device destruction and cleanup
- **vaccel_get_capset_info()** - Capability set information retrieval
- **vaccel_fill_capset()** - Capability set data filling

## Test Framework

The tests use **Google Test (gtest)**, an open-source C++ testing framework.

## Requirements

### System Requirements

- **Linux only** - Tests use real DRM devices (`/dev/dri/`)
- DRM kernel modules loaded
- At least one DRM device available (GPU, render node, or similar)
- Permissions to access `/dev/dri/` devices

### Build Requirements

- CMake 3.10 or later
- C++17 compatible compiler
- Google Test library
- vxdna library

## Building Tests

### Option 1: Build with CMake (Recommended)

From the vxdna build directory:

```bash
# Enable tests during CMake configuration
cd build
cmake .. -DBUILD_TESTING=ON
make

# Run tests
ctest --verbose
# or
./tests/vaccel_renderer_tests
```

### Option 2: Build tests separately

```bash
cd tests
mkdir build
cd build
cmake ..
make

# Run tests
./vaccel_renderer_tests
```

## Installing Google Test

If Google Test is not installed on your system:

### Ubuntu/Debian
```bash
sudo apt-get install libgtest-dev
cd /usr/src/gtest
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib
```

### Fedora/RHEL
```bash
sudo dnf install gtest-devel
```

### Build from source
```bash
git clone https://github.com/google/googletest.git
cd googletest
mkdir build && cd build
cmake ..
make
sudo make install
```

## Running Tests

### Run all tests
```bash
./vaccel_renderer_tests
```

### Run specific test suite
```bash
./vaccel_renderer_tests --gtest_filter=VaccelRendererTest.*
```

### Run specific test
```bash
./vaccel_renderer_tests --gtest_filter=VaccelRendererTest.CreateDeviceSuccess
```

### List all available tests
```bash
./vaccel_renderer_tests --gtest_list_tests
```

### Run with verbose output
```bash
./vaccel_renderer_tests --gtest_print_time=1
```

## Test Structure

### Test Files

- **test_vaccel_renderer.cpp** - Main test cases for all vaccel_renderer APIs
- **test_helper.h** - Helper utilities and test fixture definitions
- **test_helper.cpp** - Helper implementations (DRM device management, etc.)

### Test Categories

1. **DRM Device Tests** - Verify DRM device availability and access
2. **vaccel_create() Tests** - Device creation, error handling, validation
3. **vaccel_destroy() Tests** - Device destruction and cleanup
4. **vaccel_get_capset_info() Tests** - Capability set info retrieval
5. **vaccel_fill_capset() Tests** - Capability set data operations
6. **Multi-Device Tests** - Multiple device management
7. **Thread Safety Tests** - Concurrent operation testing
8. **Edge Case Tests** - Boundary conditions and error scenarios
9. **Integration Tests** - Full workflow validation

## Test Features

### Real DRM Device Access

The tests use **real DRM file descriptors** obtained from `/dev/dri/` devices:

- Attempts to open render nodes first (`/dev/dri/renderD*`)
- Falls back to card devices (`/dev/dri/card*`)
- Provides helpful error messages if no devices are found
- Automatically skips tests if DRM devices are unavailable

### Comprehensive Coverage

- **Positive tests** - Verify correct operation
- **Negative tests** - Verify error handling
- **Edge cases** - Boundary conditions, NULL pointers, invalid parameters
- **Thread safety** - Concurrent operations
- **Multi-device** - Multiple device instances

### Test Output

Tests provide detailed output including:
- DRM device information
- Capability set details
- Error messages with context
- Performance timing (with `--gtest_print_time=1`)

## Example Test Output

```
[==========] Running 35 tests from 2 test suites.
[----------] Global test environment set-up.
[----------] 3 tests from DrmHelperTest
[ RUN      ] DrmHelperTest.DrmDeviceAvailable
[       OK ] DrmHelperTest.DrmDeviceAvailable (0 ms)
[ RUN      ] DrmHelperTest.ListDrmDevices
Available DRM devices:
  - /dev/dri/card0
  - /dev/dri/renderD128
[       OK ] DrmHelperTest.ListDrmDevices (1 ms)
[ RUN      ] DrmHelperTest.OpenDrmDevice
Opened DRM render node: /dev/dri/renderD128 (fd=3)
[       OK ] DrmHelperTest.OpenDrmDevice (2 ms)
...
```

## Troubleshooting

### No DRM devices found

```
ERROR: No DRM devices found in /dev/dri/
Please ensure:
  1. DRM kernel modules are loaded
  2. You have permissions to access /dev/dri/ devices
  3. A GPU or render device is available on the system
```

**Solutions:**
- Check if `/dev/dri/` exists: `ls -la /dev/dri/`
- Load DRM modules: `sudo modprobe drm`
- Add user to video group: `sudo usermod -a -G video $USER`
- Check device permissions: `ls -la /dev/dri/*`

### Tests skipped

If you see many tests skipped, it usually means:
- No DRM devices available (expected behavior - tests skip gracefully)
- Permission denied accessing DRM devices

### Build errors

- Ensure Google Test is installed
- Check that vxdna library is built first
- Verify C++17 support in your compiler

## Contributing

When adding new tests:

1. Follow existing test naming conventions
2. Use the `VaccelRendererTest` fixture for device management
3. Add documentation comments for complex test scenarios
4. Ensure tests clean up resources properly
5. Handle missing DRM devices gracefully with `GTEST_SKIP()`

## License

Copyright (c) 2025 Advanced Micro Devices, Inc.
SPDX-License-Identifier: Apache-2.0

