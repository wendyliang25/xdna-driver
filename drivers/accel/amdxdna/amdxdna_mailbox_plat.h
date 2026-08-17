/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Shared-memory + ZynqMP IPI management mailbox for amdxdna platform parts.
 *
 * The management command/response channel lives in a reserved-memory region
 * ("mgmt", split into a host-producer TX ring and a host-consumer RX ring);
 * a pair of IPI mailbox channels (tx/rx) carry interrupt notifications only.
 * The IPI carries no data -- the payload is always in shared memory.
 *
 * The doorbell channel (hw_ctx dispatch notification) is implemented
 * separately in aie4_plat.c but reuses this mailbox's TX IPI channel via
 * amdxdna_mailbox_plat_kick().
 */

#ifndef _AMDXDNA_MAILBOX_PLAT_H_
#define _AMDXDNA_MAILBOX_PLAT_H_

#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/types.h>

struct amdxdna_dev;
struct amdxdna_mailbox_plat;
struct platform_device;
struct xdna_mailbox_msg;

/*
 * HSA-aligned ring control header for the shmem SPSC transport.  head and tail
 * are 64-bit, naturally 8-byte aligned, and placed on separate 64-byte cache
 * lines to avoid false sharing.  This is the on-wire ABI shared with the remote
 * firmware; both sides must use the same layout.  It sits at the start of each
 * TX/RX ring, followed by (ring_mask + 1) bytes of ring data.
 */
struct shmem_ring_hdr {
	u64 head;			/* offset  0: producer index            */
	u64 ring_mask;			/* offset  8: (ring_data_size - 1)      */
	u64 rsvd;			/* offset 16: reserved / FW alive magic  */
	u8  _pad0[40];			/* offset 24: pad to cache line boundary */
	/* --- 64-byte cache line boundary --- */
	u64 tail;			/* offset 64: consumer index            */
	u8  _pad1[56];			/* offset 72: pad to 128-byte total     */
} __aligned(64);

static_assert(offsetof(struct shmem_ring_hdr, tail) == 64);
static_assert(sizeof(struct shmem_ring_hdr) == 128);

/*
 * Per-message header inside the management ring data area, followed by a
 * payload of total_size - sizeof(shmem_msg_hdr) bytes.  The wire format must
 * match the firmware message header so the remote can consume ring data
 * directly.
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
 * message does not fit before the ring-end boundary; the consumer skips it.
 */
#define SHMEM_TOMBSTONE	0xDEADFACE

/*
 * amdxdna_mailbox_plat_create - map the mgmt region + acquire IPI channels.
 * Returns a mailbox handle on success or an ERR_PTR on failure.  The handle is
 * drm-managed (freed with the drm device); amdxdna_mailbox_plat_destroy()
 * releases the IPI channels and pending state.
 */
struct amdxdna_mailbox_plat *
amdxdna_mailbox_plat_create(struct amdxdna_dev *xdna,
			    struct platform_device *pdev);
void amdxdna_mailbox_plat_destroy(struct amdxdna_mailbox_plat *mb);

/* Send a management message (SPSC produce + IPI). */
int amdxdna_mailbox_plat_send(struct amdxdna_mailbox_plat *mb,
			      struct xdna_mailbox_msg *msg);

/* Bare IPI kick on the shared TX channel (used by the doorbell path). */
int amdxdna_mailbox_plat_kick(struct amdxdna_mailbox_plat *mb);

#endif /* _AMDXDNA_MAILBOX_PLAT_H_ */
