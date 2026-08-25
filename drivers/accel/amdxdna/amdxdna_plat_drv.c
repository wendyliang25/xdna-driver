// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Platform (non-PCI) driver for the aie4/aie2ps SoC parts.  This is the platform
 * counterpart of amdxdna_pci_drv.c: it owns module init/exit and the MODULE_*
 * metadata for the platform build and binds through an OF match table.  The PCI
 * and platform transports are mutually exclusive at build time
 * (CONFIG_DRM_ACCEL_AMDXDNA_PLAT selects the platform driver and drops
 * amdxdna_pci_drv.o).
 *
 * The shared DRM layer (amdxdna_drm.c, amdxdna_drm_drv) and the aie4 core are
 * compiled into both builds; this file provides only the platform bus glue and
 * hands the ioctls off to the aie4_plat_ops in dev_npu3b_info.
 */

#include "drm/amdxdna_accel.h"
#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sched/mm.h>

#include "amdxdna_cbuf.h"
#include "amdxdna_ctx.h"
#include "amdxdna_debugfs.h"
#include "amdxdna_dpt.h"
#include "amdxdna_drv.h"
#include "amdxdna_pm.h"
#include "amdxdna_plat_drv.h"

static void amdxdna_plat_drm_release(struct drm_device *drm, void *res)
{
	struct amdxdna_dev *xdna = res;

	amdxdna_carveout_fini(xdna);
	amdxdna_dpt_chan_fini(xdna);
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

	ret = amdxdna_dpt_chan_init(xdna);
	if (ret)
		return ret;

	ret = drmm_add_action(ddev, amdxdna_plat_drm_release, xdna);
	if (ret) {
		amdxdna_dpt_chan_fini(xdna);
		return ret;
	}

	if (IS_ENABLED(CONFIG_LOCKDEP)) {
		fs_reclaim_acquire(GFP_KERNEL);
		might_lock(&xdna->notifier_lock);
		fs_reclaim_release(GFP_KERNEL);
	}

	ret = amdxdna_iommu_init(xdna);
	if (ret)
		return ret;

	xdna->notifier_wq = drmm_alloc_ordered_workqueue(ddev, "notifier_wq", WQ_MEM_RECLAIM);
	if (IS_ERR(xdna->notifier_wq)) {
		ret = PTR_ERR(xdna->notifier_wq);
		goto iommu_fini;
	}

	/*
	 * Resolve the optional firmware DMA device, then bind the DT memory banks
	 * before device init (the firmware handshake allocates mgmt buffers from the
	 * firmware bank). Firmware-bank buffers map through that device when present;
	 * otherwise their 32-bit reachability comes from the reserved-memory being
	 * placed below 4 GB.
	 */
	ret = amdxdna_plat_get_fw_dev(xdna, dev->of_node);
	if (ret)
		goto iommu_fini;

	ret = amdxdna_mem_banks_init(xdna, dev->of_node);
	if (ret) {
		XDNA_ERR(xdna, "Memory bank init failed, ret %d", ret);
		goto put_fw_dev;
	}

	mutex_lock(&xdna->dev_lock);
	ret = xdna->dev_info->ops->init(xdna);
	mutex_unlock(&xdna->dev_lock);
	if (ret) {
		XDNA_ERR(xdna, "Hardware init failed, ret %d", ret);
		goto banks_fini;
	}

	ret = amdxdna_sysfs_init(xdna);
	if (ret) {
		XDNA_ERR(xdna, "Create amdxdna attrs failed: %d", ret);
		goto failed_dev_fini;
	}

	ret = drm_dev_register(ddev, 0);
	if (ret) {
		XDNA_ERR(xdna, "DRM register failed, ret %d", ret);
		goto failed_sysfs_fini;
	}

	amdxdna_debugfs_init(xdna);
	XDNA_INFO(xdna, "amdxdna platform device probed");
	return 0;

failed_sysfs_fini:
	amdxdna_sysfs_fini(xdna);
failed_dev_fini:
	mutex_lock(&xdna->dev_lock);
	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
banks_fini:
	amdxdna_mem_banks_fini(xdna);
put_fw_dev:
	amdxdna_plat_put_fw_dev(xdna);
iommu_fini:
	amdxdna_iommu_fini(xdna);
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
	list_for_each_entry(client, &xdna->client_list, node) {
		amdxdna_hwctx_remove_all(client);
		amdxdna_sva_fini(client);
	}

	xdna->dev_info->ops->fini(xdna);
	mutex_unlock(&xdna->dev_lock);
	mutex_unlock(&xdna->client_lock);

	amdxdna_mem_banks_fini(xdna);
	amdxdna_plat_put_fw_dev(xdna);
	amdxdna_iommu_fini(xdna);
}

static const struct dev_pm_ops amdxdna_plat_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(amdxdna_pm_suspend, amdxdna_pm_resume)
	RUNTIME_PM_OPS(amdxdna_pm_runtime_suspend, amdxdna_pm_runtime_resume, NULL)
};

static const struct of_device_id amdxdna_plat_of_match[] = {
	{ .compatible = "amd,amdxdna", .data = &dev_npu3b_info },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, amdxdna_plat_of_match);

static struct platform_driver amdxdna_plat_driver = {
	.probe	= amdxdna_plat_probe,
	.remove	= amdxdna_plat_remove,
	.driver	= {
		.name		= "amdxdna",
		.of_match_table	= amdxdna_plat_of_match,
		.pm		= &amdxdna_plat_pm_ops,
	},
};

module_platform_driver(amdxdna_plat_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_DESCRIPTION("amdxdna platform driver");
