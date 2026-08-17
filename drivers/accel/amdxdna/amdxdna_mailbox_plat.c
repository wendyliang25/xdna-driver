// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Shared-memory + ZynqMP IPI management mailbox for amdxdna platform parts.
 *
 * Maps the "mgmt" reserved-memory region (host-producer TX ring +
 * host-consumer RX ring) and acquires the tx/rx IPI mailbox channels described
 * by the "amd,amdxdna" device node.  The IPI is a bufferless
 * notification: the command/response payload always lives in shared memory.
 *
 * NOTE: this is the transport skeleton.  Region mapping and IPI channel
 * acquisition are wired up; the SPSC ring produce/consume paths are stubs.
 */

#include <drm/drm_managed.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "amdxdna_mailbox.h"
#include "amdxdna_mailbox_plat.h"
#include "amdxdna_drv.h"

struct amdxdna_mailbox_plat {
	struct amdxdna_dev	*xdna;
	struct platform_device	*pdev;

	/* Mgmt shared memory region mapped from device tree reserved-memory */
	void			*mgmt_shmem;
	resource_size_t		mgmt_shmem_size;

	/* Inflight management message tracking */
	struct xarray		msg_xa;
	spinlock_t		msg_id_lock; /* protects next_msg_id + xa_insert */
	u32			next_msg_id;

	/* Protects the mgmt TX ring write + IPI send */
	spinlock_t		tx_lock;

	/* Linux mailbox client for ZynqMP IPI (notification only) */
	struct mbox_client	tx_cl;
	struct mbox_client	rx_cl;
	struct mbox_chan	*tx_chan;
	struct mbox_chan	*rx_chan;

	/* Deferred mgmt RX processing (rx_callback runs in IRQ context) */
	struct work_struct	rx_work;
};

static void amdxdna_mailbox_plat_rx_work(struct work_struct *work)
{
	struct amdxdna_mailbox_plat *mb =
		container_of(work, struct amdxdna_mailbox_plat, rx_work);

	/* TODO: drain the mgmt RX ring and dispatch responses to waiters. */
	(void)mb;
}

/*
 * Called by the mailbox framework when the remote sends an IPI to us (RX
 * channel callback).  Runs in IRQ context.  ACK the IPI to re-arm the
 * notification interrupt.
 */
static void amdxdna_mailbox_plat_rx_callback(struct mbox_client *cl, void *data)
{
	struct amdxdna_mailbox_plat *mb =
		container_of(cl, struct amdxdna_mailbox_plat, rx_cl);

	dev_dbg(&mb->pdev->dev, "mailbox IPI RX callback\n");

	/* TODO: schedule rx_work only when a mgmt response is actually queued. */

	/* Re-arm the notification interrupt via an ACK on the RX channel. */
	mbox_send_message(mb->rx_chan, NULL);
	mbox_client_txdone(mb->rx_chan, 0);
}

int amdxdna_mailbox_plat_send(struct amdxdna_mailbox_plat *mb,
			      struct xdna_mailbox_msg *msg)
{
	/* TODO: allocate a msg id, SPSC produce into the TX ring, IPI the remote. */
	dev_dbg(&mb->pdev->dev, "mailbox send opcode 0x%x (stub)\n", msg->opcode);
	return 0;
}

int amdxdna_mailbox_plat_kick(struct amdxdna_mailbox_plat *mb)
{
	int ret;

	ret = mbox_send_message(mb->tx_chan, NULL);
	if (ret < 0)
		return ret;
	mbox_client_txdone(mb->tx_chan, 0);
	return 0;
}

static void amdxdna_mailbox_plat_state_init(struct amdxdna_mailbox_plat *mb)
{
	xa_init(&mb->msg_xa);
	spin_lock_init(&mb->msg_id_lock);
	spin_lock_init(&mb->tx_lock);
	mb->next_msg_id = 0;

	INIT_WORK(&mb->rx_work, amdxdna_mailbox_plat_rx_work);
}

static int amdxdna_mailbox_plat_map_mgmt(struct amdxdna_mailbox_plat *mb)
{
	struct platform_device *pdev = mb->pdev;
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

	mb->mgmt_shmem_size = resource_size(&res);
	mb->mgmt_shmem = devm_ioremap_wc(&pdev->dev, res.start,
					 mb->mgmt_shmem_size);
	if (!mb->mgmt_shmem)
		return -ENOMEM;

	dev_info(&pdev->dev, "mgmt shmem: %pa size 0x%llx\n",
		 &res.start, (u64)mb->mgmt_shmem_size);

	return 0;
}

static int amdxdna_mailbox_plat_ipi_init(struct amdxdna_mailbox_plat *mb)
{
	struct device *dev = &mb->pdev->dev;
	int ret;

	mb->tx_cl.dev = dev;
	mb->tx_cl.tx_block = false;
	mb->tx_cl.knows_txdone = true;

	mb->rx_cl.dev = dev;
	mb->rx_cl.rx_callback = amdxdna_mailbox_plat_rx_callback;
	mb->rx_cl.knows_txdone = true;

	mb->tx_chan = mbox_request_channel_byname(&mb->tx_cl, "tx");
	if (IS_ERR(mb->tx_chan)) {
		ret = PTR_ERR(mb->tx_chan);
		dev_err(dev, "failed to request IPI TX channel: %d\n", ret);
		mb->tx_chan = NULL;
		return ret;
	}

	mb->rx_chan = mbox_request_channel_byname(&mb->rx_cl, "rx");
	if (IS_ERR(mb->rx_chan)) {
		ret = PTR_ERR(mb->rx_chan);
		dev_err(dev, "failed to request IPI RX channel: %d\n", ret);
		mb->rx_chan = NULL;
		mbox_free_channel(mb->tx_chan);
		mb->tx_chan = NULL;
		return ret;
	}

	return 0;
}

struct amdxdna_mailbox_plat *
amdxdna_mailbox_plat_create(struct amdxdna_dev *xdna,
			    struct platform_device *pdev)
{
	struct amdxdna_mailbox_plat *mb;
	int ret;

	mb = drmm_kzalloc(&xdna->ddev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return ERR_PTR(-ENOMEM);

	mb->xdna = xdna;
	mb->pdev = pdev;

	ret = amdxdna_mailbox_plat_map_mgmt(mb);
	if (ret)
		return ERR_PTR(ret);

	ret = amdxdna_mailbox_plat_ipi_init(mb);
	if (ret)
		return ERR_PTR(ret);

	amdxdna_mailbox_plat_state_init(mb);

	dev_info(&pdev->dev, "amdxdna platform mailbox created\n");
	return mb;
}

void amdxdna_mailbox_plat_destroy(struct amdxdna_mailbox_plat *mb)
{
	if (!mb)
		return;

	if (mb->rx_chan)
		mbox_free_channel(mb->rx_chan);
	if (mb->tx_chan)
		mbox_free_channel(mb->tx_chan);

	cancel_work_sync(&mb->rx_work);

	/* TODO: drain and free any inflight mgmt messages once send is real. */
	xa_destroy(&mb->msg_xa);
}
