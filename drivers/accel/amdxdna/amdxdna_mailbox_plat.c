// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Shared-memory + ZynqMP IPI management mailbox for amdxdna platform parts.
 *
 * Platform implementation of the opaque struct mailbox_channel: it maps the
 * "mgmt" reserved-memory region (host-producer TX ring + host-consumer RX ring),
 * acquires the tx/rx IPI mailbox channels, and implements the channel API used
 * by the shared aie_send_mgmt_msg_wait() path (xdna_mailbox_send_msg/
 * stop_channel/free_channel).  The IPI is a bufferless notification: the
 * command/response payload always lives in shared memory.
 *
 * Data path: xdna_mailbox_send_msg() SPSC-produces a {header, payload} into the
 * TX ring and kicks the TX IPI; the remote consumes it, writes a response into
 * the RX ring and kicks the RX IPI; rx_callback() (IRQ) schedules rx_work, which
 * SPSC-consumes the response, matches it to the inflight message by id and runs
 * its notify_cb (which completes the waiter in amdxdna_mailbox_helper.c).
 */

#include <drm/drm_managed.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/log2.h>
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

/* Maximum response payload we expect from firmware (fits on the kernel stack). */
#define SHMEM_MAX_RESP_SIZE	512

struct shmem_inflight_msg {
	u32		id;
	void		*handle;
	int		(*notify_cb)(void *handle, void __iomem *data, size_t size);
};

/*
 * Platform struct mailbox_channel.  This is the opaque type the shared code
 * (amdxdna_mailbox_helper.c, aie.c) holds as aie->mgmt_chann; only this file
 * knows its layout.
 */
struct mailbox_channel {
	struct amdxdna_dev	*xdna;
	struct platform_device	*pdev;

	/* Mgmt shared memory region mapped from device tree reserved-memory */
	void			*mgmt_shmem;
	resource_size_t		mgmt_shmem_size;

	/* Mgmt TX ring (host is producer) / RX ring (host is consumer) */
	struct shmem_ring_hdr	*tx_hdr;
	void			*tx_ring;
	struct shmem_ring_hdr	*rx_hdr;
	void			*rx_ring;

	/* Cached ring mask (constant after init) and host-owned RX tail */
	u64			ring_mask;
	u64			rx_tail_cached;

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

/*
 * Management ring -- produce (host writes to TX ring).  head and tail are read
 * fresh from shared memory (stateless indices).  Returns 0 on success, -ENOSPC
 * if the ring is full.
 */
static int shmem_mgmt_produce(struct shmem_ring_hdr *hdr, void *ring_base,
			      u64 ring_mask, const struct shmem_msg_hdr *msg_hdr,
			      const void *payload, size_t payload_size)
{
	u64 size = ring_mask + 1;
	u64 head = hdr->head;
	u64 tail = hdr->tail;
	u32 total = sizeof(*msg_hdr) + payload_size;
	u64 off, gap;

	if (head - tail + total > size)
		return -ENOSPC;

	off = head & ring_mask;

	if (off + total > size) {
		gap = size - off;
		*(u32 *)(ring_base + off) = SHMEM_TOMBSTONE;
		head += gap;

		if (head - tail + total > size)
			return -ENOSPC;
		off = head & ring_mask;
	}

	memcpy(ring_base + off, msg_hdr, sizeof(*msg_hdr));
	if (payload_size)
		memcpy(ring_base + off + sizeof(*msg_hdr), payload, payload_size);

	dma_wmb();

	head += total;
	hdr->head = head;

	return 0;
}

/*
 * Management ring -- consume (host reads from RX ring).  The host owns tail
 * (sole consumer), so it is read from the cache and only written to shared
 * memory.  head is owned by the remote and read fresh.  Returns payload size on
 * success, -EAGAIN if empty, -EOVERFLOW if payload exceeds payload_max.
 */
static int shmem_mgmt_consume(struct shmem_ring_hdr *hdr, void *ring_base,
			      u64 ring_mask, struct shmem_msg_hdr *msg_hdr,
			      void *payload, size_t payload_max, u64 *tail_cached)
{
	u64 size = ring_mask + 1;
	u64 head, tail;
	u64 off;
	u32 payload_size;

	dma_rmb();
	head = hdr->head;
	tail = *tail_cached;

	if (head == tail)
		return -EAGAIN;

	off = tail & ring_mask;

	if (*(u32 *)(ring_base + off) == SHMEM_TOMBSTONE) {
		tail += size - off;
		if (tail == head) {
			hdr->tail = tail;
			*tail_cached = tail;
			dma_wmb();
			return -EAGAIN;
		}
		off = tail & ring_mask;
		dma_rmb();
	}

	memcpy(msg_hdr, ring_base + off, sizeof(*msg_hdr));

	payload_size = msg_hdr->total_size - sizeof(*msg_hdr);
	if (payload_size > payload_max)
		return -EOVERFLOW;

	if (payload_size)
		memcpy(payload, ring_base + off + sizeof(*msg_hdr), payload_size);

	dma_wmb();

	tail += msg_hdr->total_size;
	hdr->tail = tail;
	*tail_cached = tail;

	return payload_size;
}

static void amdxdna_mailbox_plat_rx_work(struct work_struct *work)
{
	struct mailbox_channel *mb =
		container_of(work, struct mailbox_channel, rx_work);
	struct shmem_inflight_msg *ifm;
	struct shmem_msg_hdr msg_hdr;
	u8 buf[SHMEM_MAX_RESP_SIZE];
	int payload_size;

	/* Drain all available management responses. */
	while (mb->rx_hdr) {
		payload_size = shmem_mgmt_consume(mb->rx_hdr, mb->rx_ring,
						  mb->ring_mask, &msg_hdr,
						  buf, sizeof(buf),
						  &mb->rx_tail_cached);
		if (payload_size < 0)
			break;

		ifm = xa_erase(&mb->msg_xa, msg_hdr.id);
		if (!ifm) {
			dev_dbg(&mb->pdev->dev,
				"unexpected response id %u opcode 0x%x\n",
				msg_hdr.id, msg_hdr.opcode);
			continue;
		}

		/* Cast to __iomem for the xdna_mailbox_msg callback signature;
		 * buf is normal memory so memcpy_fromio() in the cb is a plain copy.
		 */
		if (ifm->notify_cb)
			ifm->notify_cb(ifm->handle, (void __iomem *)buf,
				       payload_size);

		kfree(ifm);
	}
}

/*
 * Called by the mailbox framework when the remote sends an IPI to us (RX
 * channel callback).  Runs in IRQ context.  Schedule the drain only when the
 * remote has actually queued a response, then ACK the IPI to re-arm the
 * notification interrupt.
 */
static void amdxdna_mailbox_plat_rx_callback(struct mbox_client *cl, void *data)
{
	struct mailbox_channel *mb =
		container_of(cl, struct mailbox_channel, rx_cl);

	if (mb->rx_hdr->head != mb->rx_tail_cached)
		schedule_work(&mb->rx_work);

	/* Re-arm the notification interrupt via an ACK on the RX channel. */
	mbox_send_message(mb->rx_chan, NULL);
	mbox_client_txdone(mb->rx_chan, 0);
}

int xdna_mailbox_send_msg(struct mailbox_channel *mb_chann,
			  const struct xdna_mailbox_msg *msg, u64 tx_timeout)
{
	struct mailbox_channel *mb = mb_chann;
	struct shmem_inflight_msg *ifm;
	struct shmem_msg_hdr hdr;
	u32 id;
	int ret;

	if (msg->send_size > FIELD_MAX(SHMEM_MSG_BODY_SZ))
		return -EINVAL;

	ifm = kzalloc_obj(*ifm);
	if (!ifm)
		return -ENOMEM;

	ifm->handle = msg->handle;
	ifm->notify_cb = msg->notify_cb;

	spin_lock(&mb->msg_id_lock);
	id = mb->next_msg_id++;
	ifm->id = id;
	ret = xa_insert(&mb->msg_xa, id, ifm, GFP_ATOMIC);
	spin_unlock(&mb->msg_id_lock);
	if (ret) {
		kfree(ifm);
		return ret;
	}

	hdr.total_size = sizeof(hdr) + msg->send_size;
	hdr.sz_ver = FIELD_PREP(SHMEM_MSG_BODY_SZ, msg->send_size) |
		     FIELD_PREP(SHMEM_MSG_PROTO_VER, SHMEM_PROTOCOL_VER);
	hdr.id = id;
	hdr.opcode = msg->opcode;

	spin_lock(&mb->tx_lock);
	ret = shmem_mgmt_produce(mb->tx_hdr, mb->tx_ring, mb->ring_mask,
				 &hdr, msg->send_data, msg->send_size);
	if (ret)
		goto unlock_tx;

	ret = mbox_send_message(mb->tx_chan, NULL);
	if (ret < 0)
		goto unlock_tx;
	mbox_client_txdone(mb->tx_chan, 0);
	spin_unlock(&mb->tx_lock);

	return 0;

unlock_tx:
	spin_unlock(&mb->tx_lock);
	xa_erase(&mb->msg_xa, id);
	kfree(ifm);
	return ret;
}

int amdxdna_mailbox_plat_kick(struct mailbox_channel *mb_chann)
{
	int ret;

	ret = mbox_send_message(mb_chann->tx_chan, NULL);
	if (ret < 0)
		return ret;
	mbox_client_txdone(mb_chann->tx_chan, 0);
	return 0;
}

void xdna_mailbox_stop_channel(struct mailbox_channel *mb_chann)
{
	if (!mb_chann)
		return;

	/* No more RX draining after this returns. */
	cancel_work_sync(&mb_chann->rx_work);
}

void xdna_mailbox_free_channel(struct mailbox_channel *mb_chann)
{
	struct shmem_inflight_msg *ifm;
	unsigned long idx;

	if (!mb_chann)
		return;

	if (mb_chann->rx_chan)
		mbox_free_channel(mb_chann->rx_chan);
	if (mb_chann->tx_chan)
		mbox_free_channel(mb_chann->tx_chan);

	xa_for_each(&mb_chann->msg_xa, idx, ifm) {
		xa_erase(&mb_chann->msg_xa, idx);
		kfree(ifm);
	}
	xa_destroy(&mb_chann->msg_xa);
}

static void amdxdna_mailbox_plat_rings_init(struct mailbox_channel *mb)
{
	resource_size_t half = mb->mgmt_shmem_size / 2;
	resource_size_t ring_data;

	/* Split the mgmt region: first half is TX, second half is RX. */
	mb->tx_hdr = mb->mgmt_shmem;
	mb->tx_ring = mb->mgmt_shmem + sizeof(struct shmem_ring_hdr);
	mb->rx_hdr = mb->mgmt_shmem + half;
	mb->rx_ring = mb->mgmt_shmem + half + sizeof(struct shmem_ring_hdr);

	/*
	 * Ring data area is the half minus the header, rounded down to the
	 * largest power-of-2 so the mask has all lower bits set.
	 */
	ring_data = rounddown_pow_of_two(half - sizeof(struct shmem_ring_hdr));
	mb->tx_hdr->head = 0;
	mb->tx_hdr->tail = 0;
	mb->tx_hdr->ring_mask = ring_data - 1;
	mb->tx_hdr->rsvd = 0;

	mb->rx_hdr->head = 0;
	mb->rx_hdr->tail = 0;
	mb->rx_hdr->ring_mask = ring_data - 1;
	mb->rx_hdr->rsvd = 0;

	mb->ring_mask = ring_data - 1;
	mb->rx_tail_cached = 0;

	xa_init(&mb->msg_xa);
	spin_lock_init(&mb->msg_id_lock);
	spin_lock_init(&mb->tx_lock);
	mb->next_msg_id = 0;

	INIT_WORK(&mb->rx_work, amdxdna_mailbox_plat_rx_work);
}

static int amdxdna_mailbox_plat_map_mgmt(struct mailbox_channel *mb)
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

static int amdxdna_mailbox_plat_ipi_init(struct mailbox_channel *mb)
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

struct mailbox_channel *
amdxdna_mailbox_plat_create(struct amdxdna_dev *xdna,
			    struct platform_device *pdev)
{
	struct mailbox_channel *mb;
	int ret;

	mb = drmm_kzalloc(&xdna->ddev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return ERR_PTR(-ENOMEM);

	mb->xdna = xdna;
	mb->pdev = pdev;

	ret = amdxdna_mailbox_plat_map_mgmt(mb);
	if (ret)
		return ERR_PTR(ret);

	amdxdna_mailbox_plat_rings_init(mb);

	ret = amdxdna_mailbox_plat_ipi_init(mb);
	if (ret)
		return ERR_PTR(ret);

	dev_info(&pdev->dev, "amdxdna platform mgmt mailbox created\n");
	return mb;
}
