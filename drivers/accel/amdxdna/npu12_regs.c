// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include "drm/amdxdna_accel.h"
#include <linux/bits.h>

#include "aie4.h"
#include "aie4_plat.h"
#include "amdxdna_drv.h"
#include "amdxdna_plat_drv.h"

/*
 * aie2ps ("npu12") SoC variant: aie4 firmware over the platform (non-PCI)
 * transport. Functionally it speaks the aie4 message protocol like the PCI
 * npu3/aie4 parts, but it is not a PCI device -- it is described in the device
 * tree ("amd,xdna") and reaches firmware through shared memory. The
 * underlying AIE silicon matches Versal AIE2 (ve2).
 *
 * A distinct vbnv ("RyzenAI-npu12-aie2ps") lets XRT dispatch to the npu12_aie2ps
 * hardware type (ve2 ELFs) instead of treating this as a regular PCI npu3
 * device. The PCI-only fields (bars) are left unset; the platform driver drives
 * this device through aie4_plat_ops.
 */

/*
 * Firmware protocol version negotiation: advertise the base major/minimum-minor
 * the NPU firmware speaks.
 */
static const struct amdxdna_fw_feature_tbl npu12_fw_feature_table[] = {
	{ .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_GET_COREDUMP), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_RW_ACCESS), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_FW_LOG), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_FW_TRACE), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_CALIBRATE_CLOCK), .major = 6, .min_minor = 0 },
	{ 0 }
};

/*
 * CERT protocol negotiation: the expected HSA host-queue protocol version, same
 * as the PCI npu3 CERT (1.0), with AIE4_HSA_COMMAND advertised at that version.
 * The aie2ps CERT does not report its host-queue version yet (see the 0.0
 * fallback in aie4_query_cert_firmware_version()); this table is the version the
 * firmware is expected to speak once it does.
 */
static const struct amdxdna_fw_feature_tbl npu12_cert_feature_table[] = {
	{ .major = 1, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_HSA_COMMAND), .major = 1, .min_minor = 0 },
	{ 0 }
};

const struct amdxdna_dev_info dev_npu12_info = {
	.default_vbnv	= "RyzenAI-npu12-aie2ps",
	.device_type	= AMDXDNA_DEV_TYPE_UMQ,
	.partition_per_hwctx = true,
	.ops		= &aie4_plat_ops,
	.fw_feature_tbl	= npu12_fw_feature_table,
	.cert_feature_tbl = npu12_cert_feature_table,
};
