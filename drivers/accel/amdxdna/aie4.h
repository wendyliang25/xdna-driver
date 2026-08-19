/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 *
 * Transport-independent aie4 core: the classic device lifecycle and the
 * transport-independent query/policy helpers, shared by the PCI path
 * (aie4_pci.c) and the platform path (aie4_plat.c).  The transport-specific
 * bits are provided as compile-time hooks (exactly one definition linked per
 * build).  See docs/aie4_plat_design.md.
 */

#ifndef _AIE4_H_
#define _AIE4_H_

#include <linux/types.h>

struct amdxdna_dev;
struct amdxdna_dev_hdl;
struct amdxdna_client;
struct amdxdna_dpt;
struct amdxdna_drm_get_info;
struct amdxdna_drm_set_state;
struct amdxdna_drm_get_array;

/*
 * Transport hooks -- one definition per build (PCI in aie4_pci.c, platform in
 * aie4_plat.c), selected at link time by Kbuild (OFT_CONFIG_AMDXDNA_PCI vs
 * OFT_CONFIG_AMDXDNA_OF).
 */
int aie4_dev_setup(struct amdxdna_dev *xdna);		/* PCI: pcidev_init; plat: noop */
int aie4_fw_load(struct amdxdna_dev_hdl *ndev);		/* PCI: psp+smu;    plat: 0    */
void aie4_fw_unload(struct amdxdna_dev_hdl *ndev);	/* PCI: psp+smu;    plat: noop */
int aie4_mailbox_init(struct amdxdna_dev_hdl *ndev);	/* PCI: SRAM mbox;  plat: shmem*/
void aie4_mailbox_fini(struct amdxdna_dev_hdl *ndev);
int aie4_hw_start_quirk(struct amdxdna_dev_hdl *ndev);	/* PCI: npu3a echo; plat: 0    */
int aie4_hw_resume_prepare(struct amdxdna_dev *xdna);	/* PCI: pci_enable; plat: noop */
void aie4_hw_resume_cleanup(struct amdxdna_dev *xdna);	/* PCI: pci_disable;plat: noop */
int amdxdna_ring_ctx_doorbell(struct amdxdna_dev_hdl *ndev, u32 hw_ctx_id);
/* Install FW log/trace msg_ops (PCI wires MSI/SRAM; plat runs polling only). */
void aie4_fw_msg_ops_init(struct amdxdna_dev_hdl *ndev);

/* Shared classic lifecycle (aie4.c) -- pointed to by classic and plat ops. */
int aie4_hw_start(struct amdxdna_dev_hdl *ndev);
void aie4_hw_stop(struct amdxdna_dev_hdl *ndev);
int aie4_init(struct amdxdna_dev *xdna);
void aie4_fini(struct amdxdna_dev *xdna);
int aie4_suspend(struct amdxdna_dev *xdna);
int aie4_resume(struct amdxdna_dev *xdna);

/* Transport-independent query/policy helpers (aie4.c). */
int aie4_query_fw(struct amdxdna_dev_hdl *ndev);
int aie4_query_aie(struct amdxdna_dev_hdl *ndev);
int aie4_partition_init(struct amdxdna_dev_hdl *ndev);
void aie4_partition_fini(struct amdxdna_dev_hdl *ndev);
void aie4_restore_power_mode(struct amdxdna_dev_hdl *ndev);
void aie4_restore_force_preemption(struct amdxdna_dev_hdl *ndev);
int aie4_alloc_work_buffer(struct amdxdna_dev_hdl *ndev);
void aie4_free_work_buffer(struct amdxdna_dev_hdl *ndev);
void aie4_hwctx_suspend_all(struct amdxdna_dev_hdl *ndev, int clean_jobs);
void aie4_hwctx_cleanup_all(struct amdxdna_dev_hdl *ndev);
int aie4_hwctx_resume_all(struct amdxdna_dev_hdl *ndev);
int aie4_get_info(struct amdxdna_client *client, struct amdxdna_drm_get_info *args);
int aie4_set_state(struct amdxdna_client *client,
		   struct amdxdna_drm_set_state *args, u32 *settle_ms);
int aie4_get_array(struct amdxdna_client *client, struct amdxdna_drm_get_array *args);

/*
 * Transport-independent FW log/trace core (aie4.c). The *_init helpers return
 * the DPT handle (or an ERR_PTR) so the transport wrapper can wire its MSI/
 * io_base (PCI) or leave it polling (OF); msi_idx/msi_address may be NULL.
 */
struct amdxdna_dpt *aie4_fw_log_init(struct amdxdna_dev *xdna, size_t size,
				     u32 level, u32 *msi_idx, u32 *msi_address);
int aie4_fw_log_config(struct amdxdna_dev *xdna, u32 level);
int aie4_fw_log_fini(struct amdxdna_dev *xdna);
void aie4_fw_log_parse(struct amdxdna_dev *xdna, char *buf, size_t size);
struct amdxdna_dpt *aie4_fw_trace_init(struct amdxdna_dev *xdna, size_t size,
				       u32 categories, u32 *msi_idx, u32 *msi_address);
int aie4_fw_trace_config(struct amdxdna_dev *xdna, u32 categories);
int aie4_fw_trace_fini(struct amdxdna_dev *xdna);

/* Shared debugfs (aie4.c): common init + individual knob adders. */
void aie4_debugfs_init(struct amdxdna_dev *xdna);
void aie4_debugfs_add_kernel_submit(struct amdxdna_dev *xdna);
void aie4_debugfs_add_hysteresis(struct amdxdna_dev *xdna);

#endif /* _AIE4_H_ */
