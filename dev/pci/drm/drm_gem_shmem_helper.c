// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 Noralf Trønnes
 */

#include <linux/dma-buf.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/iosys-map.h>

#ifdef CONFIG_X86
#include <asm/set_memory.h>
#endif

#include <drm/drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>

#include <uvm/uvm.h>

MODULE_IMPORT_NS(DMA_BUF);

/**
 * DOC: overview
 *
 * This library provides helpers for GEM objects backed by shmem buffers
 * allocated using anonymous pageable memory.
 *
 * Functions that operate on the GEM object receive struct &drm_gem_shmem_object.
 * For GEM callback helpers in struct &drm_gem_object functions, see likewise
 * named functions with an _object_ infix (e.g., drm_gem_shmem_object_vmap() wraps
 * drm_gem_shmem_vmap()). These helpers perform the necessary type conversion.
 */

static const struct drm_gem_object_funcs drm_gem_shmem_funcs = {
	.free = drm_gem_shmem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
#ifdef __OpenBSD__
	.mmap = drm_gem_shmem_object_mmap,
#endif
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static struct drm_gem_shmem_object *
__drm_gem_shmem_create(struct drm_device *dev, size_t size, bool private)
{
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	int ret = 0;

	size = round_up(size, 0x4000);

	if (dev->driver->gem_create_object) {
		obj = dev->driver->gem_create_object(dev, size);
		if (IS_ERR(obj))
			return ERR_CAST(obj);
		shmem = to_drm_gem_shmem_obj(obj);
	} else {
		shmem = kzalloc(sizeof(*shmem), GFP_KERNEL);
		if (!shmem)
			return ERR_PTR(-ENOMEM);
		obj = &shmem->base;
	}

	if (!obj->funcs)
		obj->funcs = &drm_gem_shmem_funcs;

	if (private) {
		drm_gem_private_object_init(dev, obj, size);
		shmem->map_wc = false; /* dma-buf mappings use always writecombine */
	} else {
		ret = drm_gem_object_init(dev, obj, size);
	}
	if (ret) {
		drm_gem_private_object_fini(obj);
		goto err_free;
	}
	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto err_release;
	INIT_LIST_HEAD(&shmem->madv_list);

	if (!private) {
		/*
		 * Our buffers are kept pinned, so allocating them
		 * from the MOVABLE zone is a really bad idea, and
		 * conflicts with CMA. See comments above new_inode()
		 * why this is required _and_ expected if you're
		 * going to pin these pages.
		 */
		// mapping_set_gfp_mask(obj->filp->f_mapping, GFP_HIGHUSER |
				     // __GFP_RETRY_MAYFAIL | __GFP_NOWARN);
	}

	return shmem;

err_release:
	drm_gem_object_release(obj);
err_free:
	kfree(obj);

	return ERR_PTR(ret);
}
/**
 * drm_gem_shmem_create - Allocate an object with the given size
 * @dev: DRM device
 * @size: Size of the object to allocate
 *
 * This function creates a shmem GEM object.
 *
 * Returns:
 * A struct drm_gem_shmem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *dev, size_t size)
{
	return __drm_gem_shmem_create(dev, size, false);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_create);

/**
 * drm_gem_shmem_free - Free resources associated with a shmem GEM object
 * @shmem: shmem GEM object to free
 *
 * This function cleans up the GEM object state and frees the memory used to
 * store the object itself.
 */
void drm_gem_shmem_free(struct drm_gem_shmem_object *shmem)
{
	STUB();
	return;
	
	struct drm_gem_object *obj = &shmem->base;

	if (obj->import_attach) {
		drm_prime_gem_destroy(obj, shmem->sgt);
	} else {
		dma_resv_lock(shmem->base.resv, NULL);

		drm_WARN_ON(obj->dev, shmem->vmap_use_count);

		if (shmem->sgt) {
			// dma_unmap_sgtable(obj->dev->dev, shmem->sgt,
					  // DMA_BIDIRECTIONAL, 0);
			sg_free_table(shmem->sgt);
			kfree(shmem->sgt);
		}
		if (shmem->pages)
			drm_gem_shmem_put_pages(shmem);

		drm_WARN_ON(obj->dev, shmem->pages_use_count);

		dma_resv_unlock(shmem->base.resv);
	}

	drm_gem_object_release(obj);
	kfree(shmem);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_free);

static inline void
uvm_pageinsert(struct vm_page *pg)
{
	struct vm_page	*dupe;

	KASSERT(UVM_OBJ_IS_DUMMY(pg->uobject) ||
	    rw_write_held(pg->uobject->vmobjlock));
	KASSERT((pg->pg_flags & PG_TABLED) == 0);

	dupe = RBT_INSERT(uvm_objtree, &pg->uobject->memt, pg);
	/* not allowed to insert over another page */
	KASSERT(dupe == NULL);
	atomic_setbits_int(&pg->pg_flags, PG_TABLED);
	pg->uobject->uo_npages++;
}

void drm_unref(struct uvm_object *);
void drm_ref(struct uvm_object *);
boolean_t drm_flush(struct uvm_object *, voff_t, voff_t, int);

static int
drm_get(struct uvm_object *uobj, voff_t offset, struct vm_page **pps,
    int *npagesp, int centeridx, vm_prot_t access_type, int advice, int flags)
{
	voff_t current_offset;
	vm_page_t ptmp;
	int lcv, gotpages, maxpages, swslot, rv, pageidx;
	boolean_t done;

	KASSERT(rw_write_held(uobj->vmobjlock));

	/*
 	 * get number of pages
 	 */
	maxpages = *npagesp;

	if (flags & PGO_LOCKED) {
		/*
 		 * step 1a: get pages that are already resident.   only do
		 * this if the data structures are locked (i.e. the first
		 * time through).
 		 */

		done = TRUE;	/* be optimistic */
		gotpages = 0;	/* # of pages we got so far */

		for (lcv = 0, current_offset = offset ; lcv < maxpages ;
		    lcv++, current_offset += PAGE_SIZE) {
			/* do we care about this page?  if not, skip it */
			if (pps[lcv] == PGO_DONTCARE)
				continue;

			ptmp = uvm_pagelookup(uobj, current_offset);

			/*
 			 * if page is new, attempt to allocate the page,
			 * zero-fill'd.
 			 */
			if (ptmp == NULL /*&& uao_find_swslot(uobj,
			    current_offset >> PAGE_SHIFT) == 0*/) {
				ptmp = uvm_pagealloc(uobj, current_offset,
				    NULL, UVM_PGA_ZERO);
				if (ptmp) {
					/* new page */
					atomic_clearbits_int(&ptmp->pg_flags,
					    PG_BUSY|PG_FAKE);
					atomic_setbits_int(&ptmp->pg_flags,
					    PQ_AOBJ);
					UVM_PAGE_OWN(ptmp, NULL);
				}
			}

			/*
			 * to be useful must get a non-busy page
			 */
			if (ptmp == NULL ||
			    (ptmp->pg_flags & PG_BUSY) != 0) {
				if (lcv == centeridx ||
				    (flags & PGO_ALLPAGES) != 0)
					/* need to do a wait or I/O! */
					done = FALSE;	
				continue;
			}

			/*
			 * useful page: plug it in our result array
			 */
			atomic_setbits_int(&ptmp->pg_flags, PG_BUSY);
			UVM_PAGE_OWN(ptmp, "uao_get1");
			pps[lcv] = ptmp;
			gotpages++;

		}

		/*
 		 * step 1b: now we've either done everything needed or we
		 * to unlock and do some waiting or I/O.
 		 */
		*npagesp = gotpages;
		if (done)
			/* bingo! */
			return VM_PAGER_OK;	
		else
			/* EEK!   Need to unlock and I/O */
			return VM_PAGER_UNLOCK;
	}

	/*
 	 * step 2: get non-resident or busy pages.
 	 * data structures are unlocked.
 	 */
	for (lcv = 0, current_offset = offset ; lcv < maxpages ;
	    lcv++, current_offset += PAGE_SIZE) {
		/*
		 * - skip over pages we've already gotten or don't want
		 * - skip over pages we don't _have_ to get
		 */
		if (pps[lcv] != NULL ||
		    (lcv != centeridx && (flags & PGO_ALLPAGES) == 0))
			continue;

		pageidx = current_offset >> PAGE_SHIFT;

		/*
 		 * we have yet to locate the current page (pps[lcv]).   we
		 * first look for a page that is already at the current offset.
		 * if we find a page, we check to see if it is busy or
		 * released.  if that is the case, then we sleep on the page
		 * until it is no longer busy or released and repeat the lookup.
		 * if the page we found is neither busy nor released, then we
		 * busy it (so we own it) and plug it into pps[lcv].   this
		 * 'break's the following while loop and indicates we are
		 * ready to move on to the next page in the "lcv" loop above.
 		 *
 		 * if we exit the while loop with pps[lcv] still set to NULL,
		 * then it means that we allocated a new busy/fake/clean page
		 * ptmp in the object and we need to do I/O to fill in the data.
 		 */

		/* top of "pps" while loop */
		while (pps[lcv] == NULL) {
			/* look for a resident page */
			ptmp = uvm_pagelookup(uobj, current_offset);

			/* not resident?   allocate one now (if we can) */
			if (ptmp == NULL) {

				ptmp = uvm_pagealloc(uobj, current_offset,
				    NULL, 0);

				/* out of RAM? */
				if (ptmp == NULL) {
					rw_exit(uobj->vmobjlock);
					uvm_wait("uao_getpage");
					rw_enter(uobj->vmobjlock, RW_WRITE);
					/* goto top of pps while loop */
					continue;
				}

				/*
				 * safe with PQ's unlocked: because we just
				 * alloc'd the page
				 */
				atomic_setbits_int(&ptmp->pg_flags, PQ_AOBJ);

				/* 
				 * got new page ready for I/O.  break pps while
				 * loop.  pps[lcv] is still NULL.
				 */
				break;
			}

			/* page is there, see if we need to wait on it */
			if ((ptmp->pg_flags & PG_BUSY) != 0) {
				uvm_pagewait(ptmp, uobj->vmobjlock, "uao_get");
				rw_enter(uobj->vmobjlock, RW_WRITE);
				continue;	/* goto top of pps while loop */
			}

			/*
 			 * if we get here then the page is resident and
			 * unbusy.  we busy it now (so we own it).
 			 */
			/* we own it, caller must un-busy */
			atomic_setbits_int(&ptmp->pg_flags, PG_BUSY);
			UVM_PAGE_OWN(ptmp, "uao_get2");
			pps[lcv] = ptmp;
		}

		/*
 		 * if we own the valid page at the correct offset, pps[lcv] will
 		 * point to it.   nothing more to do except go to the next page.
 		 */
		if (pps[lcv])
			continue;			/* next lcv */

		uvm_pagezero(ptmp);
#ifdef notyet
		/*
 		 * we have a "fake/busy/clean" page that we just allocated.  
 		 * do the needed "i/o", either reading from swap or zeroing.
 		 */
		swslot = uao_find_swslot(uobj, pageidx);

		/* just zero the page if there's nothing in swap.  */
		if (swslot == 0) {
			/* page hasn't existed before, just zero it. */
			uvm_pagezero(ptmp);
		} else {
			/*
			 * page in the swapped-out page.
			 * unlock object for i/o, relock when done.
			 */

			rw_exit(uobj->vmobjlock);
			rv = uvm_swap_get(ptmp, swslot, PGO_SYNCIO);
			rw_enter(uobj->vmobjlock, RW_WRITE);

			/*
			 * I/O done.  check for errors.
			 */
			if (rv != VM_PAGER_OK) {
				/*
				 * remove the swap slot from the aobj
				 * and mark the aobj as having no real slot.
				 * don't free the swap slot, thus preventing
				 * it from being used again.
				 */
				swslot = uao_set_swslot(&aobj->u_obj, pageidx,
							SWSLOT_BAD);
				uvm_swap_markbad(swslot, 1);

				if (ptmp->pg_flags & PG_WANTED)
					wakeup(ptmp);
				atomic_clearbits_int(&ptmp->pg_flags,
				    PG_WANTED|PG_BUSY);
				UVM_PAGE_OWN(ptmp, NULL);
				uvm_lock_pageq();
				uvm_pagefree(ptmp);
				uvm_unlock_pageq();
				rw_exit(uobj->vmobjlock);

				return rv;
			}
		}
#endif

		/*
 		 * we got the page!   clear the fake flag (indicates valid
		 * data now in page) and plug into our result array.   note
		 * that page is still busy.
 		 *
 		 * it is the callers job to:
 		 * => check if the page is released
 		 * => unbusy the page
 		 * => activate the page
 		 */
		atomic_clearbits_int(&ptmp->pg_flags, PG_FAKE);
		pmap_clear_modify(ptmp);		/* ... and clean */
		pps[lcv] = ptmp;

	}	/* lcv loop */

	rw_exit(uobj->vmobjlock);
	return VM_PAGER_OK;
}

struct uvm_pagerops drm_shmem_pager = {
	.pgo_reference = drm_ref,
	.pgo_detach = drm_unref,
	.pgo_flush = drm_flush,
	.pgo_get = drm_get,
};

static int drm_gem_shmem_get_pages(struct drm_gem_shmem_object *shmem)
{
    struct drm_gem_object *obj = &shmem->base;
    struct vm_page **pages;
    long npages = obj->size >> 14;
    int ret = 0;

    dma_resv_assert_held(shmem->base.resv);

    if (shmem->pages_use_count++ > 0)
        return 0;

    pages = mallocarray((npages+1)*4, sizeof(struct vm_page *), M_DRM, M_WAITOK | M_ZERO);
    if (pages == NULL) {
        ret = -ENOMEM;
        goto out;
    }

    struct pglist plist;
    struct vm_page *page;
    struct scatterlist *sg;
    struct sg_table *st = malloc(sizeof(struct sg_table), M_DRM, M_WAITOK | M_ZERO);

    if (!st) {
        ret = -ENOMEM;
        goto free_pages;
    }

    if (sg_alloc_table(st, npages, M_WAITOK)) {
        ret = -ENOMEM;
        goto free_st;
    }

    TAILQ_INIT(&plist);

    ret = uvm_pglistalloc(obj->size, (paddr_t)0, (paddr_t)(-1), (1 << 14), 0, &plist, 1, UVM_PLA_WAITOK | UVM_PLA_ZERO);
    if (ret) {
        sg_free_table(st);
        ret = -ENOMEM;
        goto free_st;
    }

    sg = st->sgl;
    st->nents = 0;
    long i = 0;
    uvm_obj_init(&obj->uobj, &drm_shmem_pager, 1);
    struct uvm_object *uobj = &obj->uobj;

    rw_enter(uobj->vmobjlock, RW_WRITE | RW_DUPOK);
    while (i < npages) {
        int j;
        for (j = 0; j < 4; j++) {
            page = TAILQ_FIRST(&plist);
            if (page == NULL) {
                ret = -ENOMEM;
                goto fail_unwire;
            }
            TAILQ_REMOVE(&plist, page, pageq);
            page->uobject = uobj;
            page->offset = (i * 4 + j)*PAGE_SIZE;
		        if (uvm_pagelookup(uobj, page->offset) == NULL) {
	            uvm_pageinsert(page);
		        }
						pages[i * 4 + j] = page;
        }

        sg_set_page(sg, pages[i * 4], 16 * 1024, 0);
        sg = sg_next(sg);
        st->nents++;
        i++;
    }
    rw_exit(uobj->vmobjlock);

    if (sg)
        sg_mark_end(sg);

    shmem->sgt = st;
    shmem->pages = pages;

    return 0;

fail_unwire:
    uvm_pglistfree(&plist);
free_st:
    free(st, sizeof(struct sg_table), M_DRM);
free_pages:
    free(pages, 4 * (npages+1) * sizeof(struct vm_page *), M_DRM);
out:
    shmem->pages_use_count = 0;
    return ret;
}

/*
 * drm_gem_shmem_put_pages - Decrease use count on the backing pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function decreases the use count and puts the backing pages when use drops to zero.
 */
void drm_gem_shmem_put_pages(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	dma_resv_assert_held(shmem->base.resv);

	if (drm_WARN_ON_ONCE(obj->dev, !shmem->pages_use_count))
		return;

	if (--shmem->pages_use_count > 0)
		return;

#ifdef CONFIG_X86
	if (shmem->map_wc)
		set_pages_array_wb(shmem->pages, obj->size >> PAGE_SHIFT);
#endif

	drm_gem_put_pages(obj, shmem->pages,
			  shmem->pages_mark_dirty_on_put,
			  shmem->pages_mark_accessed_on_put);
	shmem->pages = NULL;
}
EXPORT_SYMBOL(drm_gem_shmem_put_pages);

static int drm_gem_shmem_pin_locked(struct drm_gem_shmem_object *shmem)
{
	int ret;

	dma_resv_assert_held(shmem->base.resv);

	ret = drm_gem_shmem_get_pages(shmem);

	return ret;
}

static void drm_gem_shmem_unpin_locked(struct drm_gem_shmem_object *shmem)
{
	dma_resv_assert_held(shmem->base.resv);

	drm_gem_shmem_put_pages(shmem);
}

/**
 * drm_gem_shmem_pin - Pin backing pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function makes sure the backing pages are pinned in memory while the
 * buffer is exported.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_pin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	drm_WARN_ON(obj->dev, obj->import_attach);

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return ret;
	ret = drm_gem_shmem_pin_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	return ret;
}
EXPORT_SYMBOL(drm_gem_shmem_pin);

/**
 * drm_gem_shmem_unpin - Unpin backing pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function removes the requirement that the backing pages are pinned in
 * memory.
 */
void drm_gem_shmem_unpin(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, obj->import_attach);

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_unpin_locked(shmem);
	dma_resv_unlock(shmem->base.resv);
}
EXPORT_SYMBOL(drm_gem_shmem_unpin);

int dma_buf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
    struct pglist plist;
    vaddr_t vaddr;
    struct vm_page *page;
    size_t size = dmabuf->size;  // Assume dma-buf has a 'size' field
    size_t npages = size >> PAGE_SHIFT;  // Number of pages based on the buffer size
    int ret = 0;

    /* Initialize the page list */
    TAILQ_INIT(&plist);

    /* Simulate getting the pages backing the dma-buf */
    /* This step is where you would typically fill the plist with pages backing the DMA buffer */
    ret = uvm_pglistalloc(size, 0, -1, PAGE_SIZE, 0, &plist, npages, UVM_PLA_WAITOK);
    if (ret) {
        return -ENOMEM;
    }

	vaddr = (vaddr_t)km_alloc(round_page(size), &kv_any, &kp_none, &kd_waitok);

    for (size_t i = 0; i < npages; i++) {
        page = TAILQ_FIRST(&plist);
        if (!page) {
            ret = -ENOMEM;
            goto err_unmap;
        }

        paddr_t paddr = VM_PAGE_TO_PHYS(page);
		pmap_kenter_pa(vaddr + (i * PAGE_SIZE), paddr, PROT_READ | PROT_WRITE);  // Map page into the virtual address space

        TAILQ_REMOVE(&plist, page, pageq);
    }

    /* Update the pmap to finalize the mappings */
    pmap_update(pmap_kernel());

    /* Set the virtual address in the iosys_map */
    iosys_map_set_vaddr(map, (void*)vaddr);

    return 0;

err_unmap:
    /* In case of failure, unmap the allocated virtual address space */
    pmap_kremove(vaddr, round_page(size));
    pmap_update(pmap_kernel());
    uvm_pglistfree(&plist);  // Free the allocated pages
    return ret;
}

int dma_buf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
    vaddr_t vaddr = (vaddr_t)map->vaddr;
    size_t size = dmabuf->size;          

    if (!vaddr)
        return -EINVAL;

    /* Remove the page mappings */
    pmap_kremove(vaddr, round_page(size));
    pmap_update(pmap_kernel());

    /* Reset the iosys_map structure */
    iosys_map_clear(map);

    return 0;
}

/*
 * drm_gem_shmem_vmap - Create a virtual mapping for a shmem GEM object
 * @shmem: shmem GEM object
 * @map: Returns the kernel virtual address of the SHMEM GEM object's backing
 *       store.
 *
 * This function makes sure that a contiguous kernel virtual address mapping
 * exists for the buffer backing the shmem GEM object. It hides the differences
 * between dma-buf imported and natively allocated objects.
 *
 * Acquired mappings should be cleaned up by calling drm_gem_shmem_vunmap().
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_vmap(struct drm_gem_shmem_object *shmem,
		       struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret = 0;
	dma_resv_assert_held(obj->resv);

	if (obj->import_attach) {
		ret = dma_buf_vmap(obj->dma_buf, map);
		if (!ret) {
			dma_buf_vunmap(obj->dma_buf, map);
			return -EIO;
		}
	} else {
		pgprot_t prot = PAGE_KERNEL;

		dma_resv_assert_held(shmem->base.resv);

		if (shmem->vmap_use_count++ > 0) {
			iosys_map_set_vaddr(map, shmem->vaddr);
			return 0;
		}

		ret = drm_gem_shmem_get_pages(shmem);
		if (ret)
			goto err_zero_use;

		if (shmem->map_wc)
			prot = pgprot_writecombine(prot);
		shmem->vaddr = vmap(shmem->pages, obj->size >> PAGE_SHIFT, 0, prot);
		if (!shmem->vaddr)
			ret = -ENOMEM;
		else
			iosys_map_set_vaddr(map, shmem->vaddr);
	}

	if (ret) {
		drm_dbg_kms(obj->dev, "Failed to vmap pages, error %d\n", ret);
		goto err_put_pages;
	}

	return 0;

err_put_pages:
	if (!obj->import_attach)
		drm_gem_shmem_put_pages(shmem);
err_zero_use:
	shmem->vmap_use_count = 0;
	return ret;
}
EXPORT_SYMBOL(drm_gem_shmem_vmap);

/*
 * drm_gem_shmem_vunmap - Unmap a virtual mapping for a shmem GEM object
 * @shmem: shmem GEM object
 * @map: Kernel virtual address where the SHMEM GEM object was mapped
 *
 * This function cleans up a kernel virtual address mapping acquired by
 * drm_gem_shmem_vmap(). The mapping is only removed when the use count drops to
 * zero.
 *
 * This function hides the differences between dma-buf imported and natively
 * allocated objects.
 */
void drm_gem_shmem_vunmap(struct drm_gem_shmem_object *shmem,
			  struct iosys_map *map)
{
	struct drm_gem_object *obj = &shmem->base;

	dma_resv_assert_held(obj->resv);

	if (obj->import_attach) {
		dma_buf_vunmap(obj->import_attach->dmabuf, map);
	} else {
		dma_resv_assert_held(shmem->base.resv);

		if (drm_WARN_ON_ONCE(obj->dev, !shmem->vmap_use_count))
			return;

		if (--shmem->vmap_use_count > 0)
			return;

		vunmap(shmem->vaddr, obj->size);
		drm_gem_shmem_put_pages(shmem);
	}

	shmem->vaddr = NULL;
}
EXPORT_SYMBOL(drm_gem_shmem_vunmap);

static int
drm_gem_shmem_create_with_handle(struct drm_file *file_priv,
				 struct drm_device *dev, size_t size,
				 uint32_t *handle)
{
	struct drm_gem_shmem_object *shmem;
	int ret;

	shmem = drm_gem_shmem_create(dev, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, &shmem->base, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put(&shmem->base);

	return ret;
}

/* Update madvise status, returns true if not purged, else
 * false or -errno.
 */
int drm_gem_shmem_madvise(struct drm_gem_shmem_object *shmem, int madv)
{
	dma_resv_assert_held(shmem->base.resv);

	if (shmem->madv >= 0)
		shmem->madv = madv;

	madv = shmem->madv;

	return (madv >= 0);
}
EXPORT_SYMBOL(drm_gem_shmem_madvise);

void drm_gem_shmem_purge(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	struct drm_device *dev = obj->dev;

	dma_resv_assert_held(shmem->base.resv);

	drm_WARN_ON(obj->dev, !drm_gem_shmem_is_purgeable(shmem));

	// dma_unmap_sgtable(dev->dev, shmem->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(shmem->sgt);
	kfree(shmem->sgt);
	shmem->sgt = NULL;

	drm_gem_shmem_put_pages(shmem);

	shmem->madv = -1;

	// drm_vma_node_unmap(&obj->vma_node, dev->anon_inode->i_mapping);
	drm_gem_free_mmap_offset(obj);

	/* Our goal here is to return as much of the memory as
	 * is possible back to the system as we are called from OOM.
	 * To do this we must instruct the shmfs to drop all of its
	 * backing pages, *now*.
	 */
	// shmem_truncate_range(file_inode(obj->filp), 0, (loff_t)-1);

	// invalidate_mapping_pages(file_inode(obj->filp)->i_mapping, 0, (loff_t)-1);
}
EXPORT_SYMBOL(drm_gem_shmem_purge);

/**
 * drm_gem_shmem_dumb_create - Create a dumb shmem buffer object
 * @file: DRM file structure to create the dumb buffer for
 * @dev: DRM device
 * @args: IOCTL data
 *
 * This function computes the pitch of the dumb buffer and rounds it up to an
 * integer number of bytes per pixel. Drivers for hardware that doesn't have
 * any additional restrictions on the pitch can directly use this function as
 * their &drm_driver.dumb_create callback.
 *
 * For hardware with additional restrictions, drivers can adjust the fields
 * set up by userspace before calling into this function.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_dumb_create(struct drm_file *file, struct drm_device *dev,
			      struct drm_mode_create_dumb *args)
{
	u32 min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);

	if (!args->pitch || !args->size) {
		args->pitch = min_pitch;
		args->size = PAGE_ALIGN(args->pitch * args->height);
	} else {
		/* ensure sane minimum values */
		if (args->pitch < min_pitch)
			args->pitch = min_pitch;
		if (args->size < args->pitch * args->height)
			args->size = PAGE_ALIGN(args->pitch * args->height);
	}

	return drm_gem_shmem_create_with_handle(file, dev, args->size, &args->handle);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_dumb_create);

#ifdef __linux__
vm_fault_t drm_gem_shmem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);
	loff_t num_pages = obj->size >> PAGE_SHIFT;
	vm_fault_t ret;
	struct page *page;
	pgoff_t page_offset;

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;

	dma_resv_lock(shmem->base.resv, NULL);

	if (page_offset >= num_pages ||
	    drm_WARN_ON_ONCE(obj->dev, !shmem->pages) ||
	    shmem->madv < 0) {
		ret = VM_FAULT_SIGBUS;
	} else {
		page = shmem->pages[page_offset];

		ret = vmf_insert_pfn(vma, vmf->address, page_to_pfn(page));
	}

	dma_resv_unlock(shmem->base.resv);

	return ret;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_fault);
#else
vm_fault_t drm_gem_shmem_fault(struct uvm_faultinfo *vmf)
{
	vm_fault_t ret = 0;
	return ret;	
}
#endif

#ifdef __linux___
void drm_gem_shmem_vm_open(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	drm_WARN_ON(obj->dev, obj->import_attach);

	dma_resv_lock(shmem->base.resv, NULL);

	/*
	 * We should have already pinned the pages when the buffer was first
	 * mmap'd, vm_open() just grabs an additional reference for the new
	 * mm the vma is getting copied into (ie. on fork()).
	 */
	if (!drm_WARN_ON_ONCE(obj->dev, !shmem->pages_use_count))
		shmem->pages_use_count++;

	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_open(vma);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_vm_open);

void drm_gem_shmem_vm_close(struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(obj);

	dma_resv_lock(shmem->base.resv, NULL);
	drm_gem_shmem_put_pages(shmem);
	dma_resv_unlock(shmem->base.resv);

	drm_gem_vm_close(vma);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_vm_close);
#endif

#ifdef __linux__
const struct vm_operations_struct drm_gem_shmem_vm_ops = {
	.fault = drm_gem_shmem_fault,
	.open = drm_gem_shmem_vm_open,
	.close = drm_gem_shmem_vm_close,
};
EXPORT_SYMBOL_GPL(drm_gem_shmem_vm_ops);
#else
const struct uvm_pagerops drm_gem_shmem_vm_ops = {
	
};
#endif

#ifdef __linux__
/**
 * drm_gem_shmem_mmap - Memory-map a shmem GEM object
 * @shmem: shmem GEM object
 * @vma: VMA for the area to be mapped
 *
 * This function implements an augmented version of the GEM DRM file mmap
 * operation for shmem objects.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_mmap(struct drm_gem_shmem_object *shmem, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;

	if (obj->import_attach) {
		/* Reset both vm_ops and vm_private_data, so we don't end up with
		 * vm_ops pointing to our implementation if the dma-buf backend
		 * doesn't set those fields.
		 */
		vma->vm_private_data = NULL;
		vma->vm_ops = NULL;

		ret = dma_buf_mmap(obj->dma_buf, vma, 0);

		/* Drop the reference drm_gem_mmap_obj() acquired.*/
		if (!ret)
			drm_gem_object_put(obj);

		return ret;
	}

	dma_resv_lock(shmem->base.resv, NULL);
	ret = drm_gem_shmem_get_pages(shmem);
	dma_resv_unlock(shmem->base.resv);

	if (ret)
		return ret;

	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	if (shmem->map_wc)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return 0;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_mmap);
#else

int
drm_gem_shmem_object_mmap(struct drm_gem_object *obj, vm_prot_t accessprot,
                          voff_t off, vsize_t size)
{
	struct drm_gem_shmem_object *shmem = NULL;

	shmem = container_of(obj, struct drm_gem_shmem_object, base);
	if (!shmem)
		return -1;

	int ret;	
	dma_resv_lock(shmem->base.resv, NULL);
	ret = drm_gem_shmem_get_pages(shmem);
	dma_resv_unlock(shmem->base.resv);

	return ret;
}

struct uvm_object *
drm_gem_shmem_mmap(struct file *flip, vm_prot_t accessprot,
                  voff_t off, vsize_t size)
{
	struct drm_vma_offset_node *node;
	struct drm_file *priv = (void *)flip;
	struct drm_device *dev = priv->minor->dev;
	struct drm_gem_object *obj = NULL;
	struct drm_gem_shmem_object *shmem = NULL;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  off >> PAGE_SHIFT,
						  atop(round_page(size)));
	if (likely(node)) {
		obj = container_of(node, struct drm_gem_object, vma_node);
		if (!kref_get_unless_zero(&obj->refcount))
			obj = NULL;
	}
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!obj)
		return NULL;

	if (!drm_vma_node_is_allowed(node, priv)) {
		drm_gem_object_put(obj);
		return NULL;
	}

	if (drm_gem_shmem_object_mmap(obj, accessprot, off, size))
		return NULL;

	return &obj->uobj;
}
#endif

/**
 * drm_gem_shmem_print_info() - Print &drm_gem_shmem_object info for debugfs
 * @shmem: shmem GEM object
 * @p: DRM printer
 * @indent: Tab indentation level
 */
void drm_gem_shmem_print_info(const struct drm_gem_shmem_object *shmem,
			      struct drm_printer *p, unsigned int indent)
{
	if (shmem->base.import_attach)
		return;

	drm_printf_indent(p, indent, "pages_use_count=%u\n", shmem->pages_use_count);
	drm_printf_indent(p, indent, "vmap_use_count=%u\n", shmem->vmap_use_count);
	drm_printf_indent(p, indent, "vaddr=%p\n", shmem->vaddr);
}
EXPORT_SYMBOL(drm_gem_shmem_print_info);

/**
 * drm_gem_shmem_get_sg_table - Provide a scatter/gather table of pinned
 *                              pages for a shmem GEM object
 * @shmem: shmem GEM object
 *
 * This function exports a scatter/gather table suitable for PRIME usage by
 * calling the standard DMA mapping API.
 *
 * Drivers who need to acquire an scatter/gather table for objects need to call
 * drm_gem_shmem_get_pages_sgt() instead.
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or error pointer on failure.
 */
struct sg_table *drm_gem_shmem_get_sg_table(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;

	drm_WARN_ON(obj->dev, obj->import_attach);

	return drm_prime_pages_to_sg(obj->dev, shmem->pages, obj->size >> 14);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_get_sg_table);

static struct sg_table *drm_gem_shmem_get_pages_sgt_locked(struct drm_gem_shmem_object *shmem)
{
	struct drm_gem_object *obj = &shmem->base;
	int ret;
	struct sg_table *sgt;

	if (shmem->sgt)
		return shmem->sgt;

	drm_WARN_ON(obj->dev, obj->import_attach);

	ret = drm_gem_shmem_get_pages(shmem);
	if (ret)
		return ERR_PTR(ret);
	
	return shmem->sgt;

#ifdef notyet
	sgt = drm_gem_shmem_get_sg_table(shmem);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_put_pages;
	}
#ifdef __linux__
	/* Map the pages for use by the h/w. */
	ret = dma_map_sgtable(obj->dev->dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err_free_sgt;
#endif

	shmem->sgt = sgt;

	return shmem->sgt;

#ifdef __linux__
err_free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
#endif
err_put_pages:
	drm_gem_shmem_put_pages(shmem);
	return ERR_PTR(ret);
#endif
}

/**
 * drm_gem_shmem_get_pages_sgt - Pin pages, dma map them, and return a
 *				 scatter/gather table for a shmem GEM object.
 * @shmem: shmem GEM object
 *
 * This function returns a scatter/gather table suitable for driver usage. If
 * the sg table doesn't exist, the pages are pinned, dma-mapped, and a sg
 * table created.
 *
 * This is the main function for drivers to get at backing storage, and it hides
 * and difference between dma-buf imported and natively allocated objects.
 * drm_gem_shmem_get_sg_table() should not be directly called by drivers.
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or errno on failure.
 */
struct sg_table *drm_gem_shmem_get_pages_sgt(struct drm_gem_shmem_object *shmem)
{
	int ret;
	struct sg_table *sgt;

	ret = dma_resv_lock_interruptible(shmem->base.resv, NULL);
	if (ret)
		return ERR_PTR(ret);
	sgt = drm_gem_shmem_get_pages_sgt_locked(shmem);
	dma_resv_unlock(shmem->base.resv);

	return sgt;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_get_pages_sgt);

/**
 * drm_gem_shmem_prime_import_sg_table - Produce a shmem GEM object from
 *                 another driver's scatter/gather table of pinned pages
 * @dev: Device to import into
 * @attach: DMA-BUF attachment
 * @sgt: Scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table exported via DMA-BUF by
 * another driver. Drivers that use the shmem helpers should set this as their
 * &drm_driver.gem_prime_import_sg_table callback.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object *
drm_gem_shmem_prime_import_sg_table(struct drm_device *dev,
				    struct dma_buf_attachment *attach,
				    struct sg_table *sgt)
{
	size_t size = round_up(attach->dmabuf->size, 0x4000);
	struct drm_gem_shmem_object *shmem;

	shmem = __drm_gem_shmem_create(dev, size, true);
	if (IS_ERR(shmem))
		return ERR_CAST(shmem);

	shmem->sgt = sgt;

	drm_dbg_prime(dev, "size = %zu\n", size);

	return &shmem->base;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_import_sg_table);

void BINDINGS_drm_gem_shmem_object_free(struct drm_gem_object *obj)
{
	drm_gem_shmem_object_free(obj);
}

void BINDINGS_drm_gem_shmem_object_print_info(struct drm_printer *p, unsigned int indent,
						   const struct drm_gem_object *obj) 
{
	drm_gem_shmem_object_print_info(p, indent, obj);	
}

int BINDINGS_drm_gem_shmem_object_pin(struct drm_gem_object *obj)
{
	return drm_gem_shmem_object_pin(obj);	
}

void BINDINGS_drm_gem_shmem_object_unpin(struct drm_gem_object *obj)
{
	drm_gem_shmem_object_unpin(obj);	
}

struct sg_table *BINDINGS_drm_gem_shmem_object_get_sg_table(struct drm_gem_object *obj)
{
	return drm_gem_shmem_object_get_sg_table(obj);
}

int BINDINGS_drm_gem_shmem_object_vmap(struct drm_gem_object *obj,
					    struct iosys_map *map)
{
	return drm_gem_shmem_object_vmap(obj, map);	
}

void BINDINGS_drm_gem_shmem_object_vunmap(struct drm_gem_object *obj,
					       struct iosys_map *map)
{
	drm_gem_shmem_object_vunmap(obj, map);	
}

MODULE_DESCRIPTION("DRM SHMEM memory-management helpers");
MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL v2");
