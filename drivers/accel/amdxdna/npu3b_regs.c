// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include "drm/amdxdna_accel.h"
#include <drm/drm_device.h>
#include <linux/bits.h>

#include "aie.h"
#include "aie4_pci.h"
#include "aie4_plat.h"
#include "amdxdna_drv.h"

#define NPU3B_DPM_TOPS(ndev, hclk) (4096 * (ndev)->total_col * (hclk) / 1000000)

static const struct amdxdna_fw_feature_tbl npu3b_fw_feature_table[] = {
	{ .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_GET_COREDUMP), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_RW_ACCESS), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_FW_LOG), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_FW_TRACE), .major = 6, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_CALIBRATE_CLOCK), .major = 6, .min_minor = 0 },
	{ 0 }
};

static const struct amdxdna_fw_feature_tbl npu3b_cert_feature_table[] = {
	{ .major = 1, .min_minor = 0 },
	{ .features = BIT_U64(AIE4_HSA_COMMAND), .major = 1, .min_minor = 0 },
	{ 0 }
};

static const struct dpm_clk_freq npu3b_dpm_clk_table[] = {
	{  400,  400},
	{  960,  576},
	{ 1108,  576},
	{ 1200,  847},
	{ 1200, 1200},
	{ 1200, 1200},
	{ 1200, 1200},
	{ 1200, 1200},
	{ 0 }
};

static int npu3b_set_dpm(struct aie_device *aie, u32 dpm_level)
{
	struct amdxdna_dev_hdl *ndev = aie->xdna->dev_handle;
	int max_dpm_level = 0;

	while (ndev->priv->dpm_clk_tbl[max_dpm_level].hclk)
		max_dpm_level++;
	max_dpm_level--; /* last element is 0 */

	if (max_dpm_level < 0 || dpm_level > max_dpm_level) {
		XDNA_ERR(aie->xdna, "Invalid dpm level, max:%d, request:%d",
			 max_dpm_level, dpm_level);
		return -EINVAL;
	}

	aie->npuclk_freq = ndev->priv->dpm_clk_tbl[dpm_level].npuclk;
	aie->hclk_freq = ndev->priv->dpm_clk_tbl[dpm_level].hclk;
	aie->max_tops = NPU3B_DPM_TOPS(ndev, ndev->priv->dpm_clk_tbl[max_dpm_level].hclk);
	aie->curr_tops = NPU3B_DPM_TOPS(ndev, aie->hclk_freq);

	XDNA_DBG(aie->xdna, "MP-NPU clock %d, H clock %d\n",
		 aie->npuclk_freq, aie->hclk_freq);

	ndev->max_dpm_level = max_dpm_level;
	return 0;
}

static int npu3b_update_counters(struct aie_device *aie)
{
	struct amdxdna_dev_hdl *ndev = aie->xdna->dev_handle;
	u32 aieclk_level, npuhclk_level;
	int ret;

	ret = aie4_query_dpm_level(ndev, &aieclk_level, &npuhclk_level);
	if (!ret) {
		aie->npuclk_freq = ndev->dpm_clk_tbl[aieclk_level].npuclk;
		aie->hclk_freq = ndev->dpm_clk_tbl[npuhclk_level].hclk;
		aie->max_tops = NPU3B_DPM_TOPS(ndev, ndev->dpm_clk_tbl[ndev->max_dpm_level].hclk);
		if (!aie->hclk_freq)
			XDNA_WARN(aie->xdna, "dpm freq table not populated, clk is 0");
	} else {
		XDNA_WARN(aie->xdna, "cannot get dpm level from fw, using default");
	}

	aie->curr_tops = NPU3B_DPM_TOPS(ndev, aie->hclk_freq);

	return 0;
}

static const struct aie_hw_ops npu3b_hw_ops = {
	.set_dpm = npu3b_set_dpm,
	.update_counters = npu3b_update_counters,
};

static const struct amdxdna_dev_priv npu3b_dev_priv = {
	/* SoC firmware is booted by the RPU/PLM; no npu/cert fw load from Linux. */
	.dpm_clk_tbl	= npu3b_dpm_clk_table,
	.hw_ops		= &npu3b_hw_ops,
};

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
	.default_vbnv		= "RyzenAI-npu3-aie2ps",
	.device_type		= AMDXDNA_DEV_TYPE_UMQ,
	.dev_priv		= &npu3b_dev_priv,
	.fw_feature_tbl		= npu3b_fw_feature_table,
	.cert_feature_tbl	= npu3b_cert_feature_table,
	.ops			= &aie4_plat_ops,
};
