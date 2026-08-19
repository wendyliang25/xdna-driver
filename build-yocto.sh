#!/bin/bash
#
# build-yocto.sh - Build amdxdna.ko and the shim/xrt tests from THIS xdna-driver
# checkout, targeting a Yocto (edf) build for the VEK385 (versal-2ve-2vm).
#
#   * amdxdna.ko  -> built against the Yocto Linux kernel (out-of-tree module)
#   * shim_test.elf / xrt_test.elf -> cross-compiled against the Yocto target
#     rootfs sysroot, so they run on the image this Yocto build produced.
#
# It reuses the cross toolchain, kernel-build-artifacts and recipe sysroots that
# the Yocto build already produced - no SDK required.
#
# Usage:
#   ./build-yocto.sh [-m] [-s] [-c] [-h]
#     -m   build only the amdxdna kernel module
#     -s   build only the shim/xrt tests
#     -c   clean build artifacts first
#     (no flag) build both
#
# Override the Yocto build location if it is not the default:
#   YOCTO_BUILD=/path/to/yocto/build ./build-yocto.sh

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration - adjust here if your layout changes.
# ---------------------------------------------------------------------------
# Root of this xdna-driver checkout (dir containing this script).
SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Yocto build directory (the one containing tmp/, conf/, ...).
YOCTO_BUILD="${YOCTO_BUILD:-/scratch/wendy/t20/Vitis-AI-Telluride/versal_2ve/reference_design/vek385/rev-b/sw/yocto/build}"

# MACHINE used for the kernel / rootfs, and the target userspace tune.
KERNEL_MACHINE="amd-cortexa78-mali-common"          # kernel module tune
USERSPACE_TUNE="cortexa72-cortexa53-amd-linux"      # target userspace tune

# amdxdna driver knobs. The upstream drivers/accel/amdxdna tree uses "of" for
# the platform/OF (non-PCI) build (its Makefile only knows pci|of, not npu_of).
XDNA_BUS_TYPE="of"
XDNA_DRIVER_VERSION="2.23.0"

# Where the built artifacts are collected.
OUT_DIR="${SRC_ROOT}/build-yocto-out"

# ---------------------------------------------------------------------------
# Derived paths.
# ---------------------------------------------------------------------------
TMP="${YOCTO_BUILD}/tmp"
# Kernel: split source tree + KBUILD_OUTPUT (build artifacts).
KSRC="${TMP}/work-shared/${KERNEL_MACHINE}/kernel-source"
KART="${TMP}/work-shared/${KERNEL_MACHINE}/kernel-build-artifacts"
# Cross toolchain that built the kernel module (aarch64-amd-linux-*).
MOD_NATIVE="${TMP}/work/amd_cortexa78_mali_common-amd-linux/amdxdna/git/recipe-sysroot-native"
# Shim test: target sysroot + native tools + generated cmake toolchain file.
SHIM_W="${TMP}/work/${USERSPACE_TUNE}/xdna-shim-test/git"
SHIM_SYSROOT="${SHIM_W}/recipe-sysroot"
SHIM_NATIVE="${SHIM_W}/recipe-sysroot-native"
SHIM_TOOLCHAIN="${SHIM_W}/toolchain.cmake"

CROSS_PREFIX="aarch64-amd-linux-"

# ---------------------------------------------------------------------------
# Helpers.
# ---------------------------------------------------------------------------
log()  { echo -e "\033[1;32m[build-yocto]\033[0m $*"; }
warn() { echo -e "\033[1;33m[build-yocto]\033[0m $*" >&2; }
die()  { echo -e "\033[1;31m[build-yocto] ERROR:\033[0m $*" >&2; exit 1; }

need_dir() { [ -d "$1" ] || die "$2 not found: $1"; }

# ---------------------------------------------------------------------------
# Kernel module: amdxdna.ko
# ---------------------------------------------------------------------------
build_module() {
  log "Building amdxdna.ko against Yocto kernel ($KERNEL_MACHINE)"
  need_dir "$KSRC" "kernel-source (build the Yocto image first)"
  need_dir "$KART" "kernel-build-artifacts (build the Yocto image first)"
  need_dir "$MOD_NATIVE/usr/bin/aarch64-amd-linux" \
    "module cross toolchain (run: bitbake amdxdna -c prepare_recipe_sysroot)"

  # Cross toolchain on PATH; split source/output kernel via KBUILD_OUTPUT.
  export PATH="${MOD_NATIVE}/usr/bin/aarch64-amd-linux:${PATH}"
  export KBUILD_OUTPUT="${KART}"

  # Build entirely out-of-source: mirror the driver + include/ into
  # build-yocto/kmod and build there, so the source tree
  # (drivers/accel/amdxdna) is never written to.  The driver Kbuild expects
  # the uAPI headers at ../../../include relative to the driver dir, so the
  # mirror preserves the drivers/accel/amdxdna + include layout.
  local moddir="${SRC_ROOT}/build-yocto/kmod"
  local drvdir="${moddir}/drivers/accel/amdxdna"
  rm -rf "${moddir}"
  mkdir -p "${moddir}/drivers/accel"
  cp -a "${SRC_ROOT}/drivers/accel/amdxdna" "${moddir}/drivers/accel/"
  cp -a "${SRC_ROOT}/include" "${moddir}/include"
  # Drop anything that would let the build reach into / mimic the source tree.
  rm -rf "${drvdir}/build" "${drvdir}/config_kernel.h"
  find "${drvdir}" -maxdepth 1 -name '*.o' -o -maxdepth 1 -name '*.ko' -delete 2>/dev/null || true

  # OF build needs config_kernel.h.  The Makefile's auto-gen rule uses a wrong
  # relative path to configure_kernel.sh for this layout, so generate the header
  # into the MIRROR (never the source) with the correct script and cross env
  # (configure_kernel.sh compiles conftest modules, honoring ARCH/CROSS_COMPILE/
  # KBUILD_OUTPUT from the environment).  With the header present, the Makefile
  # skips its (broken) generation step.
  if [ "$XDNA_BUS_TYPE" = "of" ]; then
    log "Generating config_kernel.h for OF build (in build-yocto/kmod)"
    ARCH=arm64 CROSS_COMPILE="${CROSS_PREFIX}" KERNEL_SRC="${KSRC}" \
      OUT="${drvdir}/config_kernel.h" \
      sh "${SRC_ROOT}/drivers/accel/tools/configure_kernel.sh"
  fi

  # Build in-place in the mirror via the driver Makefile's `modules` target
  # (the `all` target would create a build/ dir inside the mirror's driver dir;
  # `modules` builds straight in drvdir, which is already out-of-source).
  make -C "${drvdir}" modules \
    KERNEL_SRC="${KSRC}" \
    ARCH=arm64 CROSS_COMPILE="${CROSS_PREFIX}" \
    XDNA_BUS_TYPE="${XDNA_BUS_TYPE}" \
    XDNA_DRIVER_VERSION="${XDNA_DRIVER_VERSION}" \
    -j"$(nproc)"

  local ko
  ko="$(find "${moddir}" -name amdxdna.ko -type f | head -1)"
  [ -n "$ko" ] || die "amdxdna.ko was not produced"
  mkdir -p "${OUT_DIR}"
  cp -f "$ko" "${OUT_DIR}/amdxdna.ko"
  log "amdxdna.ko -> ${OUT_DIR}/amdxdna.ko"
  unset KBUILD_OUTPUT
}

# ---------------------------------------------------------------------------
# Userspace tests: shim_test.elf / xrt_test.elf
# ---------------------------------------------------------------------------
build_shim_test() {
  log "Building shim_test.elf / xrt_test.elf against Yocto rootfs sysroot"
  need_dir "$SHIM_SYSROOT" \
    "shim target sysroot (run: bitbake xdna-shim-test -c prepare_recipe_sysroot)"
  need_dir "$SHIM_NATIVE" "shim native sysroot"
  [ -f "$SHIM_TOOLCHAIN" ] || die "cmake toolchain file not found: $SHIM_TOOLCHAIN
Run: bitbake xdna-shim-test -c configure   (to (re)generate it)"

  # Native tools (cmake, make, cross gcc/g++) on PATH; pkg-config -> target sysroot.
  export PATH="${SHIM_NATIVE}/usr/bin/aarch64-amd-linux:${SHIM_NATIVE}/usr/bin:${SHIM_NATIVE}/usr/sbin:${SHIM_NATIVE}/bin:${TMP}/hosttools:${PATH}"
  export PKG_CONFIG_SYSROOT_DIR="${SHIM_SYSROOT}"
  export PKG_CONFIG_PATH="${SHIM_SYSROOT}/usr/lib/pkgconfig:${SHIM_SYSROOT}/usr/share/pkgconfig"
  export PKG_CONFIG_DIR="${SHIM_SYSROOT}/usr/lib/pkgconfig"

  local bdir="${SRC_ROOT}/build-yocto"
  mkdir -p "$bdir"
  git config --global --add safe.directory '*' 2>/dev/null || true

  # Mirror the xdna-shim-test recipe: SKIP_KMOD=OFF exposes test/, and we
  # restrict the make to only the two ELF targets.
  cmake -G 'Unix Makefiles' -DCMAKE_MAKE_PROGRAM=make \
    -DCMAKE_TOOLCHAIN_FILE="${SHIM_TOOLCHAIN}" \
    -DCMAKE_INSTALL_PREFIX:PATH=/usr \
    -DCMAKE_NO_SYSTEM_FROM_IMPORTED=1 \
    -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE \
    -DSKIP_KMOD=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev \
    -S "${SRC_ROOT}" -B "${bdir}"

  make -C "${bdir}" -j"$(nproc)" shim_test.elf xrt_test.elf

  mkdir -p "${OUT_DIR}"
  cp -f "${bdir}/test/shim_test/shim_test.elf" "${OUT_DIR}/shim_test.elf"
  cp -f "${bdir}/test/xrt_test/xrt_test.elf"   "${OUT_DIR}/xrt_test.elf"
  [ -f "${bdir}/test/xrt_test/xrt_test" ] && cp -f "${bdir}/test/xrt_test/xrt_test" "${OUT_DIR}/xrt_test" || true
  log "shim_test.elf / xrt_test.elf -> ${OUT_DIR}/"
}

do_clean() {
  log "Cleaning"
  rm -rf "${SRC_ROOT}/src/driver/amdxdna/build" "${SRC_ROOT}/build-yocto" "${OUT_DIR}"
}

# ---------------------------------------------------------------------------
# Arg parsing.
# ---------------------------------------------------------------------------
do_mod=0; do_shim=0; do_cln=0
while getopts "msch" opt; do
  case "$opt" in
    m) do_mod=1 ;;
    s) do_shim=1 ;;
    c) do_cln=1 ;;
    h) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
  esac
done
if [ $do_mod -eq 0 ] && [ $do_shim -eq 0 ]; then do_mod=1; do_shim=1; fi

need_dir "$YOCTO_BUILD" "YOCTO_BUILD"
log "SRC_ROOT   = ${SRC_ROOT}"
log "YOCTO_BUILD= ${YOCTO_BUILD}"

[ $do_cln  -eq 1 ] && do_clean
[ $do_mod  -eq 1 ] && build_module
[ $do_shim -eq 1 ] && build_shim_test

log "Done. Artifacts in ${OUT_DIR}"
ls -l "${OUT_DIR}" 2>/dev/null || true
