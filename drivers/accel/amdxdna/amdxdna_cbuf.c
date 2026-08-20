// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <drm/drm_mm.h>
#include <drm/drm_prime.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/string.h>

#include "amdxdna_cbuf.h"
#include "amdxdna_drv.h"

/*
 * Contiguous DRAM allocation for the OF platform (and x86 debugfs bring-up).
 *
 * Memory comes from "banks" keyed by a fixed, userspace-facing bank id
 * (create-BO flags bit N -> bank id N): id AMDXDNA_MEM_BANK_FW (0) is the
 * firmware-visible bank, ids >= 1 are AIE/CERT app banks. A bank's memory is
 * amdxdna's own reserved-memory (a named region, drm_mm-suballocated and mapped
 * with dma_map_resource + ioremap_cache) or system CMA when it has no region.
 * The firmware bank must be placed below 4 GB so the firmware processor, which
 * only reaches 32-bit, can dereference it; that reachability is a property of
 * the reserved-memory placement, not of any borrowed DMA master. When a bank is
 * absent (x86 bring-up), allocation falls back to the debugfs carveout, then to
 * system-default CMA on the platform device. All backings are cacheable;
 * coherency is software-managed (SYNC_BO / drm_clflush + DPT reads).
 */

/* Debugfs single carveout (x86 bring-up). */
struct amdxdna_carveout {
	u64		addr;
	u64		size;
	struct drm_mm	mm;
	struct mutex	lock; /* protect mm */
};

/*
 * One memory bank. @dma_dev is the device the bank's buffers are DMA-mapped
 * through. When @carveout is true the bank sub-allocates a reserved region via
 * @mm; otherwise it is system CMA (dma_alloc on @dma_dev).
 */
struct amdxdna_mem_bank {
	struct device	*dma_dev;
	bool		fw;		/* firmware-visible (id AMDXDNA_MEM_BANK_FW) */
	bool		carveout;	/* true: drm_mm region; false: system CMA */
	u64		addr;
	u64		size;
	struct drm_mm	mm;
	struct mutex	lock; /* protect mm */
};

bool amdxdna_mem_banks_present(struct amdxdna_dev *xdna)
{
	unsigned long id;
	void *bank;

	xa_for_each(&xdna->banks, id, bank)
		return true;

	return false;
}

bool amdxdna_use_carveout(struct amdxdna_dev *xdna)
{
	return xdna->carveout || amdxdna_mem_banks_present(xdna);
}

void amdxdna_get_carveout_conf(struct amdxdna_dev *xdna, u64 *addr, u64 *size)
{
	if (xdna->carveout) {
		*addr = xdna->carveout->addr;
		*size = xdna->carveout->size;
	} else {
		*addr = 0;
		*size = 0;
	}
}

int amdxdna_carveout_init(struct amdxdna_dev *xdna, u64 carveout_addr, u64 carveout_size)
{
	struct amdxdna_carveout *carveout;

	/* Only allow carveout memory to be set up once. */
	if (xdna->carveout) {
		XDNA_ERR(xdna, "Carveout memory has already been set up.");
		return -EBUSY;
	}

	carveout = kzalloc_obj(*carveout);
	if (!carveout)
		return -ENOMEM;

	carveout->addr = carveout_addr;
	carveout->size = carveout_size;
	mutex_init(&carveout->lock);
	drm_mm_init(&carveout->mm, carveout->addr, carveout->size);

	xdna->carveout = carveout;
	XDNA_INFO(xdna, "Use carveout mem: 0x%llx@0x%llx\n", carveout->size, carveout->addr);
	return 0;
}

void amdxdna_carveout_fini(struct amdxdna_dev *xdna)
{
	struct amdxdna_carveout *carveout = xdna->carveout;

	if (!carveout)
		return;

	XDNA_INFO(xdna, "Cleanup carveout mem: 0x%llx@0x%llx\n", carveout->size, carveout->addr);
	mutex_destroy(&carveout->lock);
	drm_mm_takedown(&carveout->mm);
	kfree(carveout);
	xdna->carveout = NULL;
}

/*
 * Scatterlist entries cannot describe more than 4 GiB per node on some
 * platforms; use 2 GiB per entry and keep bus addresses contiguous.
 */
#define AMDXDNA_CBUF_MAX_SG_LEN	(2UL * 1024 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Carveout backing: drm_mm node + dma_map_resource + ioremap_cache.  */
/* ------------------------------------------------------------------ */

struct amdxdna_cbuf_priv {
	struct amdxdna_dev	*xdna;
	struct device		*dev;
	struct drm_mm		*mm;
	struct mutex		*lock;	/* protects *mm */
	struct drm_mm_node	node;
};

static struct sg_table *amdxdna_cbuf_map(struct dma_buf_attachment *attach,
					 enum dma_data_direction direction)
{
	struct amdxdna_cbuf_priv *cbuf = attach->dmabuf->priv;
	struct scatterlist *sgl, *sg;
	int ret, n_entries, i;
	struct sg_table *sgt;
	dma_addr_t dma_addr;
	size_t dma_size;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	n_entries = (cbuf->node.size + AMDXDNA_CBUF_MAX_SG_LEN - 1) / AMDXDNA_CBUF_MAX_SG_LEN;
	sgl = kzalloc_objs(*sg, n_entries);
	if (!sgl) {
		ret = -ENOMEM;
		goto free_sgt;
	}
	sg_init_table(sgl, n_entries);
	sgt->orig_nents = n_entries;
	sgt->nents = n_entries;
	sgt->sgl = sgl;

	dma_size = cbuf->node.size;
	/* Map against cbuf->dev (region owner) so its mask/stream ID apply. */
	dma_addr = dma_map_resource(cbuf->dev, cbuf->node.start, dma_size,
				    direction, DMA_ATTR_SKIP_CPU_SYNC);
	ret = dma_mapping_error(cbuf->dev, dma_addr);
	if (ret) {
		XDNA_ERR(cbuf->xdna, "dma_map_resource carveout failed, ret %d", ret);
		goto free_sgl;
	}

	for_each_sgtable_dma_sg(sgt, sg, i) {
		size_t len = min_t(size_t, AMDXDNA_CBUF_MAX_SG_LEN, dma_size);

		sg_dma_address(sg) = dma_addr;
		sg_dma_len(sg) = len;
		dma_addr += len;
		dma_size -= len;
	}

	return sgt;

free_sgl:
	kfree(sgl);
free_sgt:
	kfree(sgt);
	return ERR_PTR(ret);
}

static void amdxdna_cbuf_unmap(struct dma_buf_attachment *attach,
			       struct sg_table *sgt,
			       enum dma_data_direction direction)
{
	struct amdxdna_cbuf_priv *cbuf = attach->dmabuf->priv;

	dma_unmap_resource(cbuf->dev, sg_dma_address(sgt->sgl),
			   drm_prime_get_contiguous_size(sgt), direction,
			   DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

static void amdxdna_cbuf_release(struct dma_buf *dbuf)
{
	struct amdxdna_cbuf_priv *cbuf = dbuf->priv;

	mutex_lock(cbuf->lock);
	drm_mm_remove_node(&cbuf->node);
	mutex_unlock(cbuf->lock);

	kfree(cbuf);
}

static vm_fault_t amdxdna_cbuf_vm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct amdxdna_cbuf_priv *cbuf;
	unsigned long pfn;
	pgoff_t pgoff;

	cbuf = vma->vm_private_data;
	pgoff = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	pfn = (cbuf->node.start >> PAGE_SHIFT) + pgoff;

	return vmf_insert_pfn(vma, vmf->address, pfn);
}

static const struct vm_operations_struct amdxdna_cbuf_vm_ops = {
	.fault = amdxdna_cbuf_vm_fault,
};

static int amdxdna_cbuf_mmap(struct dma_buf *dbuf, struct vm_area_struct *vma)
{
	struct amdxdna_cbuf_priv *cbuf = dbuf->priv;

	vma->vm_ops = &amdxdna_cbuf_vm_ops;
	vma->vm_private_data = cbuf;
	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	return 0;
}

static int amdxdna_cbuf_vmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	struct amdxdna_cbuf_priv *cbuf = dbuf->priv;
	void *kva;

	kva = ioremap_cache(cbuf->node.start, cbuf->node.size);
	if (!kva) {
		XDNA_ERR(cbuf->xdna, "Failed to vmap carveout dma buf");
		return -EINVAL;
	}

	iosys_map_set_vaddr(map, kva);
	return 0;
}

static void amdxdna_cbuf_vunmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	iounmap(map->vaddr);
}

static const struct dma_buf_ops amdxdna_cbuf_dmabuf_ops = {
	.map_dma_buf = amdxdna_cbuf_map,
	.unmap_dma_buf = amdxdna_cbuf_unmap,
	.release = amdxdna_cbuf_release,
	.mmap = amdxdna_cbuf_mmap,
	.vmap = amdxdna_cbuf_vmap,
	.vunmap = amdxdna_cbuf_vunmap,
};

static void amdxdna_cbuf_clear(struct dma_buf *dbuf)
{
	struct iosys_map vmap = IOSYS_MAP_INIT_VADDR(NULL);

	dma_buf_vmap(dbuf, &vmap);
	if (!vmap.vaddr)
		return;

	memset(vmap.vaddr, 0, dbuf->size);
	dma_buf_vunmap(dbuf, &vmap);
}

static struct dma_buf *amdxdna_cbuf_alloc(struct amdxdna_dev *xdna,
					  struct device *dev, struct drm_mm *mm,
					  struct mutex *lock, size_t size, u64 align)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct amdxdna_cbuf_priv *cbuf;
	struct dma_buf *dbuf;
	int ret;

	cbuf = kzalloc_obj(*cbuf);
	if (!cbuf)
		return ERR_PTR(-ENOMEM);
	cbuf->xdna = xdna;
	cbuf->dev = dev;
	cbuf->mm = mm;
	cbuf->lock = lock;

	mutex_lock(lock);
	ret = drm_mm_insert_node_generic(mm, &cbuf->node, size, align, 0,
					 DRM_MM_INSERT_BEST);
	mutex_unlock(lock);
	if (ret)
		goto free_cbuf;

	exp_info.size = size;
	exp_info.ops = &amdxdna_cbuf_dmabuf_ops;
	exp_info.priv = cbuf;
	exp_info.flags = O_RDWR;
	dbuf = dma_buf_export(&exp_info);
	if (IS_ERR(dbuf)) {
		ret = PTR_ERR(dbuf);
		goto remove_node;
	}

	amdxdna_cbuf_clear(dbuf);
	return dbuf;

remove_node:
	mutex_lock(lock);
	drm_mm_remove_node(&cbuf->node);
	mutex_unlock(lock);
free_cbuf:
	kfree(cbuf);
	return ERR_PTR(ret);
}

/* ------------------------------------------------------------------ */
/* System-CMA backing: dma_alloc_noncoherent on the bank/platform dev. */
/* ------------------------------------------------------------------ */

struct amdxdna_cmabuf_priv {
	struct amdxdna_dev	*xdna;
	struct device		*dev;
	dma_addr_t		dma_addr;
	void			*cpu_addr;
	size_t			size;
};

static struct sg_table *amdxdna_cmabuf_map(struct dma_buf_attachment *attach,
					   enum dma_data_direction direction)
{
	struct amdxdna_cmabuf_priv *cmabuf = attach->dmabuf->priv;
	struct scatterlist *sgl, *sg;
	int ret, n_entries, i;
	struct sg_table *sgt;
	dma_addr_t dma_addr;
	size_t dma_size;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	n_entries = (cmabuf->size + AMDXDNA_CBUF_MAX_SG_LEN - 1) / AMDXDNA_CBUF_MAX_SG_LEN;
	sgl = kzalloc_objs(*sg, n_entries);
	if (!sgl) {
		ret = -ENOMEM;
		goto free_sgt;
	}
	sg_init_table(sgl, n_entries);
	sgt->orig_nents = n_entries;
	sgt->nents = n_entries;
	sgt->sgl = sgl;

	/*
	 * dma_alloc_noncoherent() already returned a dma_addr valid for the
	 * producer (cmabuf->dev, behind its DT stream ID); describe that single
	 * contiguous range in the sgt (segmented) without re-mapping.
	 */
	dma_addr = cmabuf->dma_addr;
	dma_size = cmabuf->size;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		size_t len = min_t(size_t, AMDXDNA_CBUF_MAX_SG_LEN, dma_size);

		sg_dma_address(sg) = dma_addr;
		sg_dma_len(sg) = len;
		dma_addr += len;
		dma_size -= len;
	}

	return sgt;

free_sgt:
	kfree(sgt);
	return ERR_PTR(ret);
}

static void amdxdna_cmabuf_unmap(struct dma_buf_attachment *attach,
				 struct sg_table *sgt,
				 enum dma_data_direction direction)
{
	kfree(sgt->sgl);
	kfree(sgt);
}

static void amdxdna_cmabuf_release(struct dma_buf *dbuf)
{
	struct amdxdna_cmabuf_priv *cmabuf = dbuf->priv;

	if (!cmabuf)
		return;

	dma_free_noncoherent(cmabuf->dev, cmabuf->size, cmabuf->cpu_addr,
			     cmabuf->dma_addr, DMA_BIDIRECTIONAL);
	kfree(cmabuf);
	dbuf->priv = NULL;
}

static int amdxdna_cmabuf_mmap(struct dma_buf *dbuf, struct vm_area_struct *vma)
{
	struct amdxdna_cmabuf_priv *cmabuf = dbuf->priv;
	size_t size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff)
		return -EINVAL;
	if (size > cmabuf->size)
		return -EINVAL;

	/* Keep the userspace mapping cached (default prot); coherency via SYNC_BO. */
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	return remap_pfn_range(vma, vma->vm_start,
			       page_to_pfn(virt_to_page(cmabuf->cpu_addr)), size,
			       vma->vm_page_prot);
}

static int amdxdna_cmabuf_vmap(struct dma_buf *dbuf, struct iosys_map *map)
{
	struct amdxdna_cmabuf_priv *cmabuf = dbuf->priv;

	iosys_map_set_vaddr(map, cmabuf->cpu_addr);
	return 0;
}

static const struct dma_buf_ops amdxdna_cmabuf_dmabuf_ops = {
	.map_dma_buf = amdxdna_cmabuf_map,
	.unmap_dma_buf = amdxdna_cmabuf_unmap,
	.release = amdxdna_cmabuf_release,
	.mmap = amdxdna_cmabuf_mmap,
	.vmap = amdxdna_cmabuf_vmap,
};

static struct dma_buf *amdxdna_cmabuf_alloc(struct amdxdna_dev *xdna,
					    struct device *dev, size_t size)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct amdxdna_cmabuf_priv *cmabuf;
	struct dma_buf *dbuf;
	dma_addr_t dma_addr;
	void *cpu_addr;
	int ret;

	cmabuf = kzalloc_obj(*cmabuf);
	if (!cmabuf)
		return ERR_PTR(-ENOMEM);

	size = PAGE_ALIGN(size);
	cpu_addr = dma_alloc_noncoherent(dev, size, &dma_addr, DMA_BIDIRECTIONAL,
					 GFP_KERNEL);
	if (!cpu_addr) {
		XDNA_DBG(xdna, "CMA alloc failed on %s: size 0x%zx", dev_name(dev), size);
		ret = -ENOMEM;
		goto free_cmabuf;
	}

	cmabuf->xdna = xdna;
	cmabuf->dev = dev;
	cmabuf->cpu_addr = cpu_addr;
	cmabuf->dma_addr = dma_addr;
	cmabuf->size = size;

	exp_info.size = size;
	exp_info.ops = &amdxdna_cmabuf_dmabuf_ops;
	exp_info.priv = cmabuf;
	exp_info.flags = O_RDWR;
	dbuf = dma_buf_export(&exp_info);
	if (IS_ERR(dbuf)) {
		ret = PTR_ERR(dbuf);
		goto free_dma;
	}

	return dbuf;

free_dma:
	dma_free_noncoherent(dev, size, cpu_addr, dma_addr, DMA_BIDIRECTIONAL);
free_cmabuf:
	kfree(cmabuf);
	return ERR_PTR(ret);
}

/* ------------------------------------------------------------------ */
/* Bank selection.                                                     */
/* ------------------------------------------------------------------ */

static struct dma_buf *amdxdna_bank_get(struct amdxdna_dev *xdna,
					struct amdxdna_mem_bank *bank,
					size_t size, u64 align)
{
	if (bank->carveout)
		return amdxdna_cbuf_alloc(xdna, bank->dma_dev, &bank->mm, &bank->lock,
					  size, align);

	return amdxdna_cmabuf_alloc(xdna, bank->dma_dev, size);
}

/* First app bank (id != AMDXDNA_MEM_BANK_FW), or NULL. */
static struct amdxdna_mem_bank *amdxdna_first_app_bank(struct amdxdna_dev *xdna)
{
	struct amdxdna_mem_bank *bank;
	unsigned long id;

	xa_for_each(&xdna->banks, id, bank)
		if (!bank->fw)
			return bank;

	return NULL;
}

struct dma_buf *amdxdna_get_cbuf(struct drm_device *dev, size_t size,
				 u64 alignment, u64 flags)
{
	struct amdxdna_dev *xdna = to_xdna_dev(dev);
	u32 bitmap = (u32)(flags & 0xFFULL);
	struct amdxdna_mem_bank *bank;
	struct dma_buf *dbuf;
	int i;

	/* Requested bank id(s): try each set bit in ascending order. */
	for (i = 0; i < MAX_MEM_REGIONS; i++) {
		if (!(bitmap & (1U << i)))
			continue;

		bank = xa_load(&xdna->banks, i);
		if (!bank)
			continue;

		dbuf = amdxdna_bank_get(xdna, bank, size, alignment);
		if (!IS_ERR(dbuf))
			return dbuf;
	}

	/* Default (no bank requested): first app bank. */
	if (!bitmap) {
		bank = amdxdna_first_app_bank(xdna);
		if (bank) {
			dbuf = amdxdna_bank_get(xdna, bank, size, alignment);
			if (!IS_ERR(dbuf))
				return dbuf;
		}
	}

	/* Fallback: debugfs carveout, then system-default CMA. */
	if (xdna->carveout)
		return amdxdna_cbuf_alloc(xdna, dev->dev, &xdna->carveout->mm,
					  &xdna->carveout->lock, size, alignment);

	return amdxdna_cmabuf_alloc(xdna, dev->dev, size);
}

/* ------------------------------------------------------------------ */
/* Kernel-side firmware buffers (vaddr + dma_addr).                    */
/* ------------------------------------------------------------------ */

struct amdxdna_cbuf_kbuf {
	bool			carveout;
	/* carveout */
	struct device		*dev;
	struct drm_mm		*mm;
	struct mutex		*lock;	/* protects *mm */
	struct drm_mm_node	node;
	/* common */
	void			*vaddr;
	dma_addr_t		dma_addr;
	size_t			size;
	enum dma_data_direction	dir;
};

static int amdxdna_kbuf_carveout(struct amdxdna_cbuf_kbuf *kbuf, struct device *dev,
				 struct drm_mm *mm, struct mutex *lock,
				 size_t size, enum dma_data_direction dir)
{
	int ret;

	kbuf->carveout = true;
	kbuf->dev = dev;
	kbuf->mm = mm;
	kbuf->lock = lock;

	mutex_lock(lock);
	ret = drm_mm_insert_node_generic(mm, &kbuf->node, size, 0, 0,
					 DRM_MM_INSERT_BEST);
	mutex_unlock(lock);
	if (ret)
		return ret;

	kbuf->dma_addr = dma_map_resource(dev, kbuf->node.start, size, dir,
					  DMA_ATTR_SKIP_CPU_SYNC);
	if (dma_mapping_error(dev, kbuf->dma_addr)) {
		ret = -ENOMEM;
		goto remove_node;
	}

	kbuf->vaddr = ioremap_cache(kbuf->node.start, size);
	if (!kbuf->vaddr) {
		ret = -ENOMEM;
		goto unmap;
	}

	return 0;

unmap:
	dma_unmap_resource(dev, kbuf->dma_addr, size, dir, DMA_ATTR_SKIP_CPU_SYNC);
remove_node:
	mutex_lock(lock);
	drm_mm_remove_node(&kbuf->node);
	mutex_unlock(lock);
	return ret;
}

static int amdxdna_kbuf_cma(struct amdxdna_cbuf_kbuf *kbuf, struct device *dev,
			    size_t size, enum dma_data_direction dir)
{
	kbuf->carveout = false;
	kbuf->dev = dev;
	kbuf->vaddr = dma_alloc_noncoherent(dev, size, &kbuf->dma_addr, dir,
					    GFP_KERNEL);
	return kbuf->vaddr ? 0 : -ENOMEM;
}

void *amdxdna_cbuf_kalloc(struct amdxdna_dev *xdna, size_t size, bool fw,
			  enum dma_data_direction dir,
			  void **vaddr, dma_addr_t *dma_addr)
{
	u32 id = fw ? AMDXDNA_MEM_BANK_FW : AMDXDNA_MEM_BANK_AIE;
	struct amdxdna_mem_bank *bank = xa_load(&xdna->banks, id);
	struct amdxdna_cbuf_kbuf *kbuf;
	int ret;

	kbuf = kzalloc_obj(*kbuf);
	if (!kbuf)
		return ERR_PTR(-ENOMEM);
	kbuf->size = size;
	kbuf->dir = dir;

	if (bank && bank->carveout)
		ret = amdxdna_kbuf_carveout(kbuf, bank->dma_dev, &bank->mm, &bank->lock,
					    size, dir);
	else if (bank)
		ret = amdxdna_kbuf_cma(kbuf, bank->dma_dev, size, dir);
	else if (xdna->carveout)
		ret = amdxdna_kbuf_carveout(kbuf, xdna->ddev.dev, &xdna->carveout->mm,
					    &xdna->carveout->lock, size, dir);
	else
		ret = amdxdna_kbuf_cma(kbuf, xdna->ddev.dev, size, dir);

	if (ret) {
		kfree(kbuf);
		return ERR_PTR(ret);
	}

	*vaddr = kbuf->vaddr;
	*dma_addr = kbuf->dma_addr;
	return kbuf;
}

void amdxdna_cbuf_kfree(void *cookie)
{
	struct amdxdna_cbuf_kbuf *kbuf = cookie;

	if (!kbuf)
		return;

	if (kbuf->carveout) {
		iounmap((void __iomem *)kbuf->vaddr);
		dma_unmap_resource(kbuf->dev, kbuf->dma_addr, kbuf->size, kbuf->dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
		mutex_lock(kbuf->lock);
		drm_mm_remove_node(&kbuf->node);
		mutex_unlock(kbuf->lock);
	} else {
		dma_free_noncoherent(kbuf->dev, kbuf->size, kbuf->vaddr,
				     kbuf->dma_addr, kbuf->dir);
	}
	kfree(kbuf);
}

/* ------------------------------------------------------------------ */
/* DT bank parsing.                                                    */
/* ------------------------------------------------------------------ */

/* Master DMA device a bank's buffers are mapped through. */
static struct device *amdxdna_bank_dma_dev(struct amdxdna_dev *xdna, bool fw)
{
	return xdna->ddev.dev;
}

/*
 * Create a bank, DMA-mapped through @dma_dev (this device). Memory is amdxdna's
 * own reserved-memory at memory-region index @region_idx (a "carveout"); pass
 * @region_idx < 0 (or a node without that region) to back the bank with system
 * CMA instead.
 */
static struct amdxdna_mem_bank *
amdxdna_bank_create(struct amdxdna_dev *xdna, struct device_node *np,
		    int region_idx, struct device *dma_dev, u32 id, bool fw)
{
	struct device_node *rmem_np = NULL;
	struct amdxdna_mem_bank *bank;
	struct reserved_mem *rmem;

	bank = kzalloc_obj(*bank);
	if (!bank)
		return ERR_PTR(-ENOMEM);
	bank->dma_dev = dma_dev;
	bank->fw = fw;

	/* memory-region optional: present -> carveout, absent -> system CMA. */
	if (np && region_idx >= 0)
		rmem_np = of_parse_phandle(np, "memory-region", region_idx);
	if (rmem_np) {
		rmem = of_reserved_mem_lookup(rmem_np);
		of_node_put(rmem_np);
		if (!rmem) {
			XDNA_ERR(xdna, "bank %u: bad memory-region", id);
			kfree(bank);
			return ERR_PTR(-EINVAL);
		}
		bank->carveout = true;
		bank->addr = rmem->base;
		bank->size = rmem->size;
		mutex_init(&bank->lock);
		drm_mm_init(&bank->mm, bank->addr, bank->size);
		XDNA_INFO(xdna, "%s bank %u: carveout 0x%llx@0x%llx on %s",
			  fw ? "fw" : "aie", id, bank->size, bank->addr,
			  dev_name(dma_dev));
	} else {
		XDNA_INFO(xdna, "%s bank %u: system CMA on %s", fw ? "fw" : "aie",
			  id, dev_name(dma_dev));
	}

	return bank;
}

static void amdxdna_mem_bank_free(struct amdxdna_mem_bank *bank)
{
	if (bank->carveout) {
		drm_mm_takedown(&bank->mm);
		mutex_destroy(&bank->lock);
	}
	kfree(bank);
}

static int amdxdna_bank_store(struct amdxdna_dev *xdna, struct amdxdna_mem_bank *bank,
			      u32 id)
{
	void *old;

	old = xa_store(&xdna->banks, id, bank, GFP_KERNEL);
	if (xa_is_err(old)) {
		amdxdna_mem_bank_free(bank);
		return xa_err(old);
	}
	if (old) {
		XDNA_ERR(xdna, "Duplicate bank id %u", id);
		xa_store(&xdna->banks, id, old, GFP_KERNEL);
		amdxdna_mem_bank_free(bank);
		return -EINVAL;
	}
	return 0;
}

/* Create a system-CMA bank at @id when the DT did not name one. */
static int amdxdna_bank_ensure(struct amdxdna_dev *xdna, u32 id, bool fw)
{
	struct amdxdna_mem_bank *bank;

	if (xa_load(&xdna->banks, id))
		return 0;

	bank = amdxdna_bank_create(xdna, NULL, -1, amdxdna_bank_dma_dev(xdna, fw),
				   id, fw);
	if (IS_ERR(bank))
		return PTR_ERR(bank);

	return amdxdna_bank_store(xdna, bank, id);
}

/*
 * Bind the memory banks from the amdxdna node's memory-region-names. "fw" is the
 * firmware bank (id AMDXDNA_MEM_BANK_FW, whose region must be placed below 4 GB);
 * "aie<N>" are the app banks (ids AMDXDNA_MEM_BANK_AIE + N). Other names (mgmt,
 * doorbell, ...) are ignored here. On the OF platform the firmware bank and the
 * first app bank are always available: any bank the DT did not name defaults to
 * system CMA on this device.
 */
int amdxdna_mem_banks_init(struct amdxdna_dev *xdna, struct device_node *np)
{
	int count, i, ret;

	xa_init(&xdna->banks);

	if (!np)
		return 0;

	count = of_property_count_strings(np, "memory-region-names");
	if (count < 0)
		count = 0;

	for (i = 0; i < count; i++) {
		struct amdxdna_mem_bank *bank;
		const char *name;
		bool fw = false;
		u32 id;

		ret = of_property_read_string_index(np, "memory-region-names", i,
						    &name);
		if (ret)
			goto err;

		if (!strcmp(name, "fw")) {
			fw = true;
			id = AMDXDNA_MEM_BANK_FW;
		} else if (!strncmp(name, "aie", 3) &&
			   !kstrtou32(name + 3, 10, &id)) {
			id += AMDXDNA_MEM_BANK_AIE;
		} else {
			continue;
		}

		if (id >= MAX_MEM_REGIONS) {
			XDNA_ERR(xdna, "bank id %u (\"%s\") out of range", id, name);
			ret = -EINVAL;
			goto err;
		}
		if (xa_load(&xdna->banks, id)) {
			XDNA_ERR(xdna, "Duplicate bank id %u (\"%s\")", id, name);
			ret = -EINVAL;
			goto err;
		}

		bank = amdxdna_bank_create(xdna, np, i,
					   amdxdna_bank_dma_dev(xdna, fw), id, fw);
		if (IS_ERR(bank)) {
			ret = PTR_ERR(bank);
			goto err;
		}

		ret = amdxdna_bank_store(xdna, bank, id);
		if (ret)
			goto err;
	}

	ret = amdxdna_bank_ensure(xdna, AMDXDNA_MEM_BANK_FW, true);
	if (ret)
		goto err;

	ret = amdxdna_bank_ensure(xdna, AMDXDNA_MEM_BANK_AIE, false);
	if (ret)
		goto err;

	return 0;

err:
	amdxdna_mem_banks_fini(xdna);
	return ret;
}

void amdxdna_mem_banks_fini(struct amdxdna_dev *xdna)
{
	struct amdxdna_mem_bank *bank;
	unsigned long id;

	xa_for_each(&xdna->banks, id, bank) {
		xa_erase(&xdna->banks, id);
		amdxdna_mem_bank_free(bank);
	}
	xa_destroy(&xdna->banks);
}
