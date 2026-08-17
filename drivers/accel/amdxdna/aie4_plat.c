// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Platform (non-PCI) transport hooks for the shared aie4 lifecycle.
 *
 * The classic aie4 lifecycle (init/fini/hw_start/hw_stop/suspend/resume) lives
 * in aie4.c and is shared with the PCI path; aie4_plat_ops points at it.  This
 * file only provides the platform side of the compile-time transport hooks
 * (aie4_dev_setup, aie4_fw_load/_unload, aie4_mailbox_init/_fini,
 * aie4_hw_start_quirk, aie4_hw_resume_prepare/_cleanup) and the shmem+IPI
 * doorbell (amdxdna_ring_ctx_doorbell).  Selected at link time by Kbuild
 * (OFT_CONFIG_AMDXDNA_OF).  See docs/aie4_plat_design.md.
 *
 * There is no firmware load and no PSP/SMU on the SoC (the RPU/PLM boots the
 * AIE firmware); the hooks for those are no-ops.
 */

#include "drm/amdxdna_accel.h"
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <linux/cleanup.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "aie.h"
#include "aie4.h"
#include "aie4_pci.h"
#include "aie4_plat.h"
#include "amdxdna_mailbox_plat.h"
#include "amdxdna_drv.h"

struct amdxdna_dev_hdl *aie4_plat_ndev_alloc(struct amdxdna_dev *xdna)
{
	struct amdxdna_dev_hdl *ndev;

	ndev = drmm_kzalloc(&xdna->ddev, sizeof(*ndev), GFP_KERNEL);
	if (!ndev)
		return NULL;

	ndev->priv = xdna->dev_info->dev_priv;
	ndev->aie.xdna = xdna;
	ndev->kernel_submit = true;
	ndev->ctx_switch_hysteresis_us = AIE4_CTX_HYSTERESIS_US;
	ndev->pw_mode = POWER_MODE_DEFAULT;

	xa_init_flags(&ndev->cert_comp_xa, XA_FLAGS_ALLOC);
	mutex_init(&ndev->cert_comp_lock);
	spin_lock_init(&ndev->db_lock);

	xdna->dev_handle = ndev;
	return ndev;
}

static int aie4_plat_doorbell_init(struct amdxdna_dev_hdl *ndev)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	struct platform_device *pdev = to_platform_device(xdna->ddev.dev);
	struct device_node *np = pdev->dev.of_node;
	struct device_node *mem_np;
	struct shmem_db_ring *ring;
	struct resource res;
	resource_size_t slots;
	int idx, ret;

	if (ndev->db_ring)
		return 0;

	idx = of_property_match_string(np, "memory-region-names", "doorbell");
	if (idx < 0) {
		XDNA_ERR(xdna, "missing 'doorbell' memory-region-names");
		return idx;
	}

	mem_np = of_parse_phandle(np, "memory-region", idx);
	if (!mem_np)
		return -ENODEV;

	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret)
		return ret;

	ndev->db_shmem = devm_ioremap_wc(&pdev->dev, res.start,
					 resource_size(&res));
	if (!ndev->db_shmem)
		return -ENOMEM;

	XDNA_INFO(xdna, "doorbell shmem: %pa size 0x%llx",
		  &res.start, (u64)resource_size(&res));

	/* Doorbell ring uses the entire region (slot-indexed by hw_ctx_id). */
	ring = ndev->db_shmem;
	slots = (resource_size(&res) - offsetof(struct shmem_db_ring, data))
		/ sizeof(u32);
	slots = rounddown_pow_of_two(slots);
	ring->head = 0;
	ring->tail = 0;
	ring->ring_mask = slots - 1;
	ring->rsvd = 0;

	ndev->db_ring = ring;
	ndev->db_ring_mask = slots - 1;

	return 0;
}

/* Seam #2: ring the kernel-mode doorbell (shmem ring produce + IPI kick). */
int amdxdna_ring_ctx_doorbell(struct amdxdna_dev_hdl *ndev, u32 hw_ctx_id)
{
	guard(spinlock)(&ndev->db_lock);

	/* TODO: SPSC-produce hw_ctx_id into ndev->db_ring. */

	return amdxdna_mailbox_plat_kick(ndev->mbox_plat);
}

/*
 * Transport hooks for the shared aie4 lifecycle (aie4.c); platform variants.
 * The device (drm/ndev/mailbox) is set up in amdxdna_plat_probe(), so
 * aie4_dev_setup() is a no-op here.
 */
int aie4_dev_setup(struct amdxdna_dev *xdna)
{
	return 0;
}

int aie4_fw_load(struct amdxdna_dev_hdl *ndev)
{
	return 0; /* RPU/PLM boots the AIE firmware; nothing to load from Linux. */
}

void aie4_fw_unload(struct amdxdna_dev_hdl *ndev)
{
}

int aie4_hw_start_quirk(struct amdxdna_dev_hdl *ndev)
{
	return 0;
}

int aie4_hw_resume_prepare(struct amdxdna_dev *xdna)
{
	return 0;
}

void aie4_hw_resume_cleanup(struct amdxdna_dev *xdna)
{
}

/*
 * Bring up the mgmt mailbox channel over the shmem+IPI transport and map the
 * doorbell region.  The shmem regions and IPI channels were acquired in
 * amdxdna_plat_probe() (amdxdna_mailbox_plat_create).
 */
int aie4_mailbox_init(struct amdxdna_dev_hdl *ndev)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	int ret;

	if (!ndev->mbox_plat) {
		XDNA_ERR(xdna, "platform mailbox not created");
		return -ENODEV;
	}

	ret = aie4_plat_doorbell_init(ndev);
	if (ret)
		return ret;

	/*
	 * TODO: build the shmem struct mailbox_channel and publish it as
	 * ndev->aie.mgmt_chann so the shared aie_send_mgmt_msg_wait() path
	 * reaches the shmem mailbox (amdxdna_mailbox_plat.c).
	 */
	return 0;
}

void aie4_mailbox_fini(struct amdxdna_dev_hdl *ndev)
{
	/* TODO: tear down the shmem mgmt mailbox_channel. */
}

const struct amdxdna_dev_ops aie4_plat_ops = {
	.init			= aie4_init,
	.fini			= aie4_fini,
	.hwctx_init		= aie4_hwctx_init,
	.hwctx_fini		= aie4_hwctx_fini,
	.hwctx_config		= aie4_hwctx_config,
	.cmd_submit		= aie4_cmd_submit,
	.cmd_wait		= aie4_cmd_wait,
	.get_aie_info		= aie4_get_info,
	.set_aie_state		= aie4_set_state,
	.get_array		= aie4_get_array,
	.resume			= aie4_resume,
	.suspend		= aie4_suspend,
	.runtime_resume		= aie4_resume,
	.runtime_suspend	= aie4_suspend,
	.register_async_event	= aie4_async_event_register,
	.handle_dev_async_event	= aie4_handle_dev_event,
};
