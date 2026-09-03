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
#   ./build/build-yocto.sh [-b <yocto-build-dir>] [-m] [-s] [-c] [-h]
#     -b   path to the Yocto build dir (the one containing tmp/, conf/, ...)
#     -m   build only the amdxdna kernel module
#     -s   build only the userspace packages (shim + shim tests + xrt runtime)
#     -c   clean build artifacts first
#     (no flag) build both
#
# The Yocto build location can also be given via the YOCTO_BUILD env var, and
# otherwise falls back to the built-in default:
#   YOCTO_BUILD=/path/to/yocto/build ./build/build-yocto.sh
#   ./build/build-yocto.sh -b /path/to/yocto/build

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration - adjust here if your layout changes.
# ---------------------------------------------------------------------------
# Root of this xdna-driver checkout (this script lives in build/, so go up one).
SRC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Yocto build directory (the one containing tmp/, conf/, ...).
YOCTO_BUILD="${YOCTO_BUILD:-/scratch/wendy/t20/Vitis-AI-Telluride/versal_2ve/reference_design/vek385/rev-b/sw/yocto/build}"

# MACHINE used for the kernel / rootfs, and the target userspace tune.
KERNEL_MACHINE="amd-cortexa78-mali-common"          # kernel module tune
USERSPACE_TUNE="cortexa72-cortexa53-amd-linux"      # target userspace tune

# amdxdna driver knobs. This script always builds the platform (non-PCI) aie4
# variant; Kbuild selects it via CONFIG_DRM_ACCEL_AMDXDNA_PLAT=y (passed to the
# driver make below). XDNA_BUS_TYPE is retained only to gate the local
# config_kernel.h generation step.
XDNA_BUS_TYPE="platform"
XDNA_DRIVER_VERSION="2.23.0"

# Where the built artifacts are collected (inside the build dir, so `clean`
# wipes it too and it stays out of the source tree top level).
OUT_DIR="${SRC_ROOT}/build-yocto/out"

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

  # Choose KBUILD_OUTPUT carefully to avoid a vermagic mismatch.
  #
  # The shared kernel-build-artifacts ($KART) can carry a spurious '+' in its
  # kernel release (e.g. '6.18.10-xilinx+'): Yocto's do_shared_workdir
  # regenerates utsrelease.h without the kernel's .scmversion, so
  # scripts/setlocalversion appends '+' for an untagged tree.  A module built
  # against it gets vermagic '6.18.10-xilinx+', which the deployed kernel
  # ('6.18.10-xilinx') rejects at insmod:
  #   version magic '...-xilinx+ ...' should be '...-xilinx ...'
  #
  # The linux-xlnx recipe's own standard-build dir has the CORRECT (no '+')
  # utsrelease.h -- the exact build that produced the deployed Image -- plus a
  # matching Module.symvers.  Prefer it; fall back to $KART otherwise.
  local KREC KOUT KREL
  KREC="$(ls -d ${TMP}/work/amd_cortexa78_mali_common-amd-linux/linux-xlnx/*/linux-*-standard-build 2>/dev/null | head -1)"
  if [ -n "${KREC}" ] && [ -f "${KREC}/include/generated/utsrelease.h" ] \
     && [ -f "${KREC}/Module.symvers" ]; then
    KOUT="${KREC}"
    log "KBUILD_OUTPUT = linux-xlnx recipe build dir (matches deployed Image)"
  else
    KOUT="${KART}"
    warn "recipe kernel build dir not found; falling back to shared artifacts:"
    warn "  ${KART}"
    warn "  module vermagic may get a spurious '+'; if insmod complains, rebuild"
    warn "  the kernel (bitbake virtual/kernel) and re-run this script."
  fi
  KREL="$(cat "${KOUT}/include/config/kernel.release" 2>/dev/null || true)"
  log "target kernel release (expected vermagic): ${KREL:-unknown}"

  # Cross toolchain on PATH; split source/output kernel via KBUILD_OUTPUT.
  export PATH="${MOD_NATIVE}/usr/bin/aarch64-amd-linux:${PATH}"
  export KBUILD_OUTPUT="${KOUT}"

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

  # The platform build needs config_kernel.h.  The Makefile can auto-generate it,
  # but here we generate the header into the MIRROR (never the source) with the
  # correct script and cross env (configure_kernel.sh compiles conftest modules,
  # honoring ARCH/CROSS_COMPILE/KBUILD_OUTPUT from the environment).  With the
  # header present, the Makefile skips its own generation step.
  if [ "$XDNA_BUS_TYPE" = "platform" ]; then
    log "Generating config_kernel.h for platform build (in build-yocto/kmod)"
    ARCH=arm64 CROSS_COMPILE="${CROSS_PREFIX}" KERNEL_SRC="${KSRC}" \
      OUT="${drvdir}/config_kernel.h" \
      sh "${SRC_ROOT}/drivers/accel/tools/configure_kernel.sh"

    # aie.c calls hmm_range_fault() unconditionally, but that symbol only
    # exists when the kernel is built with CONFIG_HMM_MIRROR (mm/hmm.c).  When
    # the kernel lacks it, provide a weak fake in the (generated, mirror-only)
    # config_kernel.h so the module still links; the userptr/HMM fault path
    # then returns an error instead of resolving pages.  Source tree untouched.
    if ! grep -q '^CONFIG_HMM_MIRROR=y' "${KOUT}/.config" 2>/dev/null; then
      log "kernel has no CONFIG_HMM_MIRROR; adding fake hmm_range_fault to config_kernel.h"
      cat >> "${drvdir}/config_kernel.h" <<'EOF'

/* Kernel built without CONFIG_HMM_MIRROR: hmm_range_fault is not compiled/
 * exported.  Provide a weak stub so amdxdna links (HMM fault path no-ops). */
#ifndef CONFIG_HMM_MIRROR
#include <linux/errno.h>
struct hmm_range;
long hmm_range_fault(struct hmm_range *range);
__attribute__((weak)) long hmm_range_fault(struct hmm_range *range)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_HMM_MIRROR */
EOF
    fi
  fi

  # Build in-place in the mirror via the driver Makefile's `modules` target
  # (the `all` target would create a build/ dir inside the mirror's driver dir;
  # `modules` builds straight in drvdir, which is already out-of-source).
  make -C "${drvdir}" modules \
    KERNEL_SRC="${KSRC}" \
    ARCH=arm64 CROSS_COMPILE="${CROSS_PREFIX}" \
    CONFIG_DRM_ACCEL_AMDXDNA_PLAT=y \
    XDNA_DRIVER_VERSION="${XDNA_DRIVER_VERSION}" \
    -j"$(nproc)"

  local ko
  ko="$(find "${moddir}" -name amdxdna.ko -type f | head -1)"
  [ -n "$ko" ] || die "amdxdna.ko was not produced"
  mkdir -p "${OUT_DIR}"
  cp -f "$ko" "${OUT_DIR}/amdxdna.ko"
  log "amdxdna.ko -> ${OUT_DIR}/amdxdna.ko"

  # Sanity: the module's vermagic must match the deployed kernel's release, or
  # insmod rejects it ("version magic ... should be ...").  Compare and warn.
  if command -v modinfo >/dev/null 2>&1; then
    local vm
    vm="$(modinfo -F vermagic "${OUT_DIR}/amdxdna.ko" 2>/dev/null | awk '{print $1}')"
    if [ -n "${vm}" ] && [ -n "${KREL}" ] && [ "${vm}" != "${KREL}" ]; then
      warn "vermagic mismatch: module '${vm}' vs kernel '${KREL}'"
      warn "  the .ko will NOT load on a '${KREL}' kernel."
    else
      log "vermagic OK: ${vm:-unknown} (matches kernel ${KREL:-unknown})"
    fi
  fi
  unset KBUILD_OUTPUT
}

# ---------------------------------------------------------------------------
# Userspace packages: shim (libxrt_driver_xdna.so), shim tests
# (shim_test.elf / xrt_test.elf) and the xrt runtime (xrt-smi + core libs).
# ---------------------------------------------------------------------------
build_shim_test() {
  log "Building shim + shim tests against Yocto rootfs sysroot"
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
    -DXDNA_UMQ_CACHE_NONCOHERENT=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev \
    -S "${SRC_ROOT}" -B "${bdir}"

  # Build the shim (xrt_driver_xdna) and its private xrt_core/xrt_coreutil, plus
  # the two test ELFs.  Building the ELFs alone would pull the libs in as deps,
  # but naming them explicitly keeps -s meaningful if the tests are ever dropped.
  make -C "${bdir}" -j"$(nproc)" \
    xrt_coreutil xrt_core xrt_driver_xdna shim_test.elf xrt_test.elf

  # xrt-smi (xbutil2) is NOT reachable from the xdna-driver top-level build:
  # CMake/xrt.cmake lists src/runtime_src/core/tools in XRT_EXCLUDE_SUB_DIRECTORY,
  # so the target never gets configured.  That exclusion list is set ONLY by the
  # xdna-driver wrapper -- configuring the xrt submodule (xrt/src) directly leaves
  # it empty, so the tools subdir (and xrt-smi) is included.  Do that in a second,
  # isolated build dir with the SAME cross toolchain, so xrt-smi links the same
  # xrt version as the core libs deployed above.  Best-effort: a failure here must
  # not sink the shim drop, which is the primary artifact.
  local xsdir="${bdir}/xrt-smi"
  log "Building xrt-smi from xrt submodule (xrt/src, tools included)"
  if cmake -G 'Unix Makefiles' -DCMAKE_MAKE_PROGRAM=make \
       -DCMAKE_TOOLCHAIN_FILE="${SHIM_TOOLCHAIN}" \
       -DCMAKE_INSTALL_PREFIX:PATH=/usr \
       -DCMAKE_NO_SYSTEM_FROM_IMPORTED=1 \
       -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE \
       -DXRT_NPU=1 \
       -DXDP_CLIENT_BUILD_CMAKE=yes \
       -DCMAKE_BUILD_TYPE=Release \
       -Wno-dev \
       -S "${SRC_ROOT}/xrt/src" -B "${xsdir}" \
     && make -C "${xsdir}" -j"$(nproc)" xrt-smi; then
    log "xrt-smi built under ${xsdir}"
  else
    warn "xrt-smi build failed -- OUT_DIR/xrt will ship without xrt-smi"
  fi

  # Collect the deployable userspace set into OUT_DIR, grouped per component so
  # it mirrors the Yocto packages (xdna-shim / xdna-shim-test / xrt):
  #   shim/       libxrt_driver_xdna.so*
  #   shim_test/  shim_test.elf xrt_test.elf xrt_test
  #   xrt/        libxrt_core.so* libxrt_coreutil.so* xrt-smi
  # The core libs are taken from THIS build tree (${bdir}) so they match the
  # locally-built shim (else wrong vtable slot -> SIGSEGV).  xrt-smi is built
  # separately from the xrt submodule (see ${xsdir} above) against the same xrt
  # version.  cp -a preserves the .so soname symlink chain.
  mkdir -p "${OUT_DIR}/shim" "${OUT_DIR}/shim_test" "${OUT_DIR}/xrt"

  cp -a "${bdir}"/src/shim/libxrt_driver_xdna.so* "${OUT_DIR}/shim/"

  cp -f "${bdir}/test/shim_test/shim_test.elf" "${OUT_DIR}/shim_test/shim_test.elf"
  cp -f "${bdir}/test/xrt_test/xrt_test.elf"   "${OUT_DIR}/shim_test/xrt_test.elf"
  [ -f "${bdir}/test/xrt_test/xrt_test" ] && \
    cp -f "${bdir}/test/xrt_test/xrt_test" "${OUT_DIR}/shim_test/xrt_test" || true

  # xrt core libs are freshly built under ${bdir}/xrt.  The exact subdir varies
  # (core/common, core/pcie/linux, ...), so locate each by name; for the libs
  # copy the whole .so/.so.2/.so.X.Y chain.
  rm -f "${OUT_DIR}/xrt"/libxrt_core.so* "${OUT_DIR}/xrt"/libxrt_coreutil.so* "${OUT_DIR}/xrt"/xrt-smi
  local built_core built_coreutil built_smi
  built_core="$(find "${bdir}/xrt" -name 'libxrt_core.so.*.*' -type f 2>/dev/null | head -1)"
  built_coreutil="$(find "${bdir}/xrt" -name 'libxrt_coreutil.so.*.*' -type f 2>/dev/null | head -1)"

  if [ -n "${built_core}" ]; then
    cp -a "$(dirname "${built_core}")"/libxrt_core.so* "${OUT_DIR}/xrt/"
  else
    warn "built libxrt_core not found under ${bdir}/xrt -- shim will ABI-mismatch"
  fi
  if [ -n "${built_coreutil}" ]; then
    cp -a "$(dirname "${built_coreutil}")"/libxrt_coreutil.so* "${OUT_DIR}/xrt/"
  else
    warn "built libxrt_coreutil not found under ${bdir}/xrt -- shim will ABI-mismatch"
  fi

  # xrt-smi from the dedicated xrt/src build above (${xsdir}).
  built_smi="$(find "${xsdir}" -name 'xrt-smi' -type f 2>/dev/null | head -1)"
  if [ -n "${built_smi}" ]; then
    cp -f "${built_smi}" "${OUT_DIR}/xrt/"
  else
    warn "xrt-smi binary not found under ${xsdir} (xrt-smi build step failed?)"
  fi

  log "shim      -> ${OUT_DIR}/shim/       (libxrt_driver_xdna.so)"
  log "shim_test -> ${OUT_DIR}/shim_test/  (shim_test.elf, xrt_test.elf)"
  log "xrt       -> ${OUT_DIR}/xrt/        (libxrt_core.so, libxrt_coreutil.so, xrt-smi -- all built from this checkout's xrt)"
}

do_clean() {
  log "Cleaning"
  rm -rf "${SRC_ROOT}/src/driver/amdxdna/build" "${SRC_ROOT}/build-yocto" "${OUT_DIR}"
}

# ---------------------------------------------------------------------------
# Arg parsing.
# ---------------------------------------------------------------------------
do_mod=0; do_shim=0; do_cln=0
while getopts "b:msch" opt; do
  case "$opt" in
    b) YOCTO_BUILD="$OPTARG" ;;
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
ls -lR "${OUT_DIR}" 2>/dev/null || true
