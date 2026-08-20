/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */
#ifndef _AMDXDNA_CBUF_H_
#define _AMDXDNA_CBUF_H_

#include "amdxdna_drv.h"
#include <drm/drm_device.h>
#include <linux/dma-buf.h>
#include <linux/dma-direction.h>
#include <linux/of.h>

bool amdxdna_use_carveout(struct amdxdna_dev *xdna);
int amdxdna_carveout_init(struct amdxdna_dev *xdna, u64 carveout_addr, u64 carveout_size);
void amdxdna_carveout_fini(struct amdxdna_dev *xdna);
void amdxdna_get_carveout_conf(struct amdxdna_dev *xdna, u64 *addr, u64 *size);

/*
 * @flags carries the bank-id bitmap in bits [7:0] (bit N -> bank id N; bit 0 is
 * the firmware bank); 0 selects the first app bank on the OF platform, or the
 * debugfs carveout on x86 bring-up.
 */
struct dma_buf *amdxdna_get_cbuf(struct drm_device *dev, size_t size,
				 u64 alignment, u64 flags);

/* Bind/release the DT reserved-memory banks (rpu-cma + aie-banks/bank@N). */
int amdxdna_mem_banks_init(struct amdxdna_dev *xdna, struct device_node *np);
void amdxdna_mem_banks_fini(struct amdxdna_dev *xdna);

/*
 * Kernel-side allocation for mgmt buffers. @fw selects the firmware bank (id
 * AMDXDNA_MEM_BANK_FW, mapped through the firmware processor) for buffers the
 * firmware dereferences; otherwise the first app bank (AIE/CERT). Falls back to
 * the debugfs carveout, then system CMA, when the bank is absent (x86 bring-up).
 * Returns an opaque cookie (or ERR_PTR) and fills @vaddr / @dma_addr; free with
 * amdxdna_cbuf_kfree().
 */
void *amdxdna_cbuf_kalloc(struct amdxdna_dev *xdna, size_t size, bool fw,
			  enum dma_data_direction dir,
			  void **vaddr, dma_addr_t *dma_addr);
void amdxdna_cbuf_kfree(void *cookie);
bool amdxdna_mem_banks_present(struct amdxdna_dev *xdna);

#endif
