// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Shared-memory + ZynqMP IPI transport driver for amdxdna (AIE4).
 *
 * When CONFIG_AMDXDNA_SHMEM is enabled, this file is the main driver
 * entry point.  The module registers a platform driver that binds via
 * device tree compatible string "amd,amdxdna-shmem".
 *
 * Communication with the remote firmware uses:
 *   - Two shared memory regions (mgmt mailbox + doorbell) from
 *     reserved-memory, for the actual message data.
 *   - ZynqMP IPI mailbox channels (TX + RX) via the Linux mailbox
 *     framework for interrupt notification only — no IPI message
 *     buffers are used.  Currently uses bufferless (nobuf) IPIs
 *     (agents 0x0a/0x0b) because PLM does not route buffered
 *     ipi0/ipi1 (agents 0x02/0x03) notifications correctly.
 *
 * The ZynqMP IPI mailbox controller (drivers/mailbox/zynqmp-ipi-mailbox.c)
 * handles the underlying IPI hardware (SMC/HVC calls to ATF for
 * IPI notification, ack, and IRQ management).  This driver is a
 * mailbox client that requests TX/RX channels via
 * mbox_request_channel_byname() and uses mbox_send_message() to
 * trigger an IPI to the remote, and receives rx_callback() when the
 * remote triggers an IPI to us.
 *
 * See dt-bindings/amd,amdxdna-shmem.yaml for the device tree binding.
 */

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "aie4_pci.h"
#include "aie4_message.h"
#include "amdxdna_shmem.h"
#include "amdxdna_sysfs.h"

struct shmem_inflight_msg {
	u32			id;
	void			*handle;
	int			(*notify_cb)(void *handle, void __iomem *data,
					     size_t size);
};

struct amdxdna_shmem_hdl {
	struct amdxdna_dev_hdl	*ndev;
	struct platform_device	*pdev;

	/* Shared memory regions mapped from device tree reserved-memory */
	void			*mgmt_shmem;
	resource_size_t		mgmt_shmem_size;
	void			*db_shmem;
	resource_size_t		db_shmem_size;

	/* Mgmt TX ring (host is producer) */
	struct shmem_ring_hdr	*tx_hdr;
	void			*tx_ring;

	/* Mgmt RX ring (host is consumer) */
	struct shmem_ring_hdr	*rx_hdr;
	void			*rx_ring;

	/* Doorbell ring (host is producer) */
	struct shmem_db_ring	*db_ring;

	/* Cached indices to minimise shared memory reads */
	u32			tx_local_head;
	u32			tx_cached_tail;
	u32			rx_local_tail;
	u32			rx_cached_head;
	u32			db_local_head;
	u32			db_cached_tail;

	/* Inflight management message tracking */
	struct xarray		msg_xa;
	struct mutex		msg_lock; /* serialise next_msg_id + xa_insert */
	u32			next_msg_id;

	/* Linux mailbox client for ZynqMP IPI (notification only) */
	struct mbox_client	tx_cl;
	struct mbox_client	rx_cl;
	struct mbox_chan	*tx_chan;
	struct mbox_chan	*rx_chan;

	/* Deferred mgmt RX processing (doorbell runs in IRQ context) */
	struct work_struct	rx_work;

};

/* Maximum response payload we expect from firmware (fits on kernel stack) */
#define SHMEM_MAX_RESP_SIZE	512


static void amdxdna_shmem_doorbell_notify(struct amdxdna_dev_hdl *ndev)
{
	struct cert_comp *cert_comp;
	unsigned long idx;

	xa_for_each(&ndev->cert_comp_xa, idx, cert_comp)
		wake_up_all(&cert_comp->waitq);
}

static void amdxdna_shmem_rx_work(struct work_struct *work)
{
	struct amdxdna_shmem_hdl *shdl =
		container_of(work, struct amdxdna_shmem_hdl, rx_work);
	u8 buf[SHMEM_MAX_RESP_SIZE];
	struct shmem_msg_hdr msg_hdr;
	struct shmem_inflight_msg *ifm;
	int payload_size;

	/* Drain all available management responses */
	while (shdl->rx_hdr) {
		payload_size = shmem_mgmt_consume(shdl->rx_hdr, shdl->rx_ring,
						  &shdl->rx_local_tail,
						  &shdl->rx_cached_head,
						  &msg_hdr, buf, sizeof(buf));
		if (payload_size < 0)
			break;

		ifm = xa_erase(&shdl->msg_xa, msg_hdr.id);

		if (!ifm) {
			dev_warn(&shdl->pdev->dev,
				 "unexpected response id %u\n", msg_hdr.id);
			continue;
		}

		/* Cast for xdna_mailbox_msg callback signature compatibility */
		if (ifm->notify_cb)
			ifm->notify_cb(ifm->handle, (void __iomem *)buf,
				       payload_size);

		kfree(ifm);
	}
}

/*
 * Called by the mailbox framework when the remote processor sends an
 * IPI to us (RX channel callback).  This runs in IRQ context.
 *
 * Doorbell completion is handled directly here (fast path -- just
 * wake_up_all which is IRQ-safe).  Management response draining is
 * deferred to a workqueue since it involves xarray + kfree.
 */
static void amdxdna_shmem_rx_callback(struct mbox_client *cl, void *data)
{
	struct amdxdna_shmem_hdl *shdl =
		container_of(cl, struct amdxdna_shmem_hdl, rx_cl);

	amdxdna_shmem_doorbell_notify(shdl->ndev);
	schedule_work(&shdl->rx_work);

	/*
	 * ACK the IPI to re-enable the notification interrupt.
	 * The zynqmp-ipi ISR disables the IRQ via SMC STATUS_ENQUIRY
	 * with DIRQ_MASK; sending on the RX channel triggers
	 * SMC_IPI_MAILBOX_ACK with EIRQ_MASK to re-enable it.
	 */
	mbox_send_message(shdl->rx_chan, NULL);
	mbox_client_txdone(shdl->rx_chan, 0);
}

static int amdxdna_shmem_send_msg(void *xcomm_hdl,
				  const struct xdna_mailbox_msg *msg)
{
	struct amdxdna_shmem_hdl *shdl = xcomm_hdl;
	struct shmem_inflight_msg *ifm;
	struct shmem_msg_hdr hdr;
	u32 id;
	int ret;

	ifm = kzalloc(sizeof(*ifm), GFP_KERNEL);
	if (!ifm)
		return -ENOMEM;

	mutex_lock(&shdl->msg_lock);
	id = shdl->next_msg_id++;
	ifm->id = id;
	ifm->handle = msg->handle;
	ifm->notify_cb = msg->notify_cb;

	ret = xa_insert(&shdl->msg_xa, id, ifm, GFP_KERNEL);
	if (ret) {
		mutex_unlock(&shdl->msg_lock);
		kfree(ifm);
		return ret;
	}
	mutex_unlock(&shdl->msg_lock);

	hdr.total_size = sizeof(hdr) + msg->send_size;
	hdr.id = id;
	hdr.opcode = msg->opcode;
	hdr.status = 0;

	ret = shmem_mgmt_produce(shdl->tx_hdr, shdl->tx_ring,
				 &shdl->tx_local_head,
				 &shdl->tx_cached_tail,
				 &hdr, msg->send_data, msg->send_size);
	if (ret) {
		xa_erase(&shdl->msg_xa, id);
		kfree(ifm);
		return ret;
	}

	/* Trigger IPI to notify firmware */
	ret = mbox_send_message(shdl->tx_chan, NULL);
	if (ret < 0) {
		xa_erase(&shdl->msg_xa, id);
		kfree(ifm);
		return ret;
	}
	mbox_client_txdone(shdl->tx_chan, 0);

	return 0;
}

static int amdxdna_shmem_ring_doorbell(void *xcomm_hdl, u32 hw_ctx_id)
{
	struct amdxdna_shmem_hdl *shdl = xcomm_hdl;
	int ret;

	ret = shmem_db_produce(shdl->db_ring,
			       &shdl->db_local_head,
			       &shdl->db_cached_tail,
			       hw_ctx_id);
	if (ret)
		return ret;

	ret = mbox_send_message(shdl->tx_chan, NULL);
	if (ret < 0)
		return ret;
	mbox_client_txdone(shdl->tx_chan, 0);

	return 0;
}

static void amdxdna_shmem_fini(void *xcomm_hdl)
{
	struct amdxdna_shmem_hdl *shdl = xcomm_hdl;
	struct shmem_inflight_msg *ifm;
	unsigned long idx;

	if (shdl->rx_chan)
		mbox_free_channel(shdl->rx_chan);
	if (shdl->tx_chan)
		mbox_free_channel(shdl->tx_chan);

	cancel_work_sync(&shdl->rx_work);

	xa_for_each(&shdl->msg_xa, idx, ifm) {
		xa_erase(&shdl->msg_xa, idx);
		kfree(ifm);
	}
	xa_destroy(&shdl->msg_xa);
	mutex_destroy(&shdl->msg_lock);
}

static void amdxdna_shmem_rings_init(struct amdxdna_shmem_hdl *shdl)
{
	resource_size_t half = shdl->mgmt_shmem_size / 2;
	resource_size_t ring_data;

	/* Split mgmt region: first half is TX, second half is RX */
	shdl->tx_hdr = (struct shmem_ring_hdr *)shdl->mgmt_shmem;
	shdl->tx_ring = shdl->mgmt_shmem + sizeof(struct shmem_ring_hdr);

	shdl->rx_hdr = (struct shmem_ring_hdr *)(shdl->mgmt_shmem + half);
	shdl->rx_ring = shdl->mgmt_shmem + half +
			sizeof(struct shmem_ring_hdr);

	/*
	 * Ring data area is the half minus the header.  Round down to the
	 * largest power-of-2 so the mask has all lower bits set.
	 */
	ring_data = rounddown_pow_of_two(half - sizeof(struct shmem_ring_hdr));
	WRITE_ONCE(shdl->tx_hdr->head, 0);
	WRITE_ONCE(shdl->tx_hdr->tail, 0);
	WRITE_ONCE(shdl->tx_hdr->ring_mask, ring_data - 1);
	WRITE_ONCE(shdl->tx_hdr->rsvd, 0);

	WRITE_ONCE(shdl->rx_hdr->head, 0);
	WRITE_ONCE(shdl->rx_hdr->tail, 0);
	WRITE_ONCE(shdl->rx_hdr->ring_mask, ring_data - 1);
	WRITE_ONCE(shdl->rx_hdr->rsvd, 0);

	/* Doorbell ring uses the entire doorbell region (slot-indexed) */
	shdl->db_ring = (struct shmem_db_ring *)shdl->db_shmem;
	ring_data = (shdl->db_shmem_size - offsetof(struct shmem_db_ring, data))
		    / sizeof(u32);
	ring_data = rounddown_pow_of_two(ring_data);
	WRITE_ONCE(shdl->db_ring->head, 0);
	WRITE_ONCE(shdl->db_ring->tail, 0);
	WRITE_ONCE(shdl->db_ring->ring_mask, ring_data - 1);
	WRITE_ONCE(shdl->db_ring->rsvd, 0);

	/* Reset local cached indices */
	shdl->tx_local_head = 0;
	shdl->tx_cached_tail = 0;
	shdl->rx_local_tail = 0;
	shdl->rx_cached_head = 0;
	shdl->db_local_head = 0;
	shdl->db_cached_tail = 0;

	xa_init(&shdl->msg_xa);
	mutex_init(&shdl->msg_lock);
	shdl->next_msg_id = 0;

	INIT_WORK(&shdl->rx_work, amdxdna_shmem_rx_work);
}

static const struct amdxdna_xcomm_ops amdxdna_shmem_xcomm_ops = {
	.send_msg	= amdxdna_shmem_send_msg,
	.ring_doorbell	= amdxdna_shmem_ring_doorbell,
	.fini		= amdxdna_shmem_fini,
};

static int amdxdna_shmem_map_regions(struct amdxdna_shmem_hdl *shdl)
{
	struct platform_device *pdev = shdl->pdev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *mem_np;
	struct resource res;
	int idx, ret;

	idx = of_property_match_string(np, "memory-region-names", "mgmt");
	if (idx < 0) {
		dev_err(&pdev->dev, "missing 'mgmt' memory-region-names\n");
		return idx;
	}

	mem_np = of_parse_phandle(np, "memory-region", idx);
	if (!mem_np)
		return -ENODEV;

	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret)
		return ret;

	shdl->mgmt_shmem_size = resource_size(&res);
	shdl->mgmt_shmem =
		devm_ioremap_wc(&pdev->dev, res.start, shdl->mgmt_shmem_size);
	if (!shdl->mgmt_shmem)
		return -ENOMEM;

	dev_info(&pdev->dev, "mgmt shmem: %pa size 0x%llx\n",
		 &res.start, (u64)shdl->mgmt_shmem_size);

	idx = of_property_match_string(np, "memory-region-names", "doorbell");
	if (idx < 0) {
		dev_err(&pdev->dev, "missing 'doorbell' memory-region-names\n");
		return idx;
	}

	mem_np = of_parse_phandle(np, "memory-region", idx);
	if (!mem_np)
		return -ENODEV;

	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret)
		return ret;

	shdl->db_shmem_size = resource_size(&res);
	shdl->db_shmem =
		devm_ioremap_wc(&pdev->dev, res.start, shdl->db_shmem_size);
	if (!shdl->db_shmem)
		return -ENOMEM;

	dev_info(&pdev->dev, "doorbell shmem: %pa size 0x%llx\n",
		 &res.start, (u64)shdl->db_shmem_size);

	return 0;
}

static int amdxdna_shmem_mbox_init(struct amdxdna_shmem_hdl *shdl)
{
	struct device *dev = &shdl->pdev->dev;

	shdl->tx_cl.dev = dev;
	shdl->tx_cl.tx_block = false;
	shdl->tx_cl.knows_txdone = true;
	shdl->tx_cl.tx_done = NULL;

	shdl->rx_cl.dev = dev;
	shdl->rx_cl.rx_callback = amdxdna_shmem_rx_callback;
	shdl->rx_cl.knows_txdone = true;
	shdl->rx_cl.tx_done = NULL;

	shdl->tx_chan = mbox_request_channel_byname(&shdl->tx_cl, "tx");
	if (IS_ERR(shdl->tx_chan)) {
		int ret = PTR_ERR(shdl->tx_chan);

		dev_err(dev, "failed to request IPI TX channel: %d\n", ret);
		shdl->tx_chan = NULL;
		return ret;
	}

	shdl->rx_chan = mbox_request_channel_byname(&shdl->rx_cl, "rx");
	if (IS_ERR(shdl->rx_chan)) {
		int ret = PTR_ERR(shdl->rx_chan);

		dev_err(dev, "failed to request IPI RX channel: %d\n", ret);
		shdl->rx_chan = NULL;
		mbox_free_channel(shdl->tx_chan);
		shdl->tx_chan = NULL;
		return ret;
	}

	return 0;
}

static const struct amdxdna_dev_info amdxdna_shmem_dev_info = {
};

static int amdxdna_shmem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct amdxdna_shmem_hdl *shdl;
	struct amdxdna_dev_hdl *ndev;
	struct amdxdna_dev *xdna;
	int ret;

	xdna = devm_drm_dev_alloc(dev, &amdxdna_drm_drv,
				  typeof(*xdna), ddev);
	if (IS_ERR(xdna))
		return PTR_ERR(xdna);

	xdna->dev_info = &amdxdna_shmem_dev_info;

	drmm_mutex_init(&xdna->ddev, &xdna->dev_lock);
	init_rwsem(&xdna->notifier_lock);
	INIT_LIST_HEAD(&xdna->client_list);

	if (IS_ENABLED(CONFIG_LOCKDEP)) {
		fs_reclaim_acquire(GFP_KERNEL);
		might_lock(&xdna->notifier_lock);
		fs_reclaim_release(GFP_KERNEL);
	}

	ndev = drmm_kzalloc(&xdna->ddev, sizeof(*ndev), GFP_KERNEL);
	if (!ndev)
		return -ENOMEM;

	shdl = drmm_kzalloc(&xdna->ddev, sizeof(*shdl), GFP_KERNEL);
	if (!shdl)
		return -ENOMEM;

	shdl->ndev = ndev;
	shdl->pdev = pdev;

	ret = amdxdna_shmem_map_regions(shdl);
	if (ret) {
		dev_err(dev, "failed to map shared memory regions: %d\n", ret);
		return ret;
	}

	ret = amdxdna_shmem_mbox_init(shdl);
	if (ret)
		return ret;

	amdxdna_shmem_rings_init(shdl);

	ndev->xdna = xdna;
	ndev->xcomm_ops = &amdxdna_shmem_xcomm_ops;
	ndev->xcomm_hdl = shdl;
	xa_init(&ndev->cert_comp_xa);
	mutex_init(&ndev->aie4_lock);

	xdna->dev_handle = ndev;
	platform_set_drvdata(pdev, xdna);

	xdna->notifier_wq = alloc_ordered_workqueue("notifier_wq",
						    WQ_MEM_RECLAIM);
	if (!xdna->notifier_wq) {
		ret = -ENOMEM;
		goto mbox_fini;
	}

	ret = amdxdna_sysfs_init(xdna);
	if (ret)
		goto destroy_wq;

	ret = drm_dev_register(&xdna->ddev, 0);
	if (ret)
		goto sysfs_fini;

	/*
	* TODO: Using existing debugfs interface for now
	* remove this once its no longer needed
	*/
	aie4_debugfs_init(xdna);

	dev_info(dev, "amdxdna shmem+IPI driver probed\n");
	return 0;

sysfs_fini:
	amdxdna_sysfs_fini(xdna);
destroy_wq:
	destroy_workqueue(xdna->notifier_wq);
mbox_fini:
	amdxdna_shmem_fini(shdl);
	return ret;
}

static void amdxdna_shmem_remove(struct platform_device *pdev)
{
	struct amdxdna_dev *xdna = platform_get_drvdata(pdev);
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;

	destroy_workqueue(xdna->notifier_wq);
	drm_dev_unplug(&xdna->ddev);
	amdxdna_sysfs_fini(xdna);

	if (ndev->xcomm_ops && ndev->xcomm_ops->fini)
		ndev->xcomm_ops->fini(ndev->xcomm_hdl);
}

static const struct of_device_id amdxdna_shmem_of_match[] = {
	{ .compatible = "amd,amdxdna-shmem" },
	{ },
};
MODULE_DEVICE_TABLE(of, amdxdna_shmem_of_match);

static struct platform_driver amdxdna_shmem_driver = {
	.probe	= amdxdna_shmem_probe,
	.remove	= amdxdna_shmem_remove,
	.driver	= {
		.name		= "amdxdna_shmem",
		.of_match_table	= amdxdna_shmem_of_match,
	},
};

static int __init amdxdna_shmem_init(void)
{
	return platform_driver_register(&amdxdna_shmem_driver);
}

static void __exit amdxdna_shmem_exit(void)
{
	platform_driver_unregister(&amdxdna_shmem_driver);
}

module_init(amdxdna_shmem_init);
module_exit(amdxdna_shmem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("XRT Team <runtimeca39d@amd.com>");
MODULE_DESCRIPTION("amdxdna shared-memory + IPI transport driver for AIE4");
