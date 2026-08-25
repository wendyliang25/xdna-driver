// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Platform (device-tree) driver for amdxdna on non-PCI SoC parts.
 *
 * The NPU is not a PCI function: it is described in the device tree by the
 * "amd,amdxdna" binding and talks to the remote firmware through two
 * reserved-memory regions (a "mgmt" mailbox and a "doorbell" region) with a
 * pair of ZynqMP IPI channels used only for interrupt notification.  The
 * management mailbox transport lives in amdxdna_mailbox_plat.c; the device ops
 * (and the doorbell) are provided by aie4_plat.c.
 *
 * This driver does the transport-independent bring-up: allocate the drm/xdna
 * objects, create the mailbox, run the device ops init, then register the accel
 * node.  It shares the DRM driver, sysfs and debugfs with the PCI path.
 */

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "aie4_plat.h"
#include "amdxdna_cbuf.h"
#include "amdxdna_ctx.h"
#include "amdxdna_debugfs.h"
#include "amdxdna_drv.h"

static void amdxdna_plat_drm_release(struct drm_device *drm, void *res)
{
	struct amdxdna_dev *xdna = res;

	cleanup_srcu_struct(&xdna->dpt_srcu);
	ida_destroy(&xdna->hwctx_ida);
}

/*
 * Resolve the optional firmware DMA master -- the processor (e.g. an r5f core)
 * that accesses the firmware memory bank. That bank must be mapped through the
 * master's DMA context (32-bit mask, plus an IOMMU stream ID when present), which
 * the DMA API derives from the master's struct device, so we take the master's
 * own core device via its DT node -- not its remoteproc handle -- and there is no
 * remoteproc coupling. The cluster (e.g. r5fss) driver configures that core
 * device's DMA during probe, and no driver binds to the core node itself, so
 * defer until its parent is bound. Absent "amd,fw-dma-master" (e.g. a Xen domU
 * with no firmware-processor node) means the firmware bank uses this device and
 * relies on its reserved-memory being placed below 4 GB.
 */
static int amdxdna_plat_get_fw_dev(struct amdxdna_dev *xdna, struct device_node *np)
{
	struct platform_device *fw_pdev;
	struct device_node *fw_np;

	fw_np = of_parse_phandle(np, "amd,fw-dma-master", 0);
	if (!fw_np)
		return 0;

	fw_pdev = of_find_device_by_node(fw_np);
	of_node_put(fw_np);
	if (!fw_pdev)
		return -EPROBE_DEFER;

	if (!fw_pdev->dev.parent || !device_is_bound(fw_pdev->dev.parent)) {
		platform_device_put(fw_pdev);
		return -EPROBE_DEFER;
	}

	xdna->fw_dev = &fw_pdev->dev;
	return 0;
}

static void amdxdna_plat_put_fw_dev(struct amdxdna_dev *xdna)
{
	if (xdna->fw_dev) {
		put_device(xdna->fw_dev);
		xdna->fw_dev = NULL;
	}
}

static int amdxdna_plat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct amdxdna_dev_info *dev_info;
	struct amdxdna_dev *xdna;
	struct drm_device *ddev;
	int ret;

	dev_info = of_device_get_match_data(dev);
	if (!dev_info)
		return -ENODEV;

	xdna = devm_drm_dev_alloc(dev, &amdxdna_drm_drv, typeof(*xdna), ddev);
	if (IS_ERR(xdna))
		return PTR_ERR(xdna);
	ddev = &xdna->ddev;

	xdna->dev_info = dev_info;

	ret = drmm_mutex_init(ddev, &xdna->client_lock);
	if (ret)
		return ret;

	ret = drmm_mutex_init(ddev, &xdna->dev_lock);
	if (ret)
		return ret;

	init_rwsem(&xdna->notifier_lock);
	INIT_LIST_HEAD(&xdna->client_list);
	ida_init(&xdna->hwctx_ida);
	platform_set_drvdata(pdev, xdna);

	ret = init_srcu_struct(&xdna->dpt_srcu);
	if (ret)
		return ret;

	ret = drmm_add_action(ddev, amdxdna_plat_drm_release, xdna);
	if (ret) {
		cleanup_srcu_struct(&xdna->dpt_srcu);
		return ret;
	}

	if (IS_ENABLED(CONFIG_LOCKDEP)) {
		fs_reclaim_acquire(GFP_KERNEL);
		might_lock(&xdna->notifier_lock);
		fs_reclaim_release(GFP_KERNEL);
	}

	xdna->notifier_wq = drmm_alloc_ordered_workqueue(ddev, "notifier_wq",
							 WQ_MEM_RECLAIM);
	if (IS_ERR(xdna->notifier_wq))
		return PTR_ERR(xdna->notifier_wq);

	if (!aie4_plat_ndev_alloc(xdna))
		return -ENOMEM;

	/*
	 * Resolve the optional firmware DMA device, then bind the DT memory banks
	 * before device init (the firmware handshake allocates mgmt buffers from the
	 * firmware bank). Firmware-bank buffers map through that device when present;
	 * otherwise their 32-bit reachability comes from the reserved-memory being
	 * placed below 4 GB.
	 */
	ret = amdxdna_plat_get_fw_dev(xdna, dev->of_node);
	if (ret)
		return ret;

	ret = amdxdna_mem_banks_init(xdna, dev->of_node);
	if (ret) {
		XDNA_ERR(xdna, "Memory bank init failed, ret %d", ret);
		goto put_fw_dev;
	}

	/*
	 * ops->init() (aie4_init) runs the shared aie4 bring-up, which creates the
	 * shmem mgmt mailbox (aie4_mailbox_init) and does the firmware handshake.
	 */
	mutex_lock(&xdna->dev_lock);
	ret = xdna->dev_info->ops->init(xdna);
	mutex_unlock(&xdna->dev_lock);
	if (ret) {
		XDNA_ERR(xdna, "Device init failed, ret %d", ret);
		goto banks_fini;
	}

	ret = amdxdna_sysfs_init(xdna);
	if (ret) {
		XDNA_ERR(xdna, "Create amdxdna attrs failed: %d", ret);
		goto dev_fini;
	}

	ret = drm_dev_register(ddev, 0);
	if (ret) {
		XDNA_ERR(xdna, "DRM register failed, ret %d", ret);
		goto sysfs_fini;
	}

	amdxdna_debugfs_init(xdna);

	XDNA_INFO(xdna, "amdxdna platform device probed");
	return 0;

sysfs_fini:
	amdxdna_sysfs_fini(xdna);
dev_fini:
	mutex_lock(&xdna->dev_lock);
	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
banks_fini:
	amdxdna_mem_banks_fini(xdna);
put_fw_dev:
	amdxdna_plat_put_fw_dev(xdna);
	return ret;
}

static void amdxdna_plat_remove(struct platform_device *pdev)
{
	struct amdxdna_dev *xdna = platform_get_drvdata(pdev);
	struct amdxdna_client *client;

	drm_dev_unplug(&xdna->ddev);
	amdxdna_sysfs_fini(xdna);

	mutex_lock(&xdna->client_lock);
	mutex_lock(&xdna->dev_lock);
	list_for_each_entry(client, &xdna->client_list, node)
		amdxdna_hwctx_remove_all(client);

	/* ops->fini (aie4_fini) tears down the shmem mgmt mailbox. */
	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
	mutex_unlock(&xdna->client_lock);

	amdxdna_mem_banks_fini(xdna);
	amdxdna_plat_put_fw_dev(xdna);
}

static const struct of_device_id amdxdna_plat_of_match[] = {
	{ .compatible = "amd,amdxdna", .data = &dev_npu3b_info },
	{ }
};
MODULE_DEVICE_TABLE(of, amdxdna_plat_of_match);

static struct platform_driver amdxdna_plat_driver = {
	.probe	= amdxdna_plat_probe,
	.remove	= amdxdna_plat_remove,
	.driver	= {
		.name		= "amdxdna_platform",
		.of_match_table	= amdxdna_plat_of_match,
	},
};

/*
 * This file is only built for the device-tree (OF) transport, which is mutually
 * exclusive with the PCI driver at build time (see Kbuild).  It therefore owns
 * module init/exit and the MODULE_* metadata for the OF build; the PCI driver
 * (amdxdna_pci_drv.c) owns them for the PCI build.
 */
module_platform_driver(amdxdna_plat_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_DESCRIPTION("amdxdna platform driver");
