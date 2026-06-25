// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <drm/drm_cache.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>

#include "aie.h"
#include "aie4_msg_priv.h"
#include "aie4_pci.h"
#include "amdxdna_ctx.h"
#include "amdxdna_error.h"
#include "amdxdna_mailbox.h"
#include "amdxdna_pci_drv.h"

#define ASYNC_BUF_SIZE	SZ_8K

struct async_event {
	struct amdxdna_dev_hdl			*ndev;
	struct aie4_msg_async_event_config_resp	resp;
	struct workqueue_struct			*wq;
	struct work_struct			work;
	u8					*buf;
	dma_addr_t				addr;
	u32					size;
};

struct async_events {
	struct workqueue_struct		*wq;
	struct amdxdna_msg_buf_hdl	*hdl;
	u32				event_cnt;
	struct async_event		event[] __counted_by(event_cnt);
};

/*
 * Below enum, struct and lookup tables are ported from the XAIE util header.
 * The data is defined by the AIE device and is used to decode error messages
 * reported by the device.
 */

enum aie_module_type {
	AIE_MEM_MOD = 0,
	AIE_CORE_MOD,
	AIE_PL_MOD,
};

enum aie_error_category {
	AIE_ERROR_SATURATION = 0,
	AIE_ERROR_FP,
	AIE_ERROR_STREAM,
	AIE_ERROR_ACCESS,
	AIE_ERROR_BUS,
	AIE_ERROR_INSTRUCTION,
	AIE_ERROR_ECC,
	AIE_ERROR_LOCK,
	AIE_ERROR_DMA,
	AIE_ERROR_MEM_PARITY,
	/* Unknown is not from XAIE, added for better category */
	AIE_ERROR_UNKNOWN,
};

/* Don't pack, unless XAIE side changed */
struct aie_error {
	u8			row;
	u8			col;
	enum aie_module_type	mod_type;
	u8			event_id;
};

struct aie_err_info {
	u32			err_cnt;
	u32			ret_code;
	u32			rsvd;
	struct aie_error	payload[] __counted_by(err_cnt);
};

struct aie_event_category {
	u8			event_id;
	enum aie_error_category category;
};

struct aie_cat_amdxdna_err_num {
	enum aie_error_category category;
	enum amdxdna_error_num drv_err_num;
};

struct aie_mod_amdxdna_err_mod {
	enum aie_module_type mod_type;
	enum amdxdna_error_module drv_err_mod;
};

#define EVENT_CATEGORY(id, cat) { id, cat }
static const struct aie_event_category aie_ml_mem_event_cat[] = {
	EVENT_CATEGORY(88U,  AIE_ERROR_ECC),
	EVENT_CATEGORY(90U,  AIE_ERROR_ECC),
	EVENT_CATEGORY(91U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(92U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(93U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(94U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(95U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(96U,  AIE_ERROR_MEM_PARITY),
	EVENT_CATEGORY(97U,  AIE_ERROR_DMA),
	EVENT_CATEGORY(98U,  AIE_ERROR_DMA),
	EVENT_CATEGORY(99U,  AIE_ERROR_DMA),
	EVENT_CATEGORY(100U, AIE_ERROR_DMA),
	EVENT_CATEGORY(101U, AIE_ERROR_LOCK),
};

static const struct aie_event_category aie_ml_core_event_cat[] = {
	EVENT_CATEGORY(55U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(56U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(57U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(58U, AIE_ERROR_BUS),
	EVENT_CATEGORY(59U, AIE_ERROR_INSTRUCTION),
	EVENT_CATEGORY(60U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(62U, AIE_ERROR_ECC),
	EVENT_CATEGORY(64U, AIE_ERROR_ECC),
	EVENT_CATEGORY(65U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(66U, AIE_ERROR_ACCESS),
	EVENT_CATEGORY(67U, AIE_ERROR_LOCK),
	EVENT_CATEGORY(70U, AIE_ERROR_INSTRUCTION),
	EVENT_CATEGORY(71U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(72U, AIE_ERROR_BUS),
};

static const struct aie_event_category aie_ml_mem_tile_event_cat[] = {
	EVENT_CATEGORY(130U, AIE_ERROR_ECC),
	EVENT_CATEGORY(132U, AIE_ERROR_ECC),
	EVENT_CATEGORY(133U, AIE_ERROR_DMA),
	EVENT_CATEGORY(134U, AIE_ERROR_DMA),
	EVENT_CATEGORY(135U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(136U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(137U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(138U, AIE_ERROR_BUS),
	EVENT_CATEGORY(139U, AIE_ERROR_LOCK),
};

static const struct aie_event_category aie_ml_shim_tile_event_cat[] = {
	EVENT_CATEGORY(64U, AIE_ERROR_BUS),
	EVENT_CATEGORY(65U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(66U, AIE_ERROR_STREAM),
	EVENT_CATEGORY(67U, AIE_ERROR_BUS),
	EVENT_CATEGORY(68U, AIE_ERROR_BUS),
	EVENT_CATEGORY(69U, AIE_ERROR_BUS),
	EVENT_CATEGORY(70U, AIE_ERROR_BUS),
	EVENT_CATEGORY(71U, AIE_ERROR_BUS),
	EVENT_CATEGORY(72U, AIE_ERROR_DMA),
	EVENT_CATEGORY(73U, AIE_ERROR_DMA),
	EVENT_CATEGORY(74U, AIE_ERROR_LOCK),
};

static const struct aie_cat_amdxdna_err_num aie_cat_err_num_map[] = {
	{ AIE_ERROR_SATURATION, AMDXDNA_ERROR_NUM_AIE_SATURATION },
	{ AIE_ERROR_FP, AMDXDNA_ERROR_NUM_AIE_FP },
	{ AIE_ERROR_STREAM, AMDXDNA_ERROR_NUM_AIE_STREAM },
	{ AIE_ERROR_ACCESS, AMDXDNA_ERROR_NUM_AIE_ACCESS },
	{ AIE_ERROR_BUS, AMDXDNA_ERROR_NUM_AIE_BUS },
	{ AIE_ERROR_INSTRUCTION, AMDXDNA_ERROR_NUM_AIE_INSTRUCTION },
	{ AIE_ERROR_ECC, AMDXDNA_ERROR_NUM_AIE_ECC },
	{ AIE_ERROR_LOCK, AMDXDNA_ERROR_NUM_AIE_LOCK },
	{ AIE_ERROR_DMA, AMDXDNA_ERROR_NUM_AIE_DMA },
	{ AIE_ERROR_MEM_PARITY, AMDXDNA_ERROR_NUM_AIE_MEM_PARITY },
};

static const struct aie_mod_amdxdna_err_mod aie_mod_amdxdna_err_mod_map[] = {
	{ AIE_MEM_MOD, AMDXDNA_ERROR_MODULE_AIE_MEMORY },
	{ AIE_CORE_MOD, AMDXDNA_ERROR_MODULE_AIE_CORE },
	{ AIE_PL_MOD, AMDXDNA_ERROR_MODULE_AIE_PL },
};

static enum amdxdna_error_module aie_get_amdxdna_error_mod(enum aie_module_type mod_type)
{
	for (int i = 0; i < ARRAY_SIZE(aie_mod_amdxdna_err_mod_map); i++) {
		if (aie_mod_amdxdna_err_mod_map[i].mod_type == mod_type)
			return aie_mod_amdxdna_err_mod_map[i].drv_err_mod;
	}
	return AMDXDNA_ERROR_MODULE_UNKNOWN;
}

static enum amdxdna_error_num aie_err_cat_get_amdxdna_err_num(enum aie_error_category cat)
{
	for (int i = 0; i < ARRAY_SIZE(aie_cat_err_num_map); i++) {
		if (aie_cat_err_num_map[i].category == cat)
			return aie_cat_err_num_map[i].drv_err_num;
	}
	return AMDXDNA_ERROR_NUM_UNKNOWN;
}

static enum aie_error_category
aie_get_error_category(u8 row, u8 event_id, enum aie_module_type mod_type)
{
	const struct aie_event_category *lut;
	int num_entry;
	int i;

	switch (mod_type) {
	case AIE_PL_MOD:
		lut = aie_ml_shim_tile_event_cat;
		num_entry = ARRAY_SIZE(aie_ml_shim_tile_event_cat);
		break;
	case AIE_CORE_MOD:
		lut = aie_ml_core_event_cat;
		num_entry = ARRAY_SIZE(aie_ml_core_event_cat);
		break;
	case AIE_MEM_MOD:
		if (row == 1) {
			lut = aie_ml_mem_tile_event_cat;
			num_entry = ARRAY_SIZE(aie_ml_mem_tile_event_cat);
		} else {
			lut = aie_ml_mem_event_cat;
			num_entry = ARRAY_SIZE(aie_ml_mem_event_cat);
		}
		break;
	default:
		return AIE_ERROR_UNKNOWN;
	}

	for (i = 0; i < num_entry; i++) {
		if (event_id != lut[i].event_id)
			continue;

		return lut[i].category;
	}

	return AIE_ERROR_UNKNOWN;
}

/* Cache the last AIE-tile async error.  Caller holds dev_lock. */
static void aie4_update_last_async_error(struct amdxdna_dev_hdl *ndev, void *err_info, u32 num_err)
{
	enum amdxdna_error_module amdxdna_err_mod;
	struct aie_error *errs = err_info;
	enum amdxdna_error_num err_num;
	struct aie_error *err;

	/* Cache the last async error only. */
	err = &errs[num_err - 1];
	err_num = aie_err_cat_get_amdxdna_err_num(aie_get_error_category(err->row, err->event_id,
									 err->mod_type));
	amdxdna_err_mod = aie_get_amdxdna_error_mod(err->mod_type);

	ndev->last_async_err.err_code = AMDXDNA_ERROR_ENCODE(err_num, amdxdna_err_mod);
	ndev->last_async_err.ts_us = ktime_to_us(ktime_get_real());
	ndev->last_async_err.ex_err_code = AMDXDNA_EXTRA_ERR_ENCODE(err->row, err->col);
}

static u32 aie4_error_backtrack(struct amdxdna_dev_hdl *ndev, void *err_info, u32 num_err)
{
	struct aie_error *errs = err_info;
	u32 err_col = 0; /* assume that AIE has less than 32 columns */
	int i;

	/* Get err column bitmap. */
	for (i = 0; i < num_err; i++) {
		struct aie_error *err = &errs[i];
		enum aie_error_category cat;

		cat = aie_get_error_category(err->row, err->event_id, err->mod_type);
		XDNA_ERR(ndev->aie.xdna, "Row: %d, Col: %d, module %d, event ID %d, category %d",
			 err->row, err->col, err->mod_type, err->event_id, cat);

		if (err->col >= 32) {
			/* If you see this, contact NPU firmware team */
			XDNA_WARN(ndev->aie.xdna, "Device has more than 32 columns?");
			break;
		}

		err_col |= (1U << err->col);
	}

	return err_col;
}

/* Find the hwctx for a firmware hw_ctx_id.  Caller holds dev_lock; on success
 * the matching client's hwctx_srcu is held and must be released via *srcu_idx.
 */
static struct amdxdna_hwctx *
hw_ctx_id2hwctx(struct amdxdna_dev_hdl *ndev, u32 hw_ctx_id, int *srcu_idx)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	struct amdxdna_client *client;
	struct amdxdna_hwctx *hwctx;
	unsigned long hwctx_id;
	int idx;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));
	amdxdna_for_each_client(xdna, client) {
		idx = srcu_read_lock(&client->hwctx_srcu);
		amdxdna_for_each_hwctx(client, hwctx_id, hwctx) {
			if (hwctx->priv && hwctx->priv->hw_ctx_id == hw_ctx_id) {
				/* Released by the caller. */
				*srcu_idx = idx;
				return hwctx;
			}
		}
		srcu_read_unlock(&client->hwctx_srcu, idx);
	}
	XDNA_WARN(xdna, "Could not find context for hw_ctx_id=%u", hw_ctx_id);
	return NULL;
}

/*
 * Handle a CERT/MPNPU context-error async event: log the health report, update
 * the last-async-error cache, and stash the report on the owning hwctx so the
 * timeout path can attach it to the failing command.  Recovery (ctx reset) is
 * handled separately by the TDR path.
 */
static void aie4_cache_ctx_error(struct amdxdna_dev_hdl *ndev,
				 struct aie4_async_ctx_error *ctx_err)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	struct aie4_msg_app_health_report *health;
	enum amdxdna_error_num err_num;
	struct amdxdna_hwctx *hwctx;
	struct uc_health_info *uc;
	int idx;
	u32 i;

	switch (ctx_err->error_type) {
	case AIE4_ASYNC_EVENT_CTX_ERR_HWSCH_FAILURE:
	case AIE4_ASYNC_EVENT_CTX_ERR_STOP_FAILURE:
		err_num = AMDXDNA_ERROR_NUM_KDS_CU;
		break;
	case AIE4_ASYNC_EVENT_CTX_ERR_AIE_FAILURE:
	case AIE4_ASYNC_EVENT_CTX_ERR_PREEMPTION_TIMEOUT:
	case AIE4_ASYNC_EVENT_CTX_ERR_NEW_PROCESS_FAILURE:
	case AIE4_ASYNC_EVENT_CTX_ERR_UC_CRITICAL_ERROR:
	case AIE4_ASYNC_EVENT_CTX_ERR_UC_COMPLETION_TIMEOUT:
		err_num = AMDXDNA_ERROR_NUM_KDS_EXEC;
		break;
	default:
		err_num = AMDXDNA_ERROR_NUM_UNKNOWN;
		break;
	}

	health = &ctx_err->app_health_report;
	XDNA_ERR(xdna, "Health: ver %u.%u ctx_status=%u num_uc=%u runlist_read_idx=%u",
		 health->major_version, health->minor_version,
		 aie4_health_get_ctx_status(health), aie4_health_get_num_uc(health),
		 aie4_health_runlist_read_idx(health));

	for (i = 0; i < min_t(u32, aie4_health_get_num_uc(health), AIE4_MPNPUFW_MAX_UC_COUNT);
	     i++) {
		uc = &health->uc_info[i];
		XDNA_ERR(xdna, "  UC[%u]: idx=%u fw_state=%u page=%u offset=0x%x",
			 i, uc->uc_idx, uc->fw_state, uc->page_idx, uc->offset);
		if (uc->misc_status & BIT(0))
			XDNA_ERR(xdna, "        Exception: PC=0x%x EAR=0x%x ESR=0x%x",
				 uc->uc_pc, uc->uc_ear, uc->uc_esr);
	}

	mutex_lock(&xdna->dev_lock);

	ndev->last_async_err.err_code =
		AMDXDNA_ERROR_ENCODE(err_num, AMDXDNA_ERROR_MODULE_AIE_CORE);
	ndev->last_async_err.ts_us = ktime_to_us(ktime_get_real());
	ndev->last_async_err.ex_err_code =
		((u64)aie4_health_get_ctx_status(health) << 32) | ctx_err->ctx_id;

	hwctx = hw_ctx_id2hwctx(ndev, ctx_err->ctx_id, &idx);
	if (hwctx) {
		struct amdxdna_hwctx_priv *priv = hwctx->priv;

		/*
		 * Only kernel-mode contexts consume this cached report (the timeout
		 * path attaches it to the failing command), and only they have an
		 * initialized io_lock.  Skip user-mode contexts: nothing reads the
		 * cache and their io_lock does not exist.
		 *
		 * io_lock serializes the multi-word cached_ctx_error against the
		 * reader in aie4_fill_health_data() - a valid flag alone orders
		 * publication but cannot stop this overwrite from tearing a
		 * concurrent read.  dev_lock -> io_lock nesting matches the rest of
		 * the driver.  last_async_err above stays unconditional (under
		 * dev_lock) so the GET_ARRAY async-error ioctl still works for all
		 * contexts.
		 */
		if (priv->kernel_submit) {
			mutex_lock(&priv->io_lock);
			memcpy(&priv->cached_ctx_error, ctx_err, sizeof(*ctx_err));
			priv->cached_ctx_error_valid = true;
			mutex_unlock(&priv->io_lock);
		}
		srcu_read_unlock(&hwctx->client->hwctx_srcu, idx);
	}

	mutex_unlock(&xdna->dev_lock);
}

static int aie4_error_async_cb(void *handle, void __iomem *data, size_t size)
{
	struct async_event *e = handle;

	if (data) {
		e->resp.type = readl(data +
				     offsetof(struct aie4_msg_async_event_config_resp, type));
		wmb(); /* Update status last so the worker needs no lock here. */
		e->resp.status = readl(data +
				       offsetof(struct aie4_msg_async_event_config_resp, status));
	}
	queue_work(e->wq, &e->work);
	return 0;
}

static int aie4_error_event_send(struct async_event *e)
{
	drm_clflush_virt_range(e->buf, e->size); /* device can access */
	return aie4_register_asyn_event_msg(e->ndev, e->addr, e->size, e, aie4_error_async_cb);
}

static void aie4_error_worker(struct work_struct *err_work)
{
	struct async_event *e = container_of(err_work, struct async_event, work);
	struct amdxdna_dev *xdna = e->ndev->aie.xdna;
	struct aie_err_info *info;
	u32 max_err;
	u32 err_col;

	if (e->resp.status == MAX_AIE4_MSG_STATUS_CODE)
		return;

	e->resp.status = MAX_AIE4_MSG_STATUS_CODE;

	print_hex_dump_debug("AIE error: ", DUMP_PREFIX_OFFSET, 16, 4, e->buf, 0x100, false);

	if (e->resp.type >= MAX_AIE4_ASYNC_EVENT_TYPE) {
		XDNA_WARN(xdna, "Unknown async event type %d, skipping", e->resp.type);
	} else if (e->resp.type == AIE4_ASYNC_EVENT_TYPE_CTX_ERROR) {
		struct aie4_async_ctx_error *ctx_err = (struct aie4_async_ctx_error *)e->buf;

		XDNA_ERR(xdna, "Context error: ctx_id=%u error_type=%u",
			 ctx_err->ctx_id, ctx_err->error_type);
		aie4_cache_ctx_error(e->ndev, ctx_err);
	} else {
		info = (struct aie_err_info *)e->buf;
		XDNA_DBG(xdna, "Error count %d return code %d", info->err_cnt, info->ret_code);

		max_err = (e->size - sizeof(*info)) / sizeof(struct aie_error);
		if (unlikely(info->err_cnt > max_err)) {
			WARN_ONCE(1, "Error count too large %d\n", info->err_cnt);
			goto reregister;
		}
		err_col = aie4_error_backtrack(e->ndev, info->payload, info->err_cnt);
		if (!err_col) {
			XDNA_WARN(xdna, "Did not get error column");
			goto reregister;
		}

		mutex_lock(&xdna->dev_lock);
		aie4_update_last_async_error(e->ndev, info->payload, info->err_cnt);
		mutex_unlock(&xdna->dev_lock);
	}

reregister:
	/*
	 * Re-register this event with firmware.  Skip if the management channel is
	 * gone: hw_stop/reset can race this worker after aie4_mailbox_fini() frees
	 * the channel and NULLs mgmt_chann, and sending then dereferences NULL.
	 * dev_lock serializes this check against teardown.
	 */
	mutex_lock(&xdna->dev_lock);
	if (e->ndev->aie.mgmt_chann && aie4_error_event_send(e))
		XDNA_WARN(xdna, "Unable to register async event");
	mutex_unlock(&xdna->dev_lock);
}

void aie4_error_async_events_free(struct amdxdna_dev_hdl *ndev)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	struct async_events *events;

	events = ndev->async_events;
	if (!events)
		return;
	ndev->async_events = NULL;

	mutex_unlock(&xdna->dev_lock);
	destroy_workqueue(events->wq);
	mutex_lock(&xdna->dev_lock);

	amdxdna_free_msg_buff(events->hdl);
	kfree(events);
}

int aie4_error_async_events_alloc(struct amdxdna_dev_hdl *ndev)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;
	u32 total_col = ndev->total_col;
	struct async_events *events;
	int i, ret;

	/* No columns yet (e.g. PF before partition setup); nothing to register. */
	if (!total_col)
		return 0;

	events = kzalloc_flex(*events, event, total_col);
	if (!events)
		return -ENOMEM;

	events->hdl = amdxdna_alloc_msg_buff(xdna, ASYNC_BUF_SIZE * total_col);
	if (IS_ERR(events->hdl)) {
		ret = PTR_ERR(events->hdl);
		goto free_events;
	}
	events->event_cnt = total_col;

	events->wq = alloc_ordered_workqueue("aie4_async_wq", 0);
	if (!events->wq) {
		ret = -ENOMEM;
		goto free_buf;
	}

	/*
	 * Publish before the registration loop, and do NOT free on a mid-loop
	 * failure below.  Each aie4_error_event_send() hands firmware a mailbox
	 * message whose handle points into events->event[]; once registered, a
	 * message is only released - firing aie4_error_async_cb() on its handle -
	 * when the mailbox channel is stopped.  On this failure path the channel
	 * is still up (the caller stops it later, via aie4_mailbox_fini() in its
	 * hw_start error unwind), so freeing events/wq here would leave firmware
	 * holding dangling handles and the channel-stop callback would touch freed
	 * memory (use-after-free) and a destroyed workqueue.  Instead leave the
	 * partially-registered state published and let the caller tear it down
	 * with aie4_error_async_events_free() *after* the mailbox is stopped -
	 * the same ordering the hw_stop path already relies on.
	 */
	ndev->async_events = events;

	for (i = 0; i < events->event_cnt; i++) {
		struct async_event *e = &events->event[i];
		u32 offset = i * ASYNC_BUF_SIZE;

		e->ndev = ndev;
		e->wq = events->wq;
		e->buf = to_cpu_addr(events->hdl, offset);
		e->addr = to_dma_addr(events->hdl, offset);
		e->size = ASYNC_BUF_SIZE;
		e->resp.status = MAX_AIE4_MSG_STATUS_CODE;
		INIT_WORK(&e->work, aie4_error_worker);

		ret = aie4_error_event_send(e);
		if (ret)
			return ret;
	}

	XDNA_DBG(xdna, "Async event count %d, buf total size 0x%x",
		 events->event_cnt, to_buf_size(events->hdl));
	return 0;

free_buf:
	/* Reached only before any message is registered, so nothing to drain. */
	amdxdna_free_msg_buff(events->hdl);
free_events:
	kfree(events);
	return ret;
}

int aie4_get_array_async_error(struct amdxdna_dev_hdl *ndev, struct amdxdna_drm_get_array *args)
{
	struct amdxdna_dev *xdna = ndev->aie.xdna;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	if (!args->num_element)
		return -EINVAL;

	args->num_element = 1;
	args->element_size = min(args->element_size, sizeof(ndev->last_async_err));
	if (copy_to_user(u64_to_user_ptr(args->buffer),
			 &ndev->last_async_err, args->element_size))
		return -EFAULT;

	return 0;
}
