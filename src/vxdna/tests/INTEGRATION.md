# vxdna Unit Tests Integration Summary

This document summarizes the integration of vxdna unit tests into the xdna-driver build system.

## What Was Added

### 1. Unit Test Suite

Created comprehensive unit tests for `vaccel_renderer.h` API in the `src/vxdna/tests/` directory:

- **test_vaccel_renderer.cpp** (592 lines)
  - 35+ test cases covering all public APIs
  - Tests for vaccel_create(), vaccel_destroy(), vaccel_get_capset_info(), vaccel_fill_capset()
  - Thread safety tests
  - Multi-device tests
  - Edge case and error handling tests

- **test_helper.h** / **test_helper.cpp** (144 lines)
  - DRM device management utilities
  - Real DRM file descriptor handling
  - Test fixture base class
  - Helper functions for test setup/teardown

- **CMakeLists.txt** (63 lines)
  - Test build configuration
  - Google Test integration
  - CTest support

- **README.md**
  - Comprehensive test documentation
  - Usage instructions
  - Troubleshooting guide

- **run_tests.sh** (96 lines)
  - Standalone test build and run script
  - DRM device detection
  - Dependency checking

### 2. Build System Integration

#### Modified Files:

**build/build.sh**
- Added `-test` flag to enable test building and running
- Added `run_vxdna_tests()` function to execute tests after build
- Integrated test execution into build workflow
- Added `BUILD_TESTING=ON` CMake option when `-test` flag is used

**src/vxdna/CMakeLists.txt**
- Added conditional `add_subdirectory(tests)` when `BUILD_TESTING` is enabled
- Tests are built automatically when both `-vxdna` and `-test` flags are used

**tools/amdxdna_deps.sh**
- Added Google Test installation for Ubuntu/Debian (libgtest-dev)
- Added Google Test installation for Fedora/RHEL (gtest-devel)

### 3. Documentation

**TESTING.md** (at project root)
- Comprehensive testing guide
- Prerequisites and system requirements
- Build instructions
- Running tests (multiple methods)
- Troubleshooting section
- Advanced usage (Valgrind, GDB, coverage)
- CI/CD integration examples

## How to Use

### Quick Start

```bash
cd build

# Build vxdna library with tests and run them
./build.sh -vxdna -test

# Or for release build
./build.sh -vxdna -test -release
```

### Install Dependencies

```bash
sudo ./tools/amdxdna_deps.sh
```

This will install:
- jq
- Google Test
- All XRT dependencies

### Running Tests Manually

After building with tests:

```bash
# From build directory
cd Debug/src/vxdna/tests  # or Release/src/vxdna/tests
./vaccel_renderer_tests

# Or use the standalone script
cd ../../vxdna/tests
./run_tests.sh
```

## Test Requirements

### System Requirements
- Linux operating system
- At least one DRM device (`/dev/dri/card*` or `/dev/dri/renderD*`)
- Permissions to access DRM devices (user in `video` group)

### Software Requirements
- CMake 3.10+
- C++17 compatible compiler (GCC 7+, Clang 5+)
- Google Test library
- vxdna library (built with `-vxdna` flag)

### DRM Device Access

The tests use **real DRM file descriptors** from `/dev/dri/` devices:
- Prefers render nodes (`renderD*`) for compute workloads
- Falls back to card devices (`card*`) if needed
- Tests skip gracefully if no DRM devices are available

## Test Coverage

The test suite covers:

1. **Device Management**
   - Device creation with various parameters
   - Device destruction
   - Multiple device instances
   - Cookie-based device identification

2. **Capability Sets**
   - Capability set information queries
   - Capability set data retrieval
   - Structure validation

3. **Error Handling**
   - NULL pointer validation
   - Invalid parameter detection
   - Non-existent device handling
   - Duplicate device prevention

4. **Thread Safety**
   - Concurrent create/destroy operations
   - Race condition testing
   - Multi-threaded access

5. **Edge Cases**
   - Zero-size buffers
   - Large buffers
   - Partial parameter sets
   - Multiple operations

## Build Flags

### New Flag: `-test`

The `-test` flag has been added to `build/build.sh`:

```bash
./build.sh -vxdna -test
```

This flag:
1. Sets `BUILD_TESTING=ON` in CMake
2. Builds the vxdna tests
3. Runs tests automatically after build
4. Reports test results

**Note:** `-test` requires `-vxdna` flag (tests depend on vxdna library)

### Existing Relevant Flags

- `-vxdna` - Build vxdna renderer library (required for tests)
- `-debug` - Build debug version (default, includes debug symbols)
- `-release` - Build release version
- `-j <n>` - Parallel build with n jobs
- `-verbose` - Verbose build output
- `-clean` - Clean build directory

### Example Combinations

```bash
# Debug build with tests
./build.sh -vxdna -test

# Release build with tests
./build.sh -vxdna -test -release

# Debug build with verbose output and tests
./build.sh -vxdna -test -verbose

# Parallel build with 8 jobs
./build.sh -vxdna -test -j 8

# Clean and rebuild with tests
./build.sh -clean
./build.sh -vxdna -test
```

## CI/CD Integration

For continuous integration:

```bash
#!/bin/bash
# CI test script

# Install dependencies
sudo ./tools/amdxdna_deps.sh

# Build and test
cd build
./build.sh -vxdna -test -release

# Check exit code
if [ $? -ne 0 ]; then
    echo "Tests failed!"
    exit 1
fi

echo "All tests passed!"
```

## Test Results

When tests run, you'll see output like:

```
========================================
Running vxdna unit tests (Debug)
========================================

Opened DRM render node: /dev/dri/renderD128 (fd=3)
[==========] Running 35 tests from 2 test suites.
[----------] Global test environment set-up.
[----------] 3 tests from DrmHelperTest
[ RUN      ] DrmHelperTest.DrmDeviceAvailable
[       OK ] DrmHelperTest.DrmDeviceAvailable (0 ms)
...
[----------] 32 tests from VaccelRendererTest
[ RUN      ] VaccelRendererTest.CreateDeviceSuccess
[       OK ] VaccelRendererTest.CreateDeviceSuccess (5 ms)
...
[==========] 35 tests from 2 test suites ran. (234 ms total)
[  PASSED  ] 35 tests.

========================================
vxdna unit tests PASSED
========================================
```

## Files Added/Modified

### New Files (in src/vxdna/tests/)
- `test_vaccel_renderer.cpp` - Main test file
- `test_helper.h` - Helper header
- `test_helper.cpp` - Helper implementation
- `CMakeLists.txt` - Test build configuration
- `README.md` - Test documentation
- `run_tests.sh` - Standalone test script
- `INTEGRATION.md` - This file

### Modified Files
- `build/build.sh` - Added `-test` flag and test execution
- `src/vxdna/CMakeLists.txt` - Added test subdirectory
- `tools/amdxdna_deps.sh` - Added Google Test dependency
- `TESTING.md` (new at root) - Comprehensive testing guide

## Architecture

```
xdna-driver/
├── build/
│   └── build.sh              (Modified: Added -test flag)
├── tools/
│   └── amdxdna_deps.sh       (Modified: Added gtest)
├── src/
│   └── vxdna/
│       ├── CMakeLists.txt    (Modified: Added tests subdirectory)
│       ├── include/
│       │   └── vaccel_renderer.h  (API being tested)
│       └── tests/            (NEW DIRECTORY)
│           ├── CMakeLists.txt
│           ├── test_vaccel_renderer.cpp
│           ├── test_helper.h
│           ├── test_helper.cpp
│           ├── README.md
│           ├── run_tests.sh
│           └── INTEGRATION.md
└── TESTING.md                (NEW: Root-level testing guide)
```

## Future Enhancements

Potential improvements:

1. **Code Coverage Reports**
   - Integrate lcov/gcov for coverage analysis
   - Generate HTML coverage reports
   - Set minimum coverage thresholds

2. **Performance Benchmarks**
   - Add performance tests using Google Benchmark
   - Track API call latencies
   - Monitor memory usage

3. **Mock DRM Devices**
   - Create mock DRM device for testing without hardware
   - Enable tests in environments without GPU/NPU

4. **Additional Test Suites**
   - Add tests for internal APIs
   - Add integration tests with real NPU hardware
   - Add stress tests for long-running operations

5. **Automated CI**
   - GitHub Actions workflow
   - Automatic test execution on PR
   - Test result reporting

## References

- Main project README: `README.md`
- Testing guide: `TESTING.md`
- Test-specific README: `src/vxdna/tests/README.md`
- Google Test docs: https://google.github.io/googletest/

## License

Copyright (c) 2025 Advanced Micro Devices, Inc.
SPDX-License-Identifier: Apache-2.0

