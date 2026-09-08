// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Shared-memory + ZynqMP IPI implementation of the amdxdna mailbox interface
 * (amdxdna_mailbox.h) for the platform (device-tree) build.  It is the
 * compile-time-exclusive counterpart of the PCI ringbuf+MSI-X amdxdna_mailbox.c:
 * both define struct mailbox and the same external API, and exactly one is built
 * (see Kbuild).  The handle is stored in ndev->mbox like the PCI path.
 *
 * Two reserved-memory shmem regions carry the message data: "mgmt" (a TX ring
 * the host produces into and an RX ring it consumes) and "doorbell" (a ring the
 * host produces hw_ctx ids into).  A pair of ZynqMP IPI mailbox channels (tx/rx)
 * carry interrupt notification only -- the IPI has no payload.  The rings are
 * SPSC: the host owns the mgmt TX head, the mgmt RX tail and the doorbell head;
 * the remote (RPU) owns the opposite index of each.  Shared memory is mapped
 * Normal-NonCacheable (devm_ioremap_wc) and accessed as plain memory with
 * dma_wmb()/dma_rmb() ordering data against index updates.
 */

#include <asm/barrier.h>
#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "aie4.h"
#include "amdxdna_mailbox.h"
#include "amdxdna_mailbox_plat.h"
#include "amdxdna_drv.h"

/* Max response payload we consume from the mgmt RX ring (fits on the stack). */
#define SHMEM_MAX_RESP_SIZE	512
/* Max time to wait for the remote to drain a full doorbell ring. */
#define SHMEM_DB_RING_FULL_TIMEOUT_MS	1000
/* Host message ids; 0 is reserved for firmware-initiated (async) messages. */
#define SHMEM_MSG_ID_MIN	1
#define SHMEM_MSG_ID_MAX	0xfffe

/*
 * On-wire ring ABI, shared with the RPU firmware.  head and tail are 64-bit,
 * naturally aligned and placed on separate 64-byte cache lines to avoid false
 * sharing.  The ring data area (ring_mask + 1 bytes) follows the header.
 */
struct shmem_ring_hdr {
	u64 head;		/* producer index */
	u64 ring_mask;		/* ring_data_size - 1 */
	u64 rsvd;
	u8  _pad0[40];
	/* --- 64-byte cache line boundary --- */
	u64 tail;		/* consumer index */
	u8  _pad1[56];
} __aligned(64);

static_assert(offsetof(struct shmem_ring_hdr, tail) == 64);
static_assert(sizeof(struct shmem_ring_hdr) == 128);

/* Per-message header inside the mgmt ring; followed by the payload. */
#define SHMEM_MSG_BODY_SZ	GENMASK(10, 0)
#define SHMEM_MSG_PROTO_VER	GENMASK(23, 16)
#define SHMEM_PROTOCOL_VER	0x1

struct shmem_msg_hdr {
	u32 total_size;
	u32 sz_ver;
	u32 id;
	u32 opcode;
};

/*
 * Tombstone written at the tail of the ring data when a message would not fit
 * before the ring-end boundary; the consumer skips past it and wraps.  The
 * first u32 of a real message is total_size (~100 bytes max), so it can never
 * collide with the sentinel.
 */
#define SHMEM_TOMBSTONE		0xDEADFACE

/* Doorbell ring: host produces u32 hw_ctx_id slots, RPU consumes. */
struct shmem_db_ring {
	u64 head;
	u64 ring_mask;		/* num_slots - 1 */
	u64 rsvd;
	u8  _pad0[40];
	/* --- 64-byte cache line boundary --- */
	u64 tail;
	u8  _pad1[56];
	/* --- data starts at offset 128 --- */
	u32 data[];
} __aligned(64);

static_assert(offsetof(struct shmem_db_ring, tail) == 64);
static_assert(offsetof(struct shmem_db_ring, data) == 128);

/* mgmt ring produce (host -> RPU).  Returns 0 or -ENOSPC. */
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
	hdr->head = head + total;
	return 0;
}

/*
 * mgmt ring consume (RPU -> host).  @tail_cached is the host-owned tail (sole
 * consumer), read from cache and only written to shared memory.  Returns the
 * payload size, -EAGAIN when empty, -EOVERFLOW if the payload exceeds @max.
 */
static int shmem_mgmt_consume(struct shmem_ring_hdr *hdr, void *ring_base,
			      u64 ring_mask, struct shmem_msg_hdr *msg_hdr,
			      void *payload, size_t max, u64 *tail_cached)
{
	u64 size = ring_mask + 1;
	u64 head, tail, off;
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
	if (payload_size > max)
		return -EOVERFLOW;
	if (payload_size)
		memcpy(payload, ring_base + off + sizeof(*msg_hdr), payload_size);

	dma_wmb();
	tail += msg_hdr->total_size;
	hdr->tail = tail;
	*tail_cached = tail;
	return payload_size;
}

/* doorbell ring produce (host -> RPU).  @head_cached is host-owned. */
static int shmem_db_produce(struct shmem_db_ring *ring, u64 ring_mask,
			    u32 hw_ctx_id, u64 *head_cached)
{
	u64 size = ring_mask + 1;
	u64 head = *head_cached;
	u64 tail = ring->tail;

	if (head - tail >= size)
		return -ENOSPC;

	ring->data[head & ring_mask] = hw_ctx_id;
	dma_wmb();
	ring->head = head + 1;
	*head_cached = head + 1;
	return 0;
}

/* Host bookkeeping for an outstanding management request awaiting a response. */
struct shmem_inflight {
	void	*handle;
	int	(*notify_cb)(void *handle, void __iomem *data, size_t size);
};

struct mailbox {
	struct amdxdna_dev	*xdna;
	struct platform_device	*pdev;

	/* mgmt command/response shmem region, split into a TX and an RX ring */
	void __iomem		*mgmt_base;
	resource_size_t		mgmt_size;
	struct shmem_ring_hdr	*tx_hdr;
	void			*tx_ring;
	struct shmem_ring_hdr	*rx_hdr;
	void			*rx_ring;

	/* hw_ctx dispatch doorbell shmem region */
	void __iomem		*doorbell_base;
	resource_size_t		doorbell_size;
	struct shmem_db_ring	*db_ring;

	/* Ring masks, adopted from the firmware-initialized rings at create(). */
	u64			mgmt_ring_mask;
	u64			db_ring_mask;
	/* Host-owned indices, cached to avoid a non-cacheable read on the hot path. */
	u64			rx_tail_cached;	/* written only by rx_work */
	u64			db_head_cached;	/* protected by db_lock */

	/* Inflight mgmt requests keyed by message id. */
	struct xarray		msg_xa;
	u32			next_msg_id;

	spinlock_t		tx_lock; /* mgmt TX ring + IPI */
	spinlock_t		db_lock; /* doorbell ring + IPI */

	/* ZynqMP IPI channels (notification only; payload is in shared memory). */
	struct mbox_client	tx_cl;
	struct mbox_client	rx_cl;
	struct mbox_chan	*tx_chan;
	struct mbox_chan	*rx_chan;
	struct work_struct	rx_work;
	wait_queue_head_t	db_waitq; /* backpressured doorbell producers */

	/* The single mgmt channel, for firmware-initiated (id 0) messages. */
	struct mailbox_channel	*mgmt_chann;
};

struct mailbox_channel {
	struct mailbox		*mb;
	void			*async_handle;
	xdna_mailbox_async_cb_t	async_cb;
};

static int plat_map_region(struct amdxdna_dev *xdna, struct device *dev,
			   const char *name, void __iomem **base,
			   resource_size_t *size)
{
	struct device_node *np;
	struct resource res;
	int idx, ret;

	idx = of_property_match_string(dev->of_node, "memory-region-names", name);
	if (idx < 0) {
		XDNA_ERR(xdna, "memory-region '%s' not found: %d", name, idx);
		return idx;
	}

	np = of_parse_phandle(dev->of_node, "memory-region", idx);
	if (!np) {
		XDNA_ERR(xdna, "no memory-region phandle for '%s'", name);
		return -ENODEV;
	}

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret) {
		XDNA_ERR(xdna, "bad memory-region '%s': %d", name, ret);
		return ret;
	}

	*base = devm_ioremap_wc(dev, res.start, resource_size(&res));
	if (!*base) {
		XDNA_ERR(xdna, "ioremap memory-region '%s' failed", name);
		return -ENOMEM;
	}
	*size = resource_size(&res);
	XDNA_DBG(xdna, "mapped '%s' region %pa size %pa", name, &res.start, size);
	return 0;
}

/* Wake the hw_ctx completion waiters on an RX IPI (doorbell/completion). */
static void plat_mailbox_doorbell_notify(struct mailbox *mb)
{
	struct amdxdna_dev_hdl *ndev = mb->xdna->dev_handle;
	struct cert_comp *comp;
	unsigned long idx;

	xa_for_each(&ndev->cert_comp_xa, idx, comp)
		wake_up_all(&comp->waitq);

	/* The remote drained the doorbell ring; wake any backpressured producer. */
	wake_up_all(&mb->db_waitq);
}

static void plat_mailbox_rx_work(struct work_struct *work)
{
	struct mailbox *mb = container_of(work, struct mailbox, rx_work);
	struct shmem_msg_hdr hdr;
	struct shmem_inflight *ifm;
	u8 buf[SHMEM_MAX_RESP_SIZE];
	int payload_size;

	while (1) {
		payload_size = shmem_mgmt_consume(mb->rx_hdr, mb->rx_ring,
						  mb->mgmt_ring_mask, &hdr, buf,
						  sizeof(buf), &mb->rx_tail_cached);
		if (payload_size < 0)
			break;

		/* Firmware-initiated messages arrive with id 0. */
		if (!hdr.id) {
			if (mb->mgmt_chann && mb->mgmt_chann->async_cb)
				mb->mgmt_chann->async_cb(mb->mgmt_chann->async_handle,
							 hdr.opcode,
							 (void __iomem *)buf,
							 hdr.total_size);
			continue;
		}

		ifm = xa_erase(&mb->msg_xa, hdr.id);
		if (!ifm) {
			XDNA_DBG(mb->xdna, "unexpected response id %u opcode 0x%x",
				 hdr.id, hdr.opcode);
			continue;
		}

		if (ifm->notify_cb)
			ifm->notify_cb(ifm->handle, (void __iomem *)buf,
				       payload_size);
		kfree(ifm);
	}
}

/* IPI RX callback (IRQ context): wake completions, drain mgmt, re-arm the IRQ. */
static void plat_mailbox_rx_callback(struct mbox_client *cl, void *data)
{
	struct mailbox *mb = container_of(cl, struct mailbox, rx_cl);

	plat_mailbox_doorbell_notify(mb);

	/* Only kick the worker when the RPU actually queued a mgmt response. */
	dma_rmb();
	if (mb->rx_hdr->head != mb->rx_tail_cached)
		schedule_work(&mb->rx_work);

	/* ACK on the RX channel to re-enable the notification interrupt. */
	mbox_send_message(mb->rx_chan, NULL);
	mbox_client_txdone(mb->rx_chan, 0);
}

int xdna_mailbox_send_msg(struct mailbox_channel *mb_chann,
			  const struct xdna_mailbox_msg *msg, u64 tx_timeout)
{
	struct mailbox *mb = mb_chann->mb;
	struct shmem_inflight *ifm;
	struct shmem_msg_hdr hdr;
	u32 id;
	int ret;

	ifm = kzalloc(sizeof(*ifm), GFP_KERNEL);
	if (!ifm)
		return -ENOMEM;

	ifm->handle = msg->handle;
	ifm->notify_cb = msg->notify_cb;

	ret = xa_alloc_cyclic(&mb->msg_xa, &id, ifm,
			      XA_LIMIT(SHMEM_MSG_ID_MIN, SHMEM_MSG_ID_MAX),
			      &mb->next_msg_id, GFP_KERNEL);
	if (ret < 0) {
		kfree(ifm);
		return ret;
	}

	hdr.total_size = sizeof(hdr) + msg->send_size;
	hdr.sz_ver = FIELD_PREP(SHMEM_MSG_BODY_SZ, msg->send_size) |
		     FIELD_PREP(SHMEM_MSG_PROTO_VER, SHMEM_PROTOCOL_VER);
	hdr.id = id;
	hdr.opcode = msg->opcode;

	spin_lock(&mb->tx_lock);
	ret = shmem_mgmt_produce(mb->tx_hdr, mb->tx_ring, mb->mgmt_ring_mask,
				 &hdr, msg->send_data, msg->send_size);
	if (ret)
		goto unlock;

	ret = mbox_send_message(mb->tx_chan, NULL);
	if (ret < 0)
		goto unlock;
	mbox_client_txdone(mb->tx_chan, 0);
	spin_unlock(&mb->tx_lock);
	return 0;

unlock:
	spin_unlock(&mb->tx_lock);
	xa_erase(&mb->msg_xa, id);
	kfree(ifm);
	return ret;
}

int amdxdna_mailbox_plat_ring_doorbell(struct mailbox *mb, u32 hw_ctx_id)
{
	struct shmem_db_ring *ring = mb->db_ring;
	long wret;
	int ret;

	spin_lock(&mb->db_lock);

	/*
	 * If the doorbell ring is full, drop db_lock and wait for a completion
	 * IPI (which advances the remote tail and wakes db_waitq) to make room,
	 * up to a bounded timeout, rather than dropping the doorbell.
	 */
	while ((ret = shmem_db_produce(ring, mb->db_ring_mask, hw_ctx_id,
				       &mb->db_head_cached)) == -ENOSPC) {
		spin_unlock(&mb->db_lock);

		wret = wait_event_timeout(mb->db_waitq,
					  (READ_ONCE(ring->head) - READ_ONCE(ring->tail))
					  <= mb->db_ring_mask,
					  msecs_to_jiffies(SHMEM_DB_RING_FULL_TIMEOUT_MS));
		if (!wret) {
			XDNA_ERR(mb->xdna,
				 "doorbell ring full: hw_ctx_id=%u, timed out",
				 hw_ctx_id);
			return -ETIMEDOUT;
		}
		spin_lock(&mb->db_lock);
	}
	if (ret)
		goto unlock;

	ret = mbox_send_message(mb->tx_chan, NULL);
	if (ret < 0)
		goto unlock;
	mbox_client_txdone(mb->tx_chan, 0);
	ret = 0;

unlock:
	spin_unlock(&mb->db_lock);
	return ret;
}

void xdna_mailbox_drain_channel(struct mailbox_channel *mb_chann)
{
	if (!mb_chann)
		return;

	/* Run the mgmt RX path and wait for it (responses may arrive silently). */
	schedule_work(&mb_chann->mb->rx_work);
	flush_work(&mb_chann->mb->rx_work);
}

struct mailbox_channel *xdna_mailbox_alloc_channel(struct mailbox *mb)
{
	struct mailbox_channel *mb_chann;

	mb_chann = devm_kzalloc(&mb->pdev->dev, sizeof(*mb_chann), GFP_KERNEL);
	if (!mb_chann)
		return NULL;

	mb_chann->mb = mb;
	mb->mgmt_chann = mb_chann;
	return mb_chann;
}

int xdna_mailbox_start_channel(struct mailbox_channel *mb_chann,
			       const struct xdna_mailbox_chann_res *x2i,
			       const struct xdna_mailbox_chann_res *i2x,
			       u32 xdna_mailbox_intr_reg, int mb_irq)
{
	/* The shmem rings are set up at create(); nothing per-channel to start. */
	return 0;
}

void xdna_mailbox_set_async_cb(struct mailbox_channel *mb_chann,
			       void *async_handle, xdna_mailbox_async_cb_t async_cb)
{
	if (!mb_chann)
		return;

	mb_chann->async_handle = async_handle;
	mb_chann->async_cb = async_cb;
}

void xdna_mailbox_stop_channel(struct mailbox_channel *mb_chann)
{
	struct mailbox *mb;
	struct shmem_inflight *ifm;
	unsigned long id;

	if (!mb_chann)
		return;

	mb = mb_chann->mb;
	cancel_work_sync(&mb->rx_work);

	/* Complete any waiter still parked on an unanswered request. */
	xa_for_each(&mb->msg_xa, id, ifm) {
		xa_erase(&mb->msg_xa, id);
		if (ifm->notify_cb)
			ifm->notify_cb(ifm->handle, NULL, 0);
		kfree(ifm);
	}
	mb->mgmt_chann = NULL;
}

void xdna_mailbox_free_channel(struct mailbox_channel *mb_chann)
{
	/* The channel is devm-managed; nothing to free here. */
}

static int plat_mailbox_ipi_init(struct mailbox *mb)
{
	struct device *dev = &mb->pdev->dev;

	mb->tx_cl.dev = dev;
	mb->tx_cl.tx_block = false;
	mb->tx_cl.knows_txdone = true;

	mb->rx_cl.dev = dev;
	mb->rx_cl.rx_callback = plat_mailbox_rx_callback;
	mb->rx_cl.knows_txdone = true;

	mb->tx_chan = mbox_request_channel_byname(&mb->tx_cl, "tx");
	if (IS_ERR(mb->tx_chan))
		return PTR_ERR(mb->tx_chan);

	mb->rx_chan = mbox_request_channel_byname(&mb->rx_cl, "rx");
	if (IS_ERR(mb->rx_chan)) {
		mbox_free_channel(mb->tx_chan);
		return PTR_ERR(mb->rx_chan);
	}
	return 0;
}

/* devm teardown for the IPI channels (mb itself is devm-allocated). */
static void plat_mailbox_release(void *data)
{
	struct mailbox *mb = data;

	/*
	 * Free the channels first so the mbox framework stops delivering rx
	 * callbacks, then drain any work already scheduled by a callback.
	 */
	if (!IS_ERR_OR_NULL(mb->rx_chan))
		mbox_free_channel(mb->rx_chan);
	if (!IS_ERR_OR_NULL(mb->tx_chan))
		mbox_free_channel(mb->tx_chan);
	cancel_work_sync(&mb->rx_work);
	xa_destroy(&mb->msg_xa);
}

/*
 * Adopt the firmware-initialized rings: the mgmt region is split into a TX and
 * an RX ring, the doorbell region is one ring.  The head/tail indices are taken
 * from the rings as-is (the remote may already be running), not reset to zero,
 * and any responses left in the RX ring from a previous driver instance are
 * drained and dropped.
 */
static int plat_mailbox_rings_init(struct mailbox *mb)
{
	resource_size_t half = mb->mgmt_size / 2;
	struct shmem_msg_hdr hdr;
	u8 drop[SHMEM_MAX_RESP_SIZE];

	mb->tx_hdr = mb->mgmt_base;
	mb->tx_ring = (u8 *)mb->mgmt_base + sizeof(struct shmem_ring_hdr);
	mb->rx_hdr = (void *)((u8 *)mb->mgmt_base + half);
	mb->rx_ring = (u8 *)mb->mgmt_base + half + sizeof(struct shmem_ring_hdr);
	mb->db_ring = (struct shmem_db_ring *)mb->doorbell_base;

	mb->mgmt_ring_mask = mb->rx_hdr->ring_mask;
	mb->db_ring_mask = mb->db_ring->ring_mask;
	if (!mb->mgmt_ring_mask || !mb->db_ring_mask) {
		XDNA_ERR(mb->xdna, "shmem rings not initialized by firmware");
		return -ENODEV;
	}

	/* Take the host-owned indices from the rings; do not reset them. */
	mb->rx_tail_cached = mb->rx_hdr->tail;
	mb->db_head_cached = mb->db_ring->head;

	/* Drop stale responses left in the RX ring. */
	while (shmem_mgmt_consume(mb->rx_hdr, mb->rx_ring, mb->mgmt_ring_mask,
				  &hdr, drop, sizeof(drop),
				  &mb->rx_tail_cached) >= 0)
		;

	xa_init_flags(&mb->msg_xa, XA_FLAGS_ALLOC1);
	spin_lock_init(&mb->tx_lock);
	spin_lock_init(&mb->db_lock);
	init_waitqueue_head(&mb->db_waitq);
	INIT_WORK(&mb->rx_work, plat_mailbox_rx_work);
	return 0;
}

/*
 * xdnam_mailbox_create - platform (shmem+IPI) implementation.  Unlike the PCI
 * variant it derives its resources from the device tree (reserved-memory + IPI
 * mboxes) rather than @res, which is unused here.  The mgmt/doorbell memory is
 * statically reserved in the device node, so the handle, its region mappings and
 * the IPI channels are all managed by the platform device, not the drm device.
 */
struct mailbox *xdnam_mailbox_create(struct drm_device *ddev,
				     const struct xdna_mailbox_res *res)
{
	struct amdxdna_dev *xdna = to_xdna_dev(ddev);
	struct platform_device *pdev = to_platform_device(ddev->dev);
	struct device *dev = &pdev->dev;
	struct mailbox *mb;

	mb = devm_kzalloc(dev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return NULL;

	mb->xdna = xdna;
	mb->pdev = pdev;

	if (plat_map_region(xdna, dev, "mgmt", &mb->mgmt_base, &mb->mgmt_size))
		return NULL;

	if (plat_map_region(xdna, dev, "doorbell",
			    &mb->doorbell_base, &mb->doorbell_size))
		return NULL;

	if (plat_mailbox_rings_init(mb))
		return NULL;

	if (plat_mailbox_ipi_init(mb))
		return NULL;

	if (devm_add_action_or_reset(dev, plat_mailbox_release, mb))
		return NULL;

	return mb;
}
