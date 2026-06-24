// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include "drm/amdxdna_accel.h"
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>
#include <linux/overflow.h>
#include <linux/sched/mm.h>
#include <linux/types.h>

#include "aie.h"
#include "aie4_host_queue.h"
#include "aie4_pci.h"
#include "aie4_msg_priv.h"
#include "amdxdna_ctx.h"
#include "amdxdna_gem.h"
#include "amdxdna_mailbox.h"
#include "amdxdna_mailbox_helper.h"
#include "amdxdna_pci_drv.h"
#include "trace/events/amdxdna.h"

#define CTX_INVALID_ID			(~0U)
#define CTX_INVALID_DOORBELL		(~0U)

/* Max sub-commands in one ERT_CMD_CHAIN, matching the user-mode shim cap. */
#define MAX_CHAINED_SUB_CMD		64

static int kernel_mode_submission = 1;
module_param(kernel_mode_submission, int, 0600);
MODULE_PARM_DESC(kernel_mode_submission,
		 "aie4 I/O submission: 0 - by user, 1 - by driver (default)");

#define KERNEL_SUBMIT_TIMEOUT_MS_DEFAULT	2000

static u32 kernel_submit_timeout_ms = KERNEL_SUBMIT_TIMEOUT_MS_DEFAULT;
module_param(kernel_submit_timeout_ms, uint, 0600);
MODULE_PARM_DESC(kernel_submit_timeout_ms,
		 "aie4 kernel-mode per-job completion timeout in ms (0 - use default)");

static void job_worker(struct work_struct *work);
static void aie4_hwctx_cleanup_running_jobs(struct amdxdna_hwctx *hwctx);

static irqreturn_t cert_comp_isr(int irq, void *p)
{
	struct cert_comp *cert_comp = p;

	wake_up_all(&cert_comp->waitq);
	return IRQ_HANDLED;
}

static int aie4_link_cert_comp(struct amdxdna_hwctx *hwctx, u32 msix_idx)
{
	struct amdxdna_dev_hdl *ndev = hwctx->client->xdna->dev_handle;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	struct pci_dev *pdev = to_pci_dev(xdna->ddev.dev);
	struct cert_comp *cert_comp;
	int ret;

	guard(mutex)(&ndev->cert_comp_lock);

	cert_comp = xa_load(&ndev->cert_comp_xa, msix_idx);
	if (cert_comp) {
		kref_get(&cert_comp->kref);
		priv->cert_comp = cert_comp;
		return 0;
	}

	cert_comp = kzalloc_obj(*cert_comp);
	if (!cert_comp)
		return -ENOMEM;

	cert_comp->ndev = ndev;
	cert_comp->msix_idx = msix_idx;
	cert_comp->irq = -ENOENT;
	init_waitqueue_head(&cert_comp->waitq);
	kref_init(&cert_comp->kref);

	ret = pci_irq_vector(pdev, cert_comp->msix_idx);
	if (ret < 0) {
		XDNA_ERR(xdna, "MSI-X idx %u is invalid, ret:%d", msix_idx, ret);
		goto free_cert_comp;
	}
	cert_comp->irq = ret;

	ret = request_irq(cert_comp->irq, cert_comp_isr, 0, "xdna_hsa", cert_comp);
	if (ret) {
		XDNA_ERR(xdna, "request irq %d failed %d", cert_comp->irq, ret);
		cert_comp->irq = -ENOENT;
		goto free_cert_comp;
	}

	ret = xa_err(xa_store(&ndev->cert_comp_xa, msix_idx, cert_comp, GFP_KERNEL));
	if (ret) {
		XDNA_ERR(xdna, "store cert_comp for msix idx %d failed %d", msix_idx, ret);
		goto free_irq;
	}

	priv->cert_comp = cert_comp;
	return 0;

free_irq:
	free_irq(cert_comp->irq, cert_comp);
free_cert_comp:
	kfree(cert_comp);
	return -ENODEV;
}

static void cert_comp_release(struct kref *kref)
{
	struct cert_comp *cert_comp = container_of(kref, struct cert_comp, kref);
	struct amdxdna_dev_hdl *ndev = cert_comp->ndev;

	xa_erase(&ndev->cert_comp_xa, cert_comp->msix_idx);
	if (cert_comp->irq >= 0)
		free_irq(cert_comp->irq, cert_comp);
	kfree(cert_comp);
}

static void aie4_put_cert_comp(struct cert_comp *cert_comp)
{
	struct amdxdna_dev_hdl *ndev = cert_comp->ndev;

	guard(mutex)(&ndev->cert_comp_lock);

	kref_put(&cert_comp->kref, cert_comp_release);
}

static struct cert_comp *aie4_get_cert_comp(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev_hdl *ndev = hwctx->client->xdna->dev_handle;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	guard(mutex)(&ndev->cert_comp_lock);

	if (!priv->cert_comp)
		return NULL;

	kref_get(&priv->cert_comp->kref);
	return priv->cert_comp;
}

static void aie4_unlink_cert_comp(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_dev_hdl *ndev = hwctx->client->xdna->dev_handle;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct cert_comp *cert_comp;

	guard(mutex)(&ndev->cert_comp_lock);

	cert_comp = priv->cert_comp;
	/* unlink hwctx_priv link with cert_comp */
	priv->cert_comp = NULL;

	if (cert_comp) {
		wake_up_all(&cert_comp->waitq);
		kref_put(&cert_comp->kref, cert_comp_release);
	}
}

static int aie4_msg_destroy_context(struct amdxdna_dev_hdl *ndev, u32 hw_context_id)
{
	DECLARE_AIE_MSG(aie4_msg_destroy_hw_context, AIE4_MSG_OP_DESTROY_HW_CONTEXT);

	req.hw_context_id = hw_context_id;
	return aie_send_mgmt_msg_wait(&ndev->aie, &msg);
}

static u32 aie4_parse_priority_to_dev(u32 priority)
{
	switch (priority) {
	case AMDXDNA_QOS_LOW_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_IDLE;
	case AMDXDNA_QOS_NORMAL_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_NORMAL;
	case AMDXDNA_QOS_HIGH_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_FOCUS;
	case AMDXDNA_QOS_REALTIME_PRIORITY:
		return AIE4_CONTEXT_PRIORITY_BAND_REAL_TIME;
	default:
		return AIE4_CONTEXT_PRIORITY_BAND_NORMAL;
	}
}

int aie4_hwctx_create(struct amdxdna_hwctx *hwctx)
{
	DECLARE_AIE_MSG(aie4_msg_create_hw_context, AIE4_MSG_OP_CREATE_HW_CONTEXT);
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	int ret;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	if (!ndev->partition_id || !hwctx->num_tiles) {
		XDNA_ERR(xdna, "invalid request partition_id %d, num_tiles %d",
			 ndev->partition_id, hwctx->num_tiles);
		return -EINVAL;
	}

	req.partition_id = ndev->partition_id;
	req.request_num_tiles = hwctx->num_tiles;
	req.pasid = aie4_msg_pasid(client);
	req.priority_band = aie4_parse_priority_to_dev(hwctx->qos.priority);
	req.hsa_addr_high = upper_32_bits(amdxdna_gem_dev_addr(priv->umq_bo));
	req.hsa_addr_low = lower_32_bits(amdxdna_gem_dev_addr(priv->umq_bo));

	XDNA_DBG(xdna, "pasid 0x%x, num_tiles %d, hsa[0x%x 0x%x]",
		 req.pasid, req.request_num_tiles, req.hsa_addr_high, req.hsa_addr_low);

	ret = aie_send_mgmt_msg_wait(&ndev->aie, &msg);
	if (ret) {
		XDNA_ERR(xdna, "create ctx failed: %d", ret);
		return ret;
	}

	XDNA_DBG(xdna, "resp msix: %d, ctx id: %d, doorbell: %d",
		 resp.job_complete_msix_idx, resp.hw_context_id,
		 resp.doorbell_offset);

	if (priv->kernel_submit) {
		struct pci_dev *pdev = to_pci_dev(xdna->ddev.dev);
		u64 db_off = (u64)ndev->priv->doorbell_off + resp.doorbell_offset;

		/*
		 * doorbell_base is a pcim_iomap() of the whole doorbell BAR.  The
		 * doorbell offset comes from firmware (or, on a VF, the PF/hypervisor);
		 * reject one that would place the u32 doorbell write past the mapped
		 * BAR before ring_doorbell() ever dereferences priv->doorbell_addr.
		 * Mirrors the bounds check on the user mmap path (aie4_doorbell_mmap).
		 */
		if (db_off + sizeof(u32) >
		    pci_resource_len(pdev, xdna->dev_info->doorbell_bar)) {
			XDNA_ERR(xdna, "doorbell offset 0x%llx out of BAR", db_off);
			aie4_msg_destroy_context(ndev, resp.hw_context_id);
			return -EINVAL;
		}
	}

	if (ndev->aie.force_preempt_enabled) {
		ret = aie4_force_preemption(ndev);
		if (ret) {
			XDNA_ERR(xdna, "failed to enable force preempt: %d", ret);
			aie4_msg_destroy_context(ndev, resp.hw_context_id);
			return ret;
		}
	}

	/* setup interrupt completion per msix index */
	ret = aie4_link_cert_comp(hwctx, resp.job_complete_msix_idx);
	if (ret) {
		aie4_msg_destroy_context(ndev, resp.hw_context_id);
		return ret;
	}

	priv->hw_ctx_id = resp.hw_context_id;

	if (priv->kernel_submit) {
		/*
		 * Kernel-mode submission: point at this context's doorbell within
		 * the device-level doorbell BAR mapping (fixed for the device's
		 * lifetime) so the driver can ring it, and keep it out of user
		 * space (hand back an invalid offset so the doorbell cannot be
		 * mmap'd/rung by the user).
		 */
		/*
		 * Publish doorbell_addr and CONNECTED together under io_lock so a
		 * concurrent kernel-mode submitter (which reads status and rings the
		 * doorbell under io_lock) cannot, on a TDR recreate of a live ctx,
		 * observe CONNECTED with a stale doorbell_addr on a weakly-ordered
		 * architecture and ring writel(0, NULL).
		 */
		mutex_lock(&priv->io_lock);
		priv->doorbell_addr = ndev->doorbell_base +
				      ndev->priv->doorbell_off + resp.doorbell_offset;
		priv->status = CTX_STATE_CONNECTED;
		mutex_unlock(&priv->io_lock);
		hwctx->doorbell_offset = CTX_INVALID_DOORBELL;
	} else {
		/* User-mode submission: hand the doorbell to user space to ring. */
		hwctx->doorbell_offset = resp.doorbell_offset;
		priv->status = CTX_STATE_CONNECTED;
	}

	return 0;
}

void aie4_hwctx_destroy(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	int ret;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	/*
	 * Mark disconnected and drop the doorbell under io_lock so the job worker
	 * observes the teardown (and stops waiting on read_index), and so a
	 * concurrent kernel-mode submitter - which only holds hwctx_srcu, not
	 * dev_lock - cannot pass submit_one_cmd()'s status check and then ring a
	 * NULL doorbell while this context is torn down on the TDR/reset path.
	 * Mirrors the serialized transition in job_timeout().  doorbell_base is a
	 * device-level managed mapping, so just drop the pointer.
	 */
	if (priv->kernel_submit) {
		mutex_lock(&priv->io_lock);
		priv->status = CTX_STATE_DISCONNECTED;
		priv->doorbell_addr = NULL;
		mutex_unlock(&priv->io_lock);
	} else {
		/* User-mode submission has no job worker, so io_lock is not
		 * initialized; the lockless status store is safe here.
		 */
		priv->status = CTX_STATE_DISCONNECTED;
	}

	ret = aie4_msg_destroy_context(ndev, priv->hw_ctx_id);
	if (ret)
		XDNA_WARN(xdna, "destroy ctx id %d failed %d", priv->hw_ctx_id, ret);

	priv->hw_ctx_id = CTX_INVALID_ID;
	hwctx->doorbell_offset = CTX_INVALID_DOORBELL;
	aie4_unlink_cert_comp(hwctx);
}

static void aie4_hwctx_umq_fini(struct amdxdna_hwctx *hwctx)
{
	if (hwctx->priv && hwctx->priv->umq_bo)
		drm_gem_object_put(to_gobj(hwctx->priv->umq_bo));
}

static int aie4_hwctx_umq_init(struct amdxdna_hwctx *hwctx)
{
	const size_t indir_pkts_sz = CTX_MAX_CMDS * HSA_MAX_LEVEL1_INDIRECT_ENTRIES *
				     sizeof(struct host_indirect_packet_data);
	const size_t pkts_sz = CTX_MAX_CMDS * sizeof(struct host_queue_packet);
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_gem_obj *umq_bo;
	struct host_queue_header *qhdr;
	u64 data_dev_addr;
	void *umq_va;
	int ret;
	int i;

	/*
	 * The HSA queue lives in a user-allocated BO (umq_bo_hdl) in both user- and
	 * kernel-mode submission; the driver does not allocate it privately.  Under
	 * PASID/SVA the device reaches the queue through the submitting process's
	 * own page tables, so it must have a user virtual address - a kernel-private
	 * buffer would be unreachable by the device.  This is also safe: a forged
	 * read_index in this shared BO can only make the owning process complete its
	 * own command early and harm itself, never another context (NO_PASID/IOVA is
	 * a single-trust bring-up mode with no inter-process isolation).
	 */
	umq_bo = amdxdna_gem_get_obj(hwctx->client, hwctx->umq_bo_hdl, AMDXDNA_BO_SHARE);
	if (!umq_bo) {
		XDNA_ERR(xdna, "cannot find umq_bo handle %d", hwctx->umq_bo_hdl);
		return -ENOENT;
	}
	priv->umq_bo = umq_bo;

	/*
	 * Kernel-mode submission: the driver fills the host queue and rings the
	 * doorbell, so the user umq_bo must hold the header plus the direct and
	 * level-1 indirect packet arrays.  User-mode submission only needs the
	 * header (the user owns the queue content).
	 */
	if (umq_bo->mem.size < sizeof(*qhdr) ||
	    (priv->kernel_submit &&
	     umq_bo->mem.size < sizeof(*qhdr) + pkts_sz + indir_pkts_sz)) {
		XDNA_ERR(xdna, "umq_bo size %zu is too small",
			 (size_t)umq_bo->mem.size);
		ret = -EINVAL;
		goto err_fini;
	}

	umq_va = amdxdna_gem_vmap(umq_bo);
	if (!umq_va) {
		ret = -ENOMEM;
		goto err_fini;
	}
	qhdr = umq_va;

	priv->umq_read_index = &qhdr->read_index;
	priv->umq_write_index = &qhdr->write_index;

	/* User-mode submission: user owns the queue content and the doorbell. */
	if (!priv->kernel_submit)
		return 0;

	/*
	 * The queue content is driver-owned and never trusted from user space
	 * (only read_index is read back to detect completion).  Lay out the
	 * direct packets right after the header and the indirect packets after
	 * them, and publish the same base via data_address for CERT.
	 */
	data_dev_addr = amdxdna_gem_dev_addr(umq_bo) + sizeof(*qhdr);
	priv->umq_pkts = umq_va + sizeof(*qhdr);
	priv->umq_indirect_pkts = umq_va + sizeof(*qhdr) + pkts_sz;
	priv->umq_indirect_pkts_dev_addr = data_dev_addr + pkts_sz;

	memset(umq_va, 0, umq_bo->mem.size);
	priv->write_index = QUEUE_INDEX_START;
	qhdr->read_index = QUEUE_INDEX_START;
	qhdr->write_index = QUEUE_INDEX_START;
	qhdr->version.major = HOST_QUEUE_MAJOR_VERSION;
	qhdr->version.minor = HOST_QUEUE_MINOR_VERSION;
	qhdr->capacity = CTX_MAX_CMDS;
	qhdr->data_address = data_dev_addr;
	for (i = 0; i < CTX_MAX_CMDS; i++)
		priv->umq_pkts[i].pkt_header.common_header.opcode = OPCODE_EXEC_BUF;
	for (i = 0; i < CTX_MAX_CMDS * HSA_MAX_LEVEL1_INDIRECT_ENTRIES; i++) {
		priv->umq_indirect_pkts[i].header.opcode = OPCODE_EXEC_BUF;
		priv->umq_indirect_pkts[i].header.count = sizeof(struct exec_buf);
		priv->umq_indirect_pkts[i].header.distribute = 1;
	}

	return 0;

err_fini:
	aie4_hwctx_umq_fini(hwctx);
	return ret;
}

int aie4_hwctx_init(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	struct amdxdna_hwctx_priv *priv;
	int ret;

	if (!AIE_FEATURE_ON(&ndev->aie, AIE4_HSA_COMMAND))
		return -EOPNOTSUPP;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;
	hwctx->priv = priv;
	priv->hwctx = hwctx;
	/* Snapshot the submission mode so it is stable for this ctx's lifetime. */
	priv->kernel_submit = !!kernel_mode_submission;

	/*
	 * Kernel-mode submission: the driver fills the queue and rings the
	 * doorbell, so it needs the job machinery.  User-mode submission leaves
	 * the queue and doorbell to user space (no job machinery here).
	 */
	if (priv->kernel_submit) {
		mutex_init(&priv->io_lock);
		INIT_LIST_HEAD(&priv->pending_job_list);
		INIT_LIST_HEAD(&priv->running_job_list);
		init_waitqueue_head(&priv->job_list_wq);
		INIT_WORK(&priv->job_work, job_worker);
		priv->job_work_q = alloc_ordered_workqueue("%s", 0, hwctx->name);
		if (!priv->job_work_q) {
			XDNA_ERR(xdna, "Create job_work_q failed");
			ret = -ENOMEM;
			goto destroy_lock;
		}
	}

	ret = aie4_hwctx_umq_init(hwctx);
	if (ret)
		goto destroy_wq;

	ret = aie4_hwctx_create(hwctx);
	if (ret)
		goto umq_fini;

	XDNA_DBG(xdna, "hwctx %s init completed (%s submission)", hwctx->name,
		 priv->kernel_submit ? "kernel" : "user");
	return 0;

umq_fini:
	aie4_hwctx_umq_fini(hwctx);
destroy_wq:
	if (priv->kernel_submit)
		destroy_workqueue(priv->job_work_q);
destroy_lock:
	if (priv->kernel_submit)
		mutex_destroy(&priv->io_lock);
	kfree(priv);
	hwctx->priv = NULL;
	return ret;
}

void aie4_hwctx_fini(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	/* Disconnects the ctx and wakes the job worker (status DISCONNECTED). */
	aie4_hwctx_destroy(hwctx);
	if (priv->kernel_submit) {
		/* Drain/abort any in-flight jobs before tearing down the queue. */
		aie4_hwctx_cleanup_running_jobs(hwctx);
		destroy_workqueue(priv->job_work_q);
	}
	aie4_hwctx_umq_fini(hwctx);
	if (priv->kernel_submit)
		mutex_destroy(&priv->io_lock);
	kfree(priv);
}

static inline bool valid_queue_index(u64 read, u64 write, u32 capacity)
{
	return (write >= read) && ((write - read) <= capacity);
}

static u64 get_read_index(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	u64 ri, wi;

	/*
	 * Sample read_index (written by CERT) before write_index. CERT can
	 * never complete more than has been published, so a write_index sampled
	 * after read_index always satisfies wi >= ri; sampling write_index
	 * first races the submit path / CERT and yields a bogus ri > wi.
	 *
	 * In kernel-mode submission write_index is the driver's host-owned copy
	 * in coherent kernel memory (always >= the value mirrored into the UMQ,
	 * and the device never writes it).  In user-mode submission the user
	 * owns the queue, so fall back to the shared UMQ copy.
	 *
	 * Security: read_index lives in the umq_bo, which the owning process can
	 * map.  Under PASID/SVA the device reaches the queue through that process's
	 * own page tables, so a forged read_index only completes the process's own
	 * command early and corrupts or hangs itself - it cannot reach another
	 * context.
	 */
	ri = READ_ONCE(*priv->umq_read_index);
	/* Order the read_index sample before the write_index sample. */
	smp_rmb();
	wi = priv->kernel_submit ? READ_ONCE(priv->write_index)
				 : READ_ONCE(*priv->umq_write_index);

	/*
	 * CERT cannot update read index as uint64 atomically. Driver may read
	 * a half-updated read index when it has bits in the high 32 bits. If it
	 * looks invalid, re-sample once -- WITHOUT sleeping, since this can run as
	 * a wait_event() condition. If still invalid, report not-advanced; the
	 * waiter re-checks on the next completion wake or timeout.
	 */
	if (!valid_queue_index(ri, wi, CTX_MAX_CMDS)) {
		ri = READ_ONCE(*priv->umq_read_index);
		/* Order the read_index sample before the write_index sample. */
		smp_rmb();
		wi = priv->kernel_submit ? READ_ONCE(priv->write_index)
					 : READ_ONCE(*priv->umq_write_index);
		if (!valid_queue_index(ri, wi, CTX_MAX_CMDS)) {
			/*
			 * Still invalid (torn 64-bit read, or a transient
			 * accounting skew).  Return the last valid read_index
			 * instead of 0: read_index only advances, so the cached
			 * value is a safe lower bound -- it never reports a
			 * command complete that isn't, and never regresses the
			 * worker into falsely timing out a finished job.
			 */
			XDNA_DBG(xdna, "Invalid index, ri %llu, wi %llu", ri, wi);
			return READ_ONCE(priv->last_read_index);
		}
	}

	WRITE_ONCE(priv->last_read_index, ri);
	return ri;
}

static int check_cert_comp_linked(struct amdxdna_hwctx *hwctx, struct cert_comp *comp)
{
	struct amdxdna_dev_hdl *ndev = hwctx->client->xdna->dev_handle;

	guard(mutex)(&ndev->cert_comp_lock);

	/* any changed comp indicates that EAGAIN is needed */
	return comp == hwctx->priv->cert_comp;
}

static bool check_cmd_done(struct amdxdna_hwctx *hwctx, u64 seq)
{
	/*
	 * Runs as a wait_event() condition, so it must not sleep: use only
	 * lockless reads.  A disconnect (teardown/reset) breaks the wait via the
	 * status check; the caller re-checks cert_comp linkage afterwards (in
	 * TASK_RUNNING context) to decide whether to return -EAGAIN.
	 */
	if (READ_ONCE(hwctx->priv->status) != CTX_STATE_CONNECTED)
		return true;

	return get_read_index(hwctx) > seq;
}

int aie4_cmd_wait(struct amdxdna_hwctx *hwctx, u64 seq, u32 timeout)
{
	unsigned long wait_jifs = MAX_SCHEDULE_TIMEOUT;
	struct cert_comp *cert_comp = aie4_get_cert_comp(hwctx);
	long ret;

	if (!cert_comp)
		return -EAGAIN;

	if (timeout)
		wait_jifs = msecs_to_jiffies(timeout);

	ret = wait_event_interruptible_timeout(cert_comp->waitq,
					       check_cmd_done(hwctx, seq),
					       wait_jifs);

	if (!ret)
		ret = -ETIME;
	else if (ret > 0 && !check_cert_comp_linked(hwctx, cert_comp))
		ret = -EAGAIN;

	aie4_put_cert_comp(cert_comp);

	return ret <= 0 ? ret : 0;
}

/* ---- kernel-mode submission (driver fills the queue and rings doorbell) ---- */

static inline void ring_doorbell(struct amdxdna_hwctx *hwctx)
{
	writel(0, hwctx->priv->doorbell_addr);
}

/* Publish a command to CERT and return the assigned command sequence (slot). */
static u64 publish_cmd(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	u64 wi = priv->write_index;

	/* Paired with the lockless READ_ONCE() readers of write_index. */
	WRITE_ONCE(priv->write_index, wi + 1);
	/* Order the packet-slot writes before CERT sees the new write_index. */
	wmb();
	WRITE_ONCE(*priv->umq_write_index, wi + 1);
	return wi;
}

static bool job_check_done(struct amdxdna_hwctx *hwctx, u64 seq)
{
	if (READ_ONCE(hwctx->priv->status) != CTX_STATE_CONNECTED)
		return true;

	return get_read_index(hwctx) > seq;
}

static int wait_till_seq_completed(struct amdxdna_hwctx *hwctx, u64 seq)
{
	struct cert_comp *cert_comp = aie4_get_cert_comp(hwctx);
	u32 timeout_ms;
	long ret;

	if (!cert_comp)
		return -EAGAIN;

	/*
	 * Always bound the wait so an unresponsive CERT cannot park the job
	 * worker in an uninterruptible sleep forever (which would also block ctx
	 * teardown and module unload).  A timeout re-checks read_index, so a
	 * completion whose MSI-X was lost is still observed.
	 */
	timeout_ms = kernel_submit_timeout_ms ? kernel_submit_timeout_ms :
						KERNEL_SUBMIT_TIMEOUT_MS_DEFAULT;

	ret = wait_event_timeout(cert_comp->waitq, job_check_done(hwctx, seq),
				 msecs_to_jiffies(timeout_ms));
	aie4_put_cert_comp(cert_comp);

	/* Lockless read; status is written under io_lock by destroy/job_timeout. */
	if (READ_ONCE(hwctx->priv->status) != CTX_STATE_CONNECTED)
		return -EAGAIN;

	/* Condition stayed false until the deadline: CERT did not complete. */
	return ret ? 0 : -ETIME;
}

static int wait_till_hsa_not_full(struct amdxdna_hwctx *hwctx)
{
	u64 wi = READ_ONCE(hwctx->priv->write_index);

	if (wi < CTX_MAX_CMDS)
		return 0;

	return wait_till_seq_completed(hwctx, wi - CTX_MAX_CMDS);
}

static void fill_indirect_pkt(struct amdxdna_hwctx_priv *priv, u64 slot_idx,
			      u32 total_slots, struct amdxdna_cmd_start_dpu *dpu,
			      u16 entries)
{
	struct host_queue_packet *pkt = &priv->umq_pkts[slot_idx];
	struct host_indirect_packet_entry *hipe =
		(struct host_indirect_packet_entry *)(pkt->data);
	u16 i;

	for (i = 0; i < entries; i++, dpu++, hipe++) {
		struct host_indirect_packet_data *hipd;
		u64 indirect_pkt_dev_addr;
		u32 uci = dpu->uc_index;
		u32 idx;

		if (uci >= HSA_MAX_LEVEL1_INDIRECT_ENTRIES) {
			XDNA_ERR(priv->hwctx->client->xdna, "Invalid uc index %d", uci);
			continue;
		}
		idx = uci * total_slots + slot_idx;
		hipd = &priv->umq_indirect_pkts[idx];
		indirect_pkt_dev_addr = priv->umq_indirect_pkts_dev_addr +
			sizeof(struct host_indirect_packet_data) * idx;

		/* Point the indirect entry at the indirect packet. */
		hipe->host_addr_low = lower_32_bits(indirect_pkt_dev_addr);
		hipe_set_host_addr_high(&hipe->host_addr_high_uc_index,
					upper_32_bits(indirect_pkt_dev_addr));
		hipe_set_uc_index(&hipe->host_addr_high_uc_index, uci);

		/* Fill in the indirect packet. */
		hipd->payload.dpu_control_code_host_addr_low =
			lower_32_bits(dpu->instruction_buffer);
		hipd->payload.dpu_control_code_host_addr_high =
			upper_32_bits(dpu->instruction_buffer);
		hipd->payload.dtrace_buf_host_addr_low =
			lower_32_bits(dpu->dtrace_buffer);
		hipd->payload.dtrace_buf_host_addr_high =
			lower_16_bits(upper_32_bits(dpu->dtrace_buffer));
	}
	pkt->pkt_header.common_header.distribute = 1;
	pkt->pkt_header.common_header.indirect = 1;
	pkt->pkt_header.common_header.count = entries * sizeof(*hipe);
}

static void fill_direct_pkt(struct amdxdna_hwctx_priv *priv, u64 slot_idx,
			    struct amdxdna_cmd_start_dpu *dpu)
{
	struct host_queue_packet *pkt = &priv->umq_pkts[slot_idx];
	struct exec_buf *ebuf = (struct exec_buf *)(pkt->data);

	memset(pkt->data, 0, sizeof(pkt->data));
	ebuf->dpu_control_code_host_addr_low = lower_32_bits(dpu->instruction_buffer);
	ebuf->dpu_control_code_host_addr_high = upper_32_bits(dpu->instruction_buffer);
	ebuf->dtrace_buf_host_addr_low = lower_32_bits(dpu->dtrace_buffer);
	ebuf->dtrace_buf_host_addr_high = lower_16_bits(upper_32_bits(dpu->dtrace_buffer));
	pkt->pkt_header.common_header.distribute = 0;
	pkt->pkt_header.common_header.indirect = 0;
	pkt->pkt_header.common_header.count = sizeof(*ebuf);
}

/*
 * Build and submit one HSA command for @cmd_abo into the user host queue and
 * ring the doorbell.  Called with io_lock held.
 *
 * Security: cmd_abo is shared with user space; cache and validate its fields
 * before use and never trust the queue content (only read_index is read back).
 */
static int submit_one_cmd(struct amdxdna_hwctx *hwctx,
			  struct amdxdna_gem_obj *cmd_abo, bool last_of_chain,
			  u64 *seq)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_cmd_start_dpu *dpu;
	struct host_queue_packet *pkt;
	u32 payload_size;
	u64 slot_idx;
	u16 chained;
	int ret;
	u32 op;

	op = amdxdna_cmd_get_op(cmd_abo);
	if (op != ERT_START_DPU) {
		XDNA_ERR(xdna, "Invalid exec buf op, %d", op);
		return -EINVAL;
	}

	dpu = amdxdna_cmd_get_payload(cmd_abo, &payload_size);
	if (!dpu) {
		XDNA_ERR(xdna, "Invalid DPU payload");
		return -EINVAL;
	}
	/*
	 * cmd_abo is shared with user space; validate the cached chained count
	 * against the actual payload size before dereferencing chained+1 DPU
	 * entries, so a bogus count cannot drive an out-of-bounds read.
	 */
	chained = dpu->chained;
	if (chained >= HSA_MAX_LEVEL1_INDIRECT_ENTRIES) {
		XDNA_ERR(xdna, "Invalid DPU data");
		return -EINVAL;
	}
	if (payload_size < (u32)(chained + 1) * sizeof(*dpu)) {
		XDNA_ERR(xdna, "DPU payload %u too small for %u entries",
			 payload_size, chained + 1);
		return -EINVAL;
	}
	/*
	 * Validate every per-entry uc_index up front: fill_indirect_pkt() indexes
	 * priv->umq_indirect_pkts[] by uc_index, and a stale/unfilled slot must not
	 * be published to CERT.  cmd_abo is user-shared, so reject a bad index here
	 * rather than skip the entry while still advertising it in the packet count.
	 */
	if (chained) {
		u16 i;

		for (i = 0; i <= chained; i++) {
			if (dpu[i].uc_index >= HSA_MAX_LEVEL1_INDIRECT_ENTRIES) {
				XDNA_ERR(xdna, "Invalid uc index %u", dpu[i].uc_index);
				return -EINVAL;
			}
		}
	}

	/*
	 * The queue may be full; the wait sleeps until CERT drains it.  Drop
	 * io_lock across the sleep so the job worker (and other submitters) can
	 * make progress, then re-acquire and re-check the ctx is still live.
	 */
	mutex_unlock(&priv->io_lock);
	ret = wait_till_hsa_not_full(hwctx);
	mutex_lock(&priv->io_lock);
	if (ret)
		return ret;
	if (priv->status != CTX_STATE_CONNECTED)
		return -EIO;

	slot_idx = priv->write_index & (CTX_MAX_CMDS - 1);
	if (chained)
		fill_indirect_pkt(priv, slot_idx, CTX_MAX_CMDS, dpu, chained + 1);
	else
		fill_direct_pkt(priv, slot_idx, dpu);

	pkt = &priv->umq_pkts[slot_idx];
	pkt->pkt_header.common_header.opcode = OPCODE_EXEC_BUF;
	pkt->pkt_header.common_header.chain_flag =
		last_of_chain ? CHAIN_FLG_LAST_CMD : CHAIN_FLG_NOT_LAST_CMD;
	pkt->pkt_header.common_header.reserved = 0x0;
	pkt->pkt_header.completion_signal = amdxdna_gem_dev_addr(cmd_abo) +
					    offsetof(struct amdxdna_cmd, header);
	*seq = publish_cmd(hwctx);
	ring_doorbell(hwctx);
	XDNA_DBG(xdna, "Submitted one cmd, %s seq %lld", hwctx->name, *seq);
	return 0;
}

static struct amdxdna_sched_job *next_running_job(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_sched_job *job;

	mutex_lock(&priv->io_lock);
	job = list_first_entry_or_null(&priv->running_job_list,
				       struct amdxdna_sched_job, aie4_job_list);
	if (job)
		list_del(&job->aie4_job_list);
	mutex_unlock(&priv->io_lock);
	return job;
}

static void aie4_job_release(struct kref *ref)
{
	struct amdxdna_sched_job *job =
		container_of(ref, struct amdxdna_sched_job, refcnt);

	amdxdna_sched_job_cleanup(job);
	atomic64_inc(&job->hwctx->job_free_cnt);
	if (job->out_fence)
		dma_fence_put(job->out_fence);
	kfree(job);
}

static void job_done(struct amdxdna_sched_job *job)
{
	job->aie4_job_state = JOB_STATE_DONE;
	dma_fence_signal(job->fence);
	/*
	 * Release the address-space reference taken at submit.  On SVA/IOMMU
	 * platforms the device walks the submitter's page tables while the job
	 * runs, so its mm must stay alive until completion.
	 */
	mmput_async(job->mm);
	kref_put(&job->refcnt, aie4_job_release);
}

static void job_complete(struct amdxdna_sched_job *job)
{
	job_done(job);
}

/*
 * When CERT cannot complete a command (context teardown), the driver advances
 * read_index so any waiter observes the command as finished.  Only valid while
 * the context is disconnected -- never race CERT's own read_index updates.
 */
static void update_read_index(struct amdxdna_hwctx *hwctx, u64 idx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	drm_WARN_ON(&hwctx->client->xdna->ddev, priv->status == CTX_STATE_CONNECTED);

	/* Order cmd-bo state write before the waiter observes completion. */
	wmb();
	WRITE_ONCE(*priv->umq_read_index, idx);
}

static void job_abort(struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx *hwctx = job->hwctx;

	XDNA_ERR(hwctx->client->xdna, "aborting %s job %lld", hwctx->name, job->seq);
	amdxdna_cmd_set_state(job->cmd_bo, ERT_CMD_STATE_ABORT);
	update_read_index(hwctx, job->seq + 1);
	job_done(job);
}

/*
 * Write the firmware context health report into @cmd_abo's data region so user
 * space can read it after the command is marked timed out.  If no async report
 * was cached for this ctx, zero the per-generation fields.  The cached report is
 * consumed (cleared) on use.  Caller must hold io_lock, which serializes the
 * multi-word read against the async error worker overwriting it concurrently.
 */
static void __aie4_fill_health_data(struct amdxdna_gem_obj *cmd_abo,
				    struct amdxdna_hwctx *hwctx)
{
	const size_t min_size = offsetof(struct amdxdna_ctx_health_data, aie4.uc_info);
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_ctx_health_data *health;
	struct aie4_msg_app_health_report *report;
	u32 data_total;

	health = amdxdna_cmd_get_data(cmd_abo, &data_total);
	if (!health || data_total < min_size) {
		XDNA_WARN(xdna, "Health data buffer too small: %u min %zu",
			  data_total, min_size);
		return;
	}

	health->version = AMDXDNA_CTX_HEALTH_DATA_V1;
	health->npu_gen = AMDXDNA_NPU_GEN_AIE4;

	if (!priv->cached_ctx_error_valid) {
		health->aie4.ctx_state = 0;
		health->aie4.ctx_error_type = 0;
		health->aie4.num_uc = 0;
		return;
	}

	report = &priv->cached_ctx_error.app_health_report;
	health->aie4.ctx_state = aie4_health_get_ctx_status(report);
	health->aie4.ctx_error_type = priv->cached_ctx_error.error_type;
	health->aie4.num_uc = 0;
	if (data_total > min_size) {
		u32 max_uc = (data_total - min_size) / sizeof(struct uc_health_info);
		u32 num_uc = min_t(u32, aie4_health_get_num_uc(report),
				   AIE4_MPNPUFW_MAX_UC_COUNT);

		/*
		 * Bound by the fixed firmware source array (report->uc_info has
		 * AIE4_MPNPUFW_MAX_UC_COUNT entries) as well as the user buffer
		 * capacity, so a firmware-reported count cannot drive an
		 * out-of-bounds read of the source.
		 */
		num_uc = min_t(u32, num_uc, max_uc);
		if (num_uc)
			memcpy(health->aie4.uc_info, report->uc_info,
			       num_uc * sizeof(struct uc_health_info));
		health->aie4.num_uc = num_uc;
	}

	/* Consumed; further timeouts on this ctx report zeros until re-cached. */
	priv->cached_ctx_error_valid = false;
}

static void aie4_fill_health_data(struct amdxdna_gem_obj *cmd_abo,
				  struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	mutex_lock(&priv->io_lock);
	__aie4_fill_health_data(cmd_abo, hwctx);
	mutex_unlock(&priv->io_lock);
}

/*
 * For a timed-out command chain, identify the failing sub-command from the
 * cached health report's runlist index, record it in error_index, and fill that
 * sub-command's health data.  The runlist index and the report body are read
 * under a single io_lock hold, so they cannot come from two different reports if
 * the async error worker caches a newer one in between.
 */
static void aie4_fill_chain_health_data(struct amdxdna_hwctx *hwctx,
					struct amdxdna_gem_obj *cmd_abo)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_cmd_chain *payload;
	struct amdxdna_gem_obj *sub_abo;
	u32 payload_len, ccnt, i = 0;

	payload = amdxdna_cmd_get_payload(cmd_abo, &payload_len);
	if (!payload) {
		XDNA_ERR(xdna, "No chain payload for timed-out cmd");
		return;
	}
	ccnt = payload->command_count;
	if (!ccnt || payload_len < struct_size(payload, data, ccnt))
		return;

	/*
	 * The cached report is ctx-scoped, best-effort diagnostic data: the
	 * runlist index only disambiguates the failing sub-command within a
	 * chain, not which job produced the report.  With several jobs in flight
	 * in the same ctx the report may be attached to a sibling job.  That is
	 * acceptable because a fatal firmware error resets the whole ctx, so all
	 * in-flight jobs fail together; only the precise fault-site labelling may
	 * be off, and no other ctx/process is affected.
	 */
	mutex_lock(&priv->io_lock);
	if (priv->cached_ctx_error_valid)
		i = aie4_health_runlist_read_idx(&priv->cached_ctx_error.app_health_report);
	if (i >= ccnt)
		i = 0;
	payload->error_index = i;
	sub_abo = amdxdna_gem_get_obj(hwctx->client, (u32)payload->data[i], AMDXDNA_BO_SHARE);
	if (sub_abo)
		__aie4_fill_health_data(sub_abo, hwctx);
	mutex_unlock(&priv->io_lock);

	if (sub_abo)
		amdxdna_gem_put_obj(sub_abo);
	else
		XDNA_ERR(xdna, "Failed to find sub cmd BO %u", (u32)payload->data[i]);
}

/*
 * Reap a timed-out job during the disconnected drain.  job_worker() only routes
 * here on the firmware-fault path (cached async health report) once the context
 * is already DISCONNECTED - context teardown and the TDR reset both quiesce
 * firmware via aie4_hwctx_destroy() before draining - so the device no longer
 * touches the command BO, the umq buffers or read_index.  Attach the cached
 * health report to the command, mark it TIMEOUT, advance read_index so waiters
 * observe completion, and signal the fence.
 */
static void job_timeout(struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx *hwctx = job->hwctx;
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	XDNA_ERR(hwctx->client->xdna, "timing out %s job %lld", hwctx->name, job->seq);

	/*
	 * Attach the firmware health report (if one was cached for this ctx) to
	 * the timed-out command before publishing the TIMEOUT state.  For a chain
	 * the failing sub-command is identified and carries the report.
	 */
	if (amdxdna_cmd_get_op(job->cmd_bo) == ERT_CMD_CHAIN)
		aie4_fill_chain_health_data(hwctx, job->cmd_bo);
	else
		aie4_fill_health_data(job->cmd_bo, hwctx);

	/* Ensure health data is visible before user space observes TIMEOUT. */
	wmb();
	amdxdna_cmd_set_state(job->cmd_bo, ERT_CMD_STATE_TIMEOUT);

	/*
	 * Firmware is already quiesced and the context DISCONNECTED; advance
	 * read_index under io_lock (serialized against the submit path) so any
	 * waiter observes completion.
	 */
	mutex_lock(&priv->io_lock);
	update_read_index(hwctx, job->seq + 1);
	mutex_unlock(&priv->io_lock);

	/* Wake parked submitters so they observe the disconnect and bail. */
	wake_up_all(&priv->job_list_wq);

	job_done(job);
}

static void job_worker(struct work_struct *work)
{
	struct amdxdna_hwctx_priv *priv =
		container_of(work, struct amdxdna_hwctx_priv, job_work);
	struct amdxdna_hwctx *hwctx = priv->hwctx;
	struct amdxdna_sched_job *job;

	while ((job = next_running_job(hwctx))) {
		int ret = wait_till_seq_completed(hwctx, job->seq);

		trace_amdxdna_debug_point(hwctx->name, job->seq, "job complete");

		if (get_read_index(hwctx) > job->seq) {
			/*
			 * The published commands drained.  A chain that was only
			 * partially published (a later sub-command failed to
			 * enqueue) still drains its prefix here, but must be
			 * reported as failed rather than mistaken for success.
			 */
			if (job->aie4_job_state != JOB_STATE_SUBMITTED)
				amdxdna_cmd_set_state(job->cmd_bo, ERT_CMD_STATE_ABORT);
			job_complete(job);
		} else if (ret != -ETIME) {
			/*
			 * Disconnected (firmware quiesced by destroy on the teardown or
			 * TDR-reset path) - safe to reap.  If an async fault report was
			 * cached, attach it and mark the command TIMEOUT; otherwise
			 * abort.  cached_ctx_error_valid is a lockless hint;
			 * job_timeout() -> aie4_fill_health_data() re-checks under io_lock.
			 */
			if (READ_ONCE(priv->cached_ctx_error_valid))
				job_timeout(job);
			else
				job_abort(job);
		} else {
			/*
			 * Software-timeout backstop: CERT is silent but the context is
			 * still connected and may merely be slow.  Do not reclaim a
			 * possibly-live job - that would drop the mm pin and BO refs
			 * while the device might still DMA into them.  Requeue and
			 * re-wait; a later pass completes it if CERT was just slow,
			 * otherwise ctx teardown or the TDR reset reaps it.
			 */
			mutex_lock(&priv->io_lock);
			list_add(&job->aie4_job_list, &priv->running_job_list);
			mutex_unlock(&priv->io_lock);
			continue;
		}
	}
}

static void aie4_hwctx_cleanup_running_jobs(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	/* Must be disconnected so CERT can no longer complete jobs. */
	drm_WARN_ON(&hwctx->client->xdna->ddev, priv->status == CTX_STATE_CONNECTED);
	queue_work(priv->job_work_q, &priv->job_work);
	flush_work(&priv->job_work);
}

/*
 * Recover a faulted context (TDR): destroy then recreate the firmware context.
 * Destroy disconnects the ctx and wakes any cmd_wait waiters; draining the
 * running jobs reaps the faulting command (timeout, with the cached health
 * report) and aborts the rest, which empties the HSA queue; recreate reconnects
 * so user space can keep submitting.  Caller holds dev_lock.
 */
void aie4_hwctx_reset(struct amdxdna_hwctx *hwctx)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	int ret;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	if (!priv->kernel_submit)
		return;

	aie4_hwctx_destroy(hwctx);
	aie4_hwctx_cleanup_running_jobs(hwctx);

	/*
	 * Drop any cached fault report not consumed by the drain so it cannot be
	 * misattributed to a command in the next context generation.
	 */
	mutex_lock(&priv->io_lock);
	priv->cached_ctx_error_valid = false;
	mutex_unlock(&priv->io_lock);

	ret = aie4_hwctx_create(hwctx);
	if (ret) {
		XDNA_ERR(xdna, "Reset %s failed, ret %d", hwctx->name, ret);
		/*
		 * Recreate failed: the ctx stays DISCONNECTED.  Wake submitters
		 * parked in aie4_cmd_submit() so they observe the disconnect and
		 * bail instead of blocking until a fatal signal.
		 */
		wake_up_all(&priv->job_list_wq);
		return;
	}

	/* Reconnected: release submitters parked while disconnected. */
	wake_up_all(&priv->job_list_wq);
}

/*
 * Submit the command(s) carried by @job into the host queue.  Called with
 * io_lock held.  A single ERT_START_DPU maps to one queue entry; an
 * ERT_CMD_CHAIN expands to one entry per sub-command, only the last of which
 * carries CHAIN_FLG_LAST_CMD so CERT runs the whole chain back to back.
 *
 * job->seq tracks the last published sequence; the worker waits on it to reap
 * the entire chain.  job->aie4_job_state advances past PENDING as soon as any
 * sub-command is published, so the caller knows whether in-flight commands must
 * still be reaped even when a later sub-command fails to enqueue.
 *
 * Security: the chain payload and its BO handles come from user space; cache
 * command_count and validate it against the payload size before walking the
 * handle array so a bogus count cannot drive an out-of-bounds read.
 */
static int submit_job_cmds(struct amdxdna_hwctx *hwctx, struct amdxdna_sched_job *job)
{
	struct amdxdna_gem_obj *cmd_abo = job->cmd_bo;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_cmd_chain *payload;
	u32 op = amdxdna_cmd_get_op(cmd_abo);
	u32 payload_len, ccnt;
	int ret;
	u32 i;

	/* Single cmd. */
	if (op == ERT_START_DPU) {
		ret = submit_one_cmd(hwctx, cmd_abo, true, &job->seq);
		if (!ret)
			job->aie4_job_state = JOB_STATE_SUBMITTED;
		return ret;
	}

	/* Cmd chain. */
	payload = amdxdna_cmd_get_payload(cmd_abo, &payload_len);
	if (!payload) {
		XDNA_ERR(xdna, "Invalid cmd payload for chained cmd");
		return -EINVAL;
	}
	ccnt = payload->command_count;
	if (!ccnt || ccnt > MAX_CHAINED_SUB_CMD ||
	    payload_len < struct_size(payload, data, ccnt)) {
		XDNA_ERR(xdna, "Invalid command count %u", ccnt);
		return -EINVAL;
	}

	for (i = 0; i < ccnt; i++) {
		u32 boh = (u32)(payload->data[i]);
		struct amdxdna_gem_obj *abo;

		abo = amdxdna_gem_get_obj(hwctx->client, boh, AMDXDNA_BO_SHARE);
		if (!abo) {
			XDNA_ERR(xdna, "Failed to find cmd BO %u", boh);
			ret = -ENOENT;
			break;
		}
		ret = submit_one_cmd(hwctx, abo, i + 1 == ccnt, &job->seq);
		amdxdna_gem_put_obj(abo);
		if (ret)
			break;
		job->aie4_job_state = JOB_STATE_SUBMITTING;
	}
	if (i == ccnt)
		job->aie4_job_state = JOB_STATE_SUBMITTED;

	return ret;
}

/*
 * Whole-job submission is serialized across submitters that share a ctx via the
 * pending list: a job is appended on entry and only the head of the list is
 * allowed to publish its command(s).  Because the head stays on the list for the
 * entire duration of submit_job_cmds() -- which may drop io_lock to wait for
 * free queue slots -- no other submitter can interleave its commands into the
 * middle of the head job's command chain.  io_lock protects the lists; the
 * job_list_wq waitqueue notifies parked submitters when the head changes.
 */
/* Publish the current pending-list head for the lockless submit wait condition.
 * Caller holds io_lock.
 */
static void update_pending_head(struct amdxdna_hwctx_priv *priv)
{
	WRITE_ONCE(priv->pending_head,
		   list_first_entry_or_null(&priv->pending_job_list,
					    struct amdxdna_sched_job, aie4_job_list));
}

static void enqueue_pending_job(struct amdxdna_hwctx *hwctx,
				struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	mutex_lock(&priv->io_lock);
	list_add_tail(&job->aie4_job_list, &priv->pending_job_list);
	job->aie4_job_state = JOB_STATE_PENDING;
	update_pending_head(priv);
	mutex_unlock(&priv->io_lock);
}

static void cancel_pending_job(struct amdxdna_hwctx *hwctx,
			       struct amdxdna_sched_job *job)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;

	mutex_lock(&priv->io_lock);
	list_del(&job->aie4_job_list);
	job->aie4_job_state = JOB_STATE_INIT;
	update_pending_head(priv);
	mutex_unlock(&priv->io_lock);
	/* Let the next pending submitter re-check whether it is now first. */
	wake_up_all(&priv->job_list_wq);
}

int aie4_cmd_submit(struct amdxdna_hwctx *hwctx, struct amdxdna_sched_job *job, u64 *seq)
{
	struct amdxdna_hwctx_priv *priv = hwctx->priv;
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	u32 op;
	int ret;

	XDNA_DBG(xdna, "ctx %s job 0x%llx received", hwctx->name, (u64)job);

	if (!priv->kernel_submit) {
		/* User-mode submission rings its own doorbell; no kernel submit. */
		XDNA_ERR(xdna, "cmd submit ioctl not supported in user-mode submission");
		return -EOPNOTSUPP;
	}

	if (!job->cmd_bo) {
		XDNA_ERR(xdna, "No command BO in job");
		return -EINVAL;
	}

	op = amdxdna_cmd_get_op(job->cmd_bo);
	if (op != ERT_START_DPU && op != ERT_CMD_CHAIN) {
		XDNA_ERR(xdna, "Invalid cmd opcode %d", op);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&job->aie4_job_list);

	/*
	 * Hold a reference on the submitter's address space until the job
	 * completes (job_done): on SVA/IOMMU platforms the device walks the
	 * submitter's page tables while the command runs.  Balanced with the
	 * mmput_async() in job_done() and the mmput() on the failure paths below.
	 */
	if (!mmget_not_zero(job->mm)) {
		XDNA_ERR(xdna, "Failed to get mm reference");
		return -ESRCH;
	}

	/*
	 * Wait until this job is at the head of the pending list before touching
	 * the queue (see enqueue_pending_job).  The wait is killable so a parked
	 * submitter cannot keep ctx teardown (synchronize_srcu) blocked forever;
	 * it also breaks out if the ctx is being disconnected.
	 */
	enqueue_pending_job(hwctx, job);
	ret = wait_event_killable(priv->job_list_wq,
				  READ_ONCE(priv->status) != CTX_STATE_CONNECTED ||
				  READ_ONCE(priv->pending_head) == job);
	if (ret) {
		cancel_pending_job(hwctx, job);
		mmput(job->mm);
		return ret;
	}

	mutex_lock(&priv->io_lock);
	if (priv->status != CTX_STATE_CONNECTED) {
		mutex_unlock(&priv->io_lock);
		cancel_pending_job(hwctx, job);
		mmput(job->mm);
		return -EIO;
	}

	ret = submit_job_cmds(hwctx, job);
	if (job->aie4_job_state == JOB_STATE_PENDING) {
		/* No command was published; nothing for the worker to reap. */
		list_del(&job->aie4_job_list);
		update_pending_head(priv);
		mutex_unlock(&priv->io_lock);
		/* Release the next pending submitter. */
		wake_up_all(&priv->job_list_wq);
		mmput(job->mm);
		return ret;
	}

	/*
	 * At least one command is in flight (a chain may have published some
	 * before failing); move the job to the running list so the worker waits
	 * on the last published seq and reaps it.  A partially-submitted chain
	 * stays at JOB_STATE_SUBMITTING so the worker reports it as failed after
	 * draining its published prefix.
	 */
	list_move_tail(&job->aie4_job_list, &priv->running_job_list);
	update_pending_head(priv);
	*seq = job->seq;
	mutex_unlock(&priv->io_lock);

	/* Release the next pending submitter and kick the reaper. */
	wake_up_all(&priv->job_list_wq);
	atomic64_inc(&hwctx->job_submit_cnt);
	queue_work(priv->job_work_q, &priv->job_work);
	return 0;
}

int aie4_hwctx_valid_doorbell(struct amdxdna_client *client, u32 vm_pgoff)
{
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_hwctx *hwctx;
	unsigned long hwctx_id;
	int idx;

	guard(mutex)(&xdna->dev_lock);

	idx = srcu_read_lock(&client->hwctx_srcu);
	amdxdna_for_each_hwctx(client, hwctx_id, hwctx) {
		/*
		 * Kernel-mode contexts own the doorbell internally and carry the
		 * CTX_INVALID_DOORBELL sentinel; never match it, or user space
		 * could mmap (sentinel >> PAGE_SHIFT) and reach the doorbell BAR.
		 */
		if (hwctx->doorbell_offset == CTX_INVALID_DOORBELL)
			continue;
		if (vm_pgoff == (hwctx->doorbell_offset >> PAGE_SHIFT)) {
			srcu_read_unlock(&client->hwctx_srcu, idx);
			return 1;
		}
	}
	srcu_read_unlock(&client->hwctx_srcu, idx);

	return 0;
}

static int aie4_hwctx_cfg_debug_bo(struct amdxdna_hwctx *hwctx, u32 meta_bo_hdl,
				   bool attach)
{
	struct aie4_msg_context_config_cert_logging cl = { };
	struct amdxdna_client *client = hwctx->client;
	struct amdxdna_dev *xdna = client->xdna;
	struct amdxdna_dev_hdl *ndev = xdna->dev_handle;
	struct amdxdna_fw_buffer_metadata *meta;
	struct amdxdna_gem_obj *meta_bo;
	struct amdxdna_gem_obj *log_bo;
	u32 prev_size = 0;
	u32 property;
	u64 base_addr;
	u32 index;
	int ret;
	int i;

	meta_bo = amdxdna_gem_get_obj(client, meta_bo_hdl, AMDXDNA_BO_SHARE);
	if (!meta_bo) {
		XDNA_ERR(xdna, "Get meta bo %u failed", meta_bo_hdl);
		return -EINVAL;
	}

	if (meta_bo->mem.size < sizeof(*meta)) {
		XDNA_ERR(xdna, "meta bo size %lu is too small", meta_bo->mem.size);
		ret = -EINVAL;
		goto put_meta_bo;
	}

	meta = amdxdna_gem_vmap(meta_bo);
	if (!meta) {
		ret = -ENOMEM;
		goto put_meta_bo;
	}

	switch (meta->buf_type) {
	case AMDXDNA_FW_BUF_LOG:
		property = AIE4_CONFIGURE_HW_CONTEXT_PROPERTY_CERT_LOG_BUFFER;
		break;
	case AMDXDNA_FW_BUF_DEBUG:
		property = AIE4_CONFIGURE_HW_CONTEXT_PROPERTY_CERT_DEBUG_BUFFER;
		break;
	case AMDXDNA_FW_BUF_TRACE:
		property = AIE4_CONFIGURE_HW_CONTEXT_PROPERTY_CERT_TRACE_BUFFER;
		break;
	case AMDXDNA_FW_BUF_DBG_Q:
		property = AIE4_CONFIGURE_HW_CONTEXT_PROPERTY_CERT_DEBUG_QUEUE;
		break;
	default:
		XDNA_ERR(xdna, "Unsupported buf_type %u", meta->buf_type);
		ret = -EOPNOTSUPP;
		goto put_meta_bo;
	}

	if (meta->num_ucs > AIE4_MAX_NUM_CERTS) {
		XDNA_ERR(xdna, "num_ucs %u exceeds %d",
			 meta->num_ucs, AIE4_MAX_NUM_CERTS);
		ret = -EINVAL;
		goto put_meta_bo;
	}

	if (meta_bo->mem.size < struct_size(meta, uc_info, meta->num_ucs)) {
		XDNA_ERR(xdna, "meta bo size %lu too small for %u ucs",
			 meta_bo->mem.size, meta->num_ucs);
		ret = -EINVAL;
		goto put_meta_bo;
	}

	log_bo = amdxdna_gem_get_obj(client, meta->bo_handle, AMDXDNA_BO_SHARE);
	if (!log_bo) {
		XDNA_ERR(xdna, "Get payload bo %u failed", meta->bo_handle);
		ret = -EINVAL;
		goto put_meta_bo;
	}

	base_addr = amdxdna_gem_dev_addr(log_bo);

	for (i = 0; i < meta->num_ucs; i++) {
		u32 slice_size = meta->uc_info[i].size;
		u32 next_size;

		index = meta->uc_info[i].index;
		if (index >= AIE4_MAX_NUM_CERTS) {
			XDNA_ERR(xdna, "Invalid uc index %u", index);
			ret = -EINVAL;
			goto put_log_bo;
		}

		if (!attach) {
			cl.info[index].paddr = 0;
			cl.info[index].size = 0;
			continue;
		}

		if (!slice_size)
			continue;

		if (cl.info[index].size) {
			XDNA_ERR(xdna, "Duplicate uc index %u", index);
			ret = -EINVAL;
			goto put_log_bo;
		}

		if (check_add_overflow(prev_size, slice_size, &next_size) ||
		    next_size > log_bo->mem.size) {
			XDNA_ERR(xdna,
				 "uc[%u] slice 0x%x at 0x%x overflows payload bo size %lu",
				 index, slice_size, prev_size, log_bo->mem.size);
			ret = -EINVAL;
			goto put_log_bo;
		}

		cl.info[index].paddr = base_addr + prev_size;
		cl.info[index].size = slice_size;
		prev_size = next_size;
	}

	cl.num = FIELD_PREP(AIE4_MSG_CERT_LOG_NUM, attach ? meta->num_ucs : 0);

	ret = aie4_configure_hw_context_cert_log(ndev, hwctx->priv->hw_ctx_id,
						 property, &cl);
	XDNA_DBG(xdna, "%s CERT bo %u on %s, property %u, ret %d",
		 attach ? "attach" : "detach", meta_bo_hdl,
		 hwctx->name, property, ret);

put_log_bo:
	amdxdna_gem_put_obj(log_bo);
put_meta_bo:
	amdxdna_gem_put_obj(meta_bo);
	return ret;
}

int aie4_hwctx_config(struct amdxdna_hwctx *hwctx, u32 type, u64 value,
		      void *buf, u32 size)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	switch (type) {
	case DRM_AMDXDNA_HWCTX_ASSIGN_DBG_BUF:
		return aie4_hwctx_cfg_debug_bo(hwctx, (u32)value, true);
	case DRM_AMDXDNA_HWCTX_REMOVE_DBG_BUF:
		return aie4_hwctx_cfg_debug_bo(hwctx, (u32)value, false);
	default:
		XDNA_DBG(xdna, "Not supported type %d", type);
		return -EOPNOTSUPP;
	}
}
