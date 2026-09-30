// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include "drm/amdxdna_accel.h"
#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_vma_manager.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "amdxdna_cma_buf.h"
#include "amdxdna_drv.h"
#include "amdxdna_gem.h"

/*
 * CMA create-BO backing.  Physically contiguous, cacheable pages from the
 * device's DMA pool: the DT-reserved "aie"/fw region when a reusable
 * shared-dma-pool memory-region is bound to the device (dev->cma_area),
 * otherwise the system CMA.  dma_alloc_pages() returns cacheable pages (fast
 * for command-BO writes); coherency with a non-coherent CERT is done by
 * SYNC_BO, which is a no-op on a cache-coherent device, so this backing is
 * correct on the coherent npu3a part too.
 *
 * The BO is a native DRM GEM object -- there is no raw dma_buf_export() here.
 * Userspace obtains a dma-buf through the standard PRIME path
 * (DRM_IOCTL_PRIME_HANDLE_TO_FD -> fallback to drm_gem_prime_export).
 */

static void amdxdna_gem_cma_obj_free(struct drm_gem_object *gobj)
{
	struct amdxdna_dev *xdna = to_xdna_dev(gobj->dev);
	struct amdxdna_gem_obj *abo = to_xdna_obj(gobj);
	struct device *dev = xdna->ddev.dev;

	/* Drop the device mapping (iova, or the streaming sgt) taken while in use. */
	amdxdna_dma_unmap_bo(xdna, abo);
	if (abo->mem.sgt) {
		dma_unmap_sgtable(dev, abo->mem.sgt, DMA_BIDIRECTIONAL, 0);
		sg_free_table(abo->mem.sgt);
		kfree(abo->mem.sgt);
	}

	dma_free_pages(dev, abo->mem.size, abo->mem.pages[0], abo->cma_dma_addr,
		       DMA_BIDIRECTIONAL);
	kvfree(abo->mem.pages);
	drm_gem_object_release(gobj);
	amdxdna_gem_destroy_obj(abo);
}

static int amdxdna_gem_cma_obj_vmap(struct drm_gem_object *gobj, struct iosys_map *map)
{
	struct amdxdna_gem_obj *abo = to_xdna_obj(gobj);

	/* Contiguous lowmem pages: the linear kernel mapping is the vaddr. */
	iosys_map_set_vaddr(map, page_address(abo->mem.pages[0]));
	return 0;
}

static void amdxdna_gem_cma_obj_vunmap(struct drm_gem_object *gobj, struct iosys_map *map)
{
	/* page_to_virt() is not a separate mapping; nothing to tear down. */
	iosys_map_clear(map);
}

static int amdxdna_gem_cma_obj_mmap(struct drm_gem_object *gobj, struct vm_area_struct *vma)
{
	struct amdxdna_dev *xdna = to_xdna_dev(gobj->dev);
	struct amdxdna_gem_obj *abo = to_xdna_obj(gobj);

	/*
	 * drm_gem_mmap() encodes a fake buffer offset in vm_pgoff and sets
	 * VM_PFNMAP; the CMA backing is real struct pages, so map the whole
	 * buffer from offset 0 with the DMA API page helper (cacheable prot).
	 */
	vma->vm_pgoff -= drm_vma_node_start(&gobj->vma_node);
	vm_flags_mod(vma, VM_DONTEXPAND, VM_PFNMAP);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

	return dma_mmap_pages(xdna->ddev.dev, vma, vma->vm_end - vma->vm_start,
			      abo->mem.pages[0]);
}

/*
 * Unmapped sg_table describing the backing pages, for the standard GEM PRIME
 * export path (drm_gem_map_dma_buf() then maps it for the importer).  Distinct
 * from amdxdna_gem_get_sgt(), which returns the device-mapped, cached sgt used
 * internally; the table returned here is freed by drm_gem_unmap_dma_buf().
 */
static struct sg_table *amdxdna_gem_cma_get_sg_table(struct drm_gem_object *gobj)
{
	struct amdxdna_gem_obj *abo = to_xdna_obj(gobj);
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table_from_pages(sgt, abo->mem.pages, abo->mem.nr_pages,
					0, abo->mem.size, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static const struct vm_operations_struct amdxdna_gem_cma_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct drm_gem_object_funcs amdxdna_gem_cma_obj_funcs = {
	.free = amdxdna_gem_cma_obj_free,
	.open = amdxdna_gem_obj_open,
	.close = amdxdna_gem_obj_close,
	.vmap = amdxdna_gem_cma_obj_vmap,
	.vunmap = amdxdna_gem_cma_obj_vunmap,
	.mmap = amdxdna_gem_cma_obj_mmap,
	.vm_ops = &amdxdna_gem_cma_vm_ops,
	.get_sg_table = amdxdna_gem_cma_get_sg_table,
};

/*
 * Debug backing (debugfs "cma_coherent"): non-cacheable coherent memory from
 * dma_alloc_coherent(), addressed directly by its dma_addr and needing no cache
 * maintenance (amdxdna_gem_dma_sync_range() no-ops on abo->coherent). Kept as a
 * separate GEM funcs set and allocator so the default cacheable path above stays
 * untouched. No page array is built, so it is not PRIME-exportable (no
 * .get_sg_table). Used to compare cache-sync (SYNC_BO) overhead against the
 * cacheable backing.
 */
static void amdxdna_gem_coherent_obj_free(struct drm_gem_object *gobj)
{
	struct amdxdna_dev *xdna = to_xdna_dev(gobj->dev);
	struct amdxdna_gem_obj *abo = to_xdna_obj(gobj);

	amdxdna_dma_unmap_bo(xdna, abo);
	dma_free_coherent(xdna->ddev.dev, abo->mem.size, abo->mem.kva,
			  abo->cma_dma_addr);
	drm_gem_object_release(gobj);
	amdxdna_gem_destroy_obj(abo);
}

static int amdxdna_gem_coherent_obj_vmap(struct drm_gem_object *gobj, struct iosys_map *map)
{
	struct amdxdna_gem_obj *abo = to_xdna_obj(gobj);

	/* The non-cacheable CPU address from dma_alloc_coherent(). */
	iosys_map_set_vaddr(map, abo->mem.kva);
	return 0;
}

static int amdxdna_gem_coherent_obj_mmap(struct drm_gem_object *gobj, struct vm_area_struct *vma)
{
	struct amdxdna_dev *xdna = to_xdna_dev(gobj->dev);
	struct amdxdna_gem_obj *abo = to_xdna_obj(gobj);

	/* drm_gem_mmap() encodes a fake buffer offset in vm_pgoff; rebase to 0. */
	vma->vm_pgoff -= drm_vma_node_start(&gobj->vma_node);

	/* dma_mmap_coherent() maps the non-cacheable region and sets prot itself. */
	return dma_mmap_coherent(xdna->ddev.dev, vma, abo->mem.kva,
				 abo->cma_dma_addr, vma->vm_end - vma->vm_start);
}

static const struct drm_gem_object_funcs amdxdna_gem_coherent_obj_funcs = {
	.free = amdxdna_gem_coherent_obj_free,
	.open = amdxdna_gem_obj_open,
	.close = amdxdna_gem_obj_close,
	.vmap = amdxdna_gem_coherent_obj_vmap,
	.vunmap = amdxdna_gem_cma_obj_vunmap,
	.mmap = amdxdna_gem_coherent_obj_mmap,
	.vm_ops = &amdxdna_gem_cma_vm_ops,
};

static struct amdxdna_gem_obj *
amdxdna_get_coherent_buf(struct drm_device *dev, struct amdxdna_drm_create_bo *args)
{
	struct amdxdna_dev *xdna = to_xdna_dev(dev);
	struct device *cma_dev = xdna->ddev.dev;
	size_t size = PAGE_ALIGN(args->size);
	struct amdxdna_gem_obj *abo;
	dma_addr_t dma_addr;
	void *kva;
	u64 align;
	int ret;

	if (!size) {
		XDNA_ERR(xdna, "Invalid BO size 0x%llx", args->size);
		return ERR_PTR(-EINVAL);
	}

	align = (args->type == AMDXDNA_BO_DEV_HEAP) ? xdna->dev_info->dev_mem_size : 0;
	if (align > size)
		size = roundup_pow_of_two(align);

	kva = dma_alloc_coherent(cma_dev, size, &dma_addr, GFP_KERNEL);
	if (!kva) {
		XDNA_DBG(xdna, "Coherent CMA alloc failed on %s: size 0x%zx",
			 dev_name(cma_dev), size);
		return ERR_PTR(-ENOMEM);
	}
	/* dma_alloc_coherent() returns zeroed memory. */

	abo = amdxdna_gem_create_obj(dev, size);
	if (IS_ERR(abo)) {
		ret = PTR_ERR(abo);
		goto free_coherent;
	}

	abo->coherent = true;
	abo->mem.kva = kva;
	abo->cma_dma_addr = dma_addr;
	/* Addressed by its own dma_addr; amdxdna_dma_map_bo() no-ops on a set addr. */
	abo->mem.dma_addr = dma_addr;
	abo->private_buffer = true;
	abo->type = AMDXDNA_BO_SHARE;

	to_gobj(abo)->funcs = &amdxdna_gem_coherent_obj_funcs;
	drm_gem_private_object_init(dev, to_gobj(abo), size);

	ret = drm_gem_create_mmap_offset(to_gobj(abo));
	if (ret) {
		XDNA_ERR(xdna, "Create mmap offset failed, ret %d", ret);
		drm_gem_object_release(to_gobj(abo));
		goto destroy_obj;
	}

	return abo;

destroy_obj:
	amdxdna_gem_destroy_obj(abo);
free_coherent:
	dma_free_coherent(cma_dev, size, kva, dma_addr);
	return ERR_PTR(ret);
}

struct amdxdna_gem_obj *
amdxdna_get_cma_buf(struct drm_device *dev, struct amdxdna_drm_create_bo *args)
{
	struct amdxdna_dev *xdna = to_xdna_dev(dev);
	struct device *cma_dev = xdna->ddev.dev;
	size_t size = PAGE_ALIGN(args->size);
	struct amdxdna_gem_obj *abo;
	unsigned long i, npages;
	dma_addr_t dma_addr;
	struct page *page;
	u64 align;
	int ret;

	/* Debug: route to the non-cacheable coherent backing when requested. */
	if (xdna->cma_coherent)
		return amdxdna_get_coherent_buf(dev, args);

	if (!size) {
		XDNA_ERR(xdna, "Invalid BO size 0x%llx", args->size);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * A dev-heap BO must be self-aligned to its size.  dma_alloc_pages()
	 * aligns to get_order(size) (capped by CONFIG_CMA_ALIGNMENT), so grow
	 * the request until natural alignment satisfies @align; alignments
	 * beyond CONFIG_CMA_ALIGNMENT need that Kconfig raised.
	 */
	align = (args->type == AMDXDNA_BO_DEV_HEAP) ? xdna->dev_info->dev_mem_size : 0;
	if (align > size)
		size = roundup_pow_of_two(align);

	page = dma_alloc_pages(cma_dev, size, &dma_addr, DMA_BIDIRECTIONAL, GFP_KERNEL);
	if (!page) {
		XDNA_DBG(xdna, "CMA alloc failed on %s: size 0x%zx",
			 dev_name(cma_dev), size);
		return ERR_PTR(-ENOMEM);
	}
	/* dma_alloc_pages() does not zero; clear before exposing to userspace. */
	memset(page_address(page), 0, size);

	abo = amdxdna_gem_create_obj(dev, size);
	if (IS_ERR(abo)) {
		ret = PTR_ERR(abo);
		goto free_pages;
	}

	npages = size >> PAGE_SHIFT;
	abo->mem.pages = kvmalloc_objs(*abo->mem.pages, npages);
	if (!abo->mem.pages) {
		ret = -ENOMEM;
		goto destroy_obj;
	}
	for (i = 0; i < npages; i++)
		abo->mem.pages[i] = pfn_to_page(page_to_pfn(page) + i);
	abo->mem.nr_pages = npages;
	abo->cma_dma_addr = dma_addr;
	abo->private_buffer = true;
	abo->type = AMDXDNA_BO_SHARE;

	to_gobj(abo)->funcs = &amdxdna_gem_cma_obj_funcs;
	drm_gem_private_object_init(dev, to_gobj(abo), size);

	/*
	 * A private GEM object gets no fake mmap offset for free, so create one
	 * here; GET_BO_INFO returns it and userspace mmap()s the BO handle at
	 * that offset (drm_gem_mmap() -> amdxdna_gem_cma_obj_mmap()).
	 */
	ret = drm_gem_create_mmap_offset(to_gobj(abo));
	if (ret) {
		XDNA_ERR(xdna, "Create mmap offset failed, ret %d", ret);
		drm_gem_object_release(to_gobj(abo));
		goto free_bo_pages;
	}

	return abo;

free_bo_pages:
	kvfree(abo->mem.pages);
destroy_obj:
	amdxdna_gem_destroy_obj(abo);
free_pages:
	dma_free_pages(cma_dev, size, page, dma_addr, DMA_BIDIRECTIONAL);
	return ERR_PTR(ret);
}
