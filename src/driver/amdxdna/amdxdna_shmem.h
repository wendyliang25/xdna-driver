/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Shared-memory ring layout and SPSC helpers for the amdxdna shmem+IPI
 * transport.
 *
 * Shared memory is mapped via devm_ioremap_wc() (Normal Non-Cacheable
 * on ARM64).  Bulk data copies use memcpy_toio/memcpy_fromio (the
 * kernel-sanctioned accessors for IO-mapped memory); single u64 index
 * updates use plain stores with natural 8-byte alignment guaranteeing
 * atomicity on AArch64.  dma_wmb()/dma_rmb() order data vs. index
 * updates across the non-coherent boundary.
 * Both sides are little-endian, so no byte-swap is needed.
 */

#ifndef _AMDXDNA_SHMEM_H_
#define _AMDXDNA_SHMEM_H_

#include <asm/barrier.h>
#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/types.h>

struct amdxdna_dev;

int amdxdna_shmem_init(struct amdxdna_dev *xdna, struct platform_device *pdev);
void amdxdna_shmem_fini(struct amdxdna_dev *xdna);
/*
 * HSA-aligned ring control header for shmem SPSC transport.
 *
 * Follows the HSA queue convention: write_index and read_index are 64-bit,
 * naturally 8-byte aligned, and placed on separate 64-byte cache lines to
 * eliminate false sharing between producer and consumer cores.
 *
 * This struct is the on-wire ABI shared between APU (Linux) and RPU
 * (Zephyr).  Both sides must use the same layout.  Sits at the start of
 * each TX/RX; followed by (ring_mask + 1) bytes of ring data.
 */
struct shmem_ring_hdr {
	u64 write_index;		/* offset  0: producer index           */
	u64 ring_mask;			/* offset  8: (ring_data_size - 1)     */
	u64 rsvd;			/* offset 16: reserved / FW alive magic */
	u8  _pad0[40];			/* offset 24: pad to cache line boundary */
	/* --- 64-byte cache line boundary --- */
	u64 read_index;			/* offset 64: consumer index           */
	u8  _pad1[56];			/* offset 72: pad to 128-byte total    */
} __aligned(64);

static_assert(offsetof(struct shmem_ring_hdr, read_index) == 64);
static_assert(sizeof(struct shmem_ring_hdr) == 128);

/*
 * Per-message header inside the management ring data area.
 * Followed by payload of total_size - sizeof(shmem_msg_hdr) bytes.
 *
 * Wire format must match npu_mbox_msg_header (FW) and xdna_msg_header
 * (PCI mailbox) exactly so npu_msg_process() can consume ring data
 * directly without adaptation.
 */
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
 * Tombstone sentinel for ring wrap.  Written at the current offset when a
 * message does not fit before the ring-end boundary; consumer skips past it.
 *
 * The first u32 of every real message is total_size (sizeof(shmem_msg_hdr) +
 * payload), which is at most ~100 bytes for current opcodes and can never
 * collide with 0xDEADFACE.
 */
#define SHMEM_TOMBSTONE	0xDEADFACE

/*
 * HSA-aligned doorbell ring for hw_ctx dispatch notification.
 *
 * Same cache-line-separated index layout as shmem_ring_hdr.
 * The flexible data[] array (u32 hw_ctx_id slots) begins at offset 128.
 * Linux produces; RPU firmware consumes.
 */
struct shmem_db_ring {
	u64 write_index;		/* offset  0: producer index       */
	u64 ring_mask;			/* offset  8: (num_slots - 1)      */
	u64 rsvd;			/* offset 16: reserved             */
	u8  _pad0[40];			/* offset 24: pad to cache line    */
	/* --- 64-byte cache line boundary --- */
	u64 read_index;			/* offset 64: consumer index       */
	u8  _pad1[56];			/* offset 72: pad to 128 bytes     */
	/* --- data starts at offset 128 --- */
	u32 data[];
} __aligned(64);

static_assert(offsetof(struct shmem_db_ring, read_index) == 64);
static_assert(offsetof(struct shmem_db_ring, data) == 128);

/*
 * Management ring -- produce (host writes to TX ring).
 *
 * @hdr:              ring header in shared memory
 * @ring_base:        ring data area (right after the header)
 * @local_write_idx:  caller's cached write_index (updated on return)
 * @cached_read_idx:  caller's cached copy of firmware's read_index
 * @msg_hdr:          message header values
 * @payload:          message payload (kernel memory)
 * @payload_size:     payload size in bytes
 *
 * Returns 0 on success, -ENOSPC if ring is full.
 *
 * If the message does not fit between the current offset and the ring-end
 * boundary, a TOMBSTONE sentinel is written and the offset resets to 0.
 */
static inline int shmem_mgmt_produce(struct shmem_ring_hdr *hdr,
				     void *ring_base,
				     u64 *local_write_idx, u64 *cached_read_idx,
				     const struct shmem_msg_hdr *msg_hdr,
				     const void *payload, size_t payload_size)
{
	u64 mask = hdr->ring_mask;
	u64 size = mask + 1;
	u64 head = *local_write_idx;
	u32 total = sizeof(*msg_hdr) + payload_size;
	u64 off, gap;

	if (head - *cached_read_idx + total > size) {
		*cached_read_idx = hdr->read_index;
		if (head - *cached_read_idx + total > size)
			return -ENOSPC;
	}

	off = head & mask;

	if (off + total > size) {
		gap = size - off;
		*(u32 *)(ring_base + off) = SHMEM_TOMBSTONE;
		head += gap;

		if (head - *cached_read_idx + total > size) {
			*cached_read_idx = hdr->read_index;
			if (head - *cached_read_idx + total > size)
				return -ENOSPC;
		}
		off = head & mask;
	}

	memcpy_toio(ring_base + off, msg_hdr, sizeof(*msg_hdr));
	if (payload_size)
		memcpy_toio(ring_base + off + sizeof(*msg_hdr),
			    payload, payload_size);

	dma_wmb();

	head += total;
	hdr->write_index = head;
	*local_write_idx = head;

	return 0;
}

/*
 * Management ring -- consume (host reads from RX ring).
 *
 * @hdr:               ring header in shared memory
 * @ring_base:         ring data area
 * @local_read_idx:    caller's cached read_index (updated on return)
 * @cached_write_idx:  caller's cached copy of firmware's write_index
 * @msg_hdr:           output message header
 * @payload:           output buffer for payload (kernel memory)
 * @payload_max:       maximum payload bytes to copy
 *
 * Returns payload size on success, -EAGAIN if ring is empty, -EOVERFLOW
 * if the message payload exceeds payload_max.
 *
 * If a TOMBSTONE sentinel is found at the current offset, the consumer
 * skips past the remaining ring-end gap and retries from offset 0.
 */
static inline int shmem_mgmt_consume(struct shmem_ring_hdr *hdr,
				     void *ring_base,
				     u64 *local_read_idx, u64 *cached_write_idx,
				     struct shmem_msg_hdr *msg_hdr,
				     void *payload, size_t payload_max)
{
	u64 mask = hdr->ring_mask;
	u64 size = mask + 1;
	u64 tail = *local_read_idx;
	u64 off;
	u32 payload_size;

	if (*cached_write_idx == tail) {
		*cached_write_idx = hdr->write_index;
		if (*cached_write_idx == tail)
			return -EAGAIN;
	}

	dma_rmb();

	off = tail & mask;

	if (*(u32 *)(ring_base + off) == SHMEM_TOMBSTONE) {
		tail += size - off;
		off = tail & mask;

		*cached_write_idx = hdr->write_index;
		if (*cached_write_idx == tail)
			return -EAGAIN;

		dma_rmb();
	}

	memcpy_fromio(msg_hdr, ring_base + off, sizeof(*msg_hdr));

	payload_size = msg_hdr->total_size - sizeof(*msg_hdr);
	if (payload_size > payload_max)
		return -EOVERFLOW;

	if (payload_size)
		memcpy_fromio(payload, ring_base + off + sizeof(*msg_hdr),
			      payload_size);

	dma_wmb();

	tail += msg_hdr->total_size;
	hdr->read_index = tail;
	*local_read_idx = tail;

	return payload_size;
}

/*
 * Doorbell ring -- produce (host writes hw_ctx_id).
 *
 * @ring:             doorbell ring in shared memory
 * @local_write_idx:  caller's cached write_index (updated on return)
 * @cached_read_idx:  caller's cached copy of firmware's read_index
 * @hw_ctx_id:        hardware context ID to write
 *
 * Returns 0 on success, -ENOSPC if ring is full.
 */
static inline int shmem_db_produce(struct shmem_db_ring *ring,
				   u64 *local_write_idx, u64 *cached_read_idx,
				   u32 hw_ctx_id)
{
	u64 mask = ring->ring_mask;
	u64 size = mask + 1;
	u64 head = *local_write_idx;

	if (head - *cached_read_idx >= size) {
		*cached_read_idx = ring->read_index;
		if (head - *cached_read_idx >= size)
			return -ENOSPC;
	}

	ring->data[head & mask] = hw_ctx_id;

	dma_wmb();

	head++;
	ring->write_index = head;
	*local_write_idx = head;

	return 0;
}

#endif /* _AMDXDNA_SHMEM_H_ */
