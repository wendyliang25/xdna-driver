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
#include <linux/platform_device.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "aie4_plat.h"
#include "amdxdna_ctx.h"
#include "amdxdna_debugfs.h"
#include "amdxdna_drv.h"

static void amdxdna_plat_drm_release(struct drm_device *drm, void *res)
{
	struct amdxdna_dev *xdna = res;

	cleanup_srcu_struct(&xdna->dpt_srcu);
	ida_destroy(&xdna->hwctx_ida);
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
	 * ops->init() (aie4_init) runs the shared aie4 bring-up, which creates the
	 * shmem mgmt mailbox (aie4_mailbox_init) and does the firmware handshake.
	 */
	mutex_lock(&xdna->dev_lock);
	ret = xdna->dev_info->ops->init(xdna);
	mutex_unlock(&xdna->dev_lock);
	if (ret) {
		XDNA_ERR(xdna, "Device init failed, ret %d", ret);
		return ret;
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
