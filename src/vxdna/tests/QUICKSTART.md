# Quick Start Guide: vxdna Unit Tests

## TL;DR - Run Tests Now

```bash
# From project root
cd build

# Install dependencies (first time only)
sudo ../tools/amdxdna_deps.sh

# Build and run tests
./build.sh -vxdna -test
```

## What This Does

1. ✅ Installs Google Test (if needed)
2. ✅ Builds vxdna renderer library
3. ✅ Builds unit tests
4. ✅ Runs all 35+ test cases
5. ✅ Reports results

## Expected Output

```
========================================
Running vxdna unit tests (Debug)
========================================

Opened DRM render node: /dev/dri/renderD128 (fd=3)
[==========] Running 35 tests from 2 test suites.
...
[  PASSED  ] 35 tests.

========================================
vxdna unit tests PASSED
========================================
```

## Requirements

- ✅ Linux OS
- ✅ DRM device (`/dev/dri/card*` or `/dev/dri/renderD*`)
- ✅ Permissions (user in `video` group)
- ✅ Google Test library

## Check DRM Devices

```bash
ls -la /dev/dri/
```

If you see permission denied:
```bash
sudo usermod -a -G video $USER
# Log out and back in
```

## Build Options

### Debug Build with Tests (Default)
```bash
./build.sh -vxdna -test
```

### Release Build with Tests
```bash
./build.sh -vxdna -test -release
```

### Verbose Output
```bash
./build.sh -vxdna -test -verbose
```

### Parallel Build (faster)
```bash
./build.sh -vxdna -test -j 16
```

## Run Tests Manually

After building:

```bash
# All tests
cd Debug/src/vxdna/tests
./vaccel_renderer_tests

# Specific test
./vaccel_renderer_tests --gtest_filter=VaccelRendererTest.CreateDeviceSuccess

# List all tests
./vaccel_renderer_tests --gtest_list_tests
```

## Troubleshooting

### No DRM devices?
Tests will skip gracefully. This is normal on systems without GPU/NPU.

### Google Test not found?
```bash
# Ubuntu/Debian
sudo apt-get install libgtest-dev

# Fedora/RHEL
sudo dnf install gtest-devel
```

### Permission denied?
```bash
sudo usermod -a -G video $USER
newgrp video
```

## More Info

- Full testing guide: `/TESTING.md`
- Test details: `README.md` (this directory)
- Integration info: `INTEGRATION.md` (this directory)

## Quick Commands Reference

```bash
# Install all dependencies
sudo ../tools/amdxdna_deps.sh

# Build and test (debug)
./build.sh -vxdna -test

# Build and test (release)
./build.sh -vxdna -test -release

# Clean and rebuild
./build.sh -clean
./build.sh -vxdna -test

# Run tests only (after building)
cd Debug/src/vxdna/tests && ./vaccel_renderer_tests

# Run with Valgrind
cd Debug/src/vxdna/tests
valgrind ./vaccel_renderer_tests

# Get help
./build.sh -help
```

## Success Criteria

✅ Exit code 0
✅ "vxdna unit tests PASSED" message
✅ All tests show [  PASSED  ]

## Failure?

If tests fail:
1. Check DRM device availability: `ls /dev/dri/`
2. Check permissions: `groups` (should include `video`)
3. See full guide: `/TESTING.md`
4. Check logs for specific failures

## License

Copyright (c) 2025 Advanced Micro Devices, Inc.
SPDX-License-Identifier: Apache-2.0

