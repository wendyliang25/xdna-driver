// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include "drm/amdxdna_accel.h"

#include "aie4_plat.h"
#include "amdxdna_drv.h"

/*
 * T20 SoC variant: aie4 firmware over the platform shmem+IPI transport on
 * aie2ps ("ve2") hardware.  Functionally it speaks the aie4 message protocol
 * like the PCI npu3/aie4 parts, but it is not a PCI device -- it is described
 * in the device tree ("amd,amdxdna") and reaches firmware through shared
 * memory.  The underlying AIE silicon matches Versal AIE2 (ve2).
 *
 * A distinct vbnv ("RyzenAI-npu3-aie2ps") lets XRT dispatch to the npu3_aie2ps
 * hardware_type (ve2 ELFs / xrt_smi_ve2.a) instead of treating this as a
 * regular PCI npu3 device.  The PCI-only fields (bars) are left unset; the
 * platform driver drives this device.
 */
const struct amdxdna_dev_info dev_npu3b_info = {
	.default_vbnv	= "RyzenAI-npu3-aie2ps",
	.device_type	= AMDXDNA_DEV_TYPE_UMQ,
	.ops		= &aie4_plat_ops,
};
