/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * aie4-over-platform device ops and shared-memory + IPI doorbell.
 *
 * The aie4 message protocol runs over the platform shmem+IPI transport on
 * non-PCI (aie2ps / "ve2") SoC parts.  It reuses the same struct amdxdna_dev_hdl
 * as the PCI path, so the shared aie4 command/query/error flows work unchanged;
 * only hardware bring-up differs (no firmware load, no PSP/SMU, no BARs).  The
 * management mailbox is in amdxdna_mailbox_plat.c; the hw_ctx dispatch doorbell
 * is implemented here and notifies the remote via the mailbox's TX IPI channel.
 * See docs/aie4_plat_design.md.
 */

#ifndef _AIE4_PLAT_H_
#define _AIE4_PLAT_H_

#include <linux/build_bug.h>
#include <linux/types.h>

struct amdxdna_dev;
struct amdxdna_dev_hdl;
struct amdxdna_dev_ops;

/*
 * HSA-aligned doorbell ring for hw_ctx dispatch notification.  Same
 * cache-line-separated index layout as the mgmt ring header.  The flexible
 * data[] array (u32 hw_ctx_id slots) begins at offset 128.  Linux produces;
 * the remote firmware consumes.  This is the on-wire ABI shared with firmware.
 */
struct shmem_db_ring {
	u64 head;			/* offset  0: producer index    */
	u64 ring_mask;			/* offset  8: (num_slots - 1)   */
	u64 rsvd;			/* offset 16: reserved          */
	u8  _pad0[40];			/* offset 24: pad to cache line */
	/* --- 64-byte cache line boundary --- */
	u64 tail;			/* offset 64: consumer index    */
	u8  _pad1[56];			/* offset 72: pad to 128 bytes  */
	/* --- data starts at offset 128 --- */
	u32 data[];
} __aligned(64);

static_assert(offsetof(struct shmem_db_ring, tail) == 64);
static_assert(offsetof(struct shmem_db_ring, data) == 128);

/*
 * Allocate and attach the shared aie4 device handle to @xdna (drm-managed),
 * initialised for the platform transport.  Returns the handle or NULL.
 */
struct amdxdna_dev_hdl *aie4_plat_ndev_alloc(struct amdxdna_dev *xdna);

extern const struct amdxdna_dev_ops aie4_plat_ops;

#endif /* _AIE4_PLAT_H_ */
