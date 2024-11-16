/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CPU-agnostic ARM page table allocator for OpenBSD.
 *
 * Adapted from Linux's io-pgtable-arm.c for OpenBSD.
 *
 * Original Author: Will Deacon <will.deacon@arm.com>
 * OpenBSD Port: Your Name <your.email@example.com>
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/atomic.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <machine/bus.h>
#include <machine/atomic.h>

#include <uvm/uvm_extern.h>

#include <linux/io-pgtable.h>
#include <linux/gfp.h>
#include <linux/log2.h>

#include "io-pgtable-arm.h"

/* Constants and macros */

#define MAX_IAS          48  /* Maximum input address size */
#define MAX_OAS          48  /* Maximum output address size */
#define SUPPORTED_PAGE_SIZES   PAGE_SIZE_4K | PAGE_SIZE_16K
#define PAGE_SIZE_4K    (4 * 1024)
#define PAGE_SIZE_16K   (16 * 1024)

#define ARM_LPAE_MAX_ADDR_BITS     52
#define ARM_LPAE_S2_MAX_CONCAT_PAGES   16
#define ARM_LPAE_MAX_LEVELS           4

/* Page table bits */
#define ARM_LPAE_PTE_TYPE_SHIFT		0
#define ARM_LPAE_PTE_TYPE_MASK		0x3

#define ARM_LPAE_PTE_TYPE_BLOCK		1
#define ARM_LPAE_PTE_TYPE_TABLE		3
#define ARM_LPAE_PTE_TYPE_PAGE		3

#define ARM_LPAE_PTE_ADDR_MASK		GENMASK_ULL(47,12)

#define ARM_LPAE_PTE_NSTABLE		(((arm_lpae_iopte)1) << 63)
#define ARM_LPAE_PTE_XN			(((arm_lpae_iopte)3) << 53)
#define ARM_LPAE_PTE_DBM		(((arm_lpae_iopte)1) << 51)
#define ARM_LPAE_PTE_AF			(((arm_lpae_iopte)1) << 10)
#define ARM_LPAE_PTE_SH_NS		(((arm_lpae_iopte)0) << 8)
#define ARM_LPAE_PTE_SH_OS		(((arm_lpae_iopte)2) << 8)
#define ARM_LPAE_PTE_SH_IS		(((arm_lpae_iopte)3) << 8)
#define ARM_LPAE_PTE_NS			(((arm_lpae_iopte)1) << 5)
#define ARM_LPAE_PTE_VALID		(((arm_lpae_iopte)1) << 0)

#define ARM_LPAE_PTE_ATTR_LO_MASK	(((arm_lpae_iopte)0x3ff) << 2)
/* Ignore the contiguous bit for block splitting */
#define ARM_LPAE_PTE_ATTR_HI_MASK	(ARM_LPAE_PTE_XN | ARM_LPAE_PTE_DBM)
#define ARM_LPAE_PTE_ATTR_MASK		(ARM_LPAE_PTE_ATTR_LO_MASK |	\
					 ARM_LPAE_PTE_ATTR_HI_MASK)
/* Software bit for solving coherency races */
#define ARM_LPAE_PTE_SW_SYNC		(((arm_lpae_iopte)1) << 55)

/* Stage-1 PTE */
#define ARM_LPAE_PTE_AP_UNPRIV		(((arm_lpae_iopte)1) << 6)
#define ARM_LPAE_PTE_AP_RDONLY_BIT	7
#define ARM_LPAE_PTE_AP_RDONLY		(((arm_lpae_iopte)1) << \
					   ARM_LPAE_PTE_AP_RDONLY_BIT)
#define ARM_LPAE_PTE_AP_WR_CLEAN_MASK	(ARM_LPAE_PTE_AP_RDONLY | \
					 ARM_LPAE_PTE_DBM)
#define ARM_LPAE_PTE_ATTRINDX_SHIFT	2
#define ARM_LPAE_PTE_nG			(((arm_lpae_iopte)1) << 11)

/* Stage-2 PTE */
#define ARM_LPAE_PTE_HAP_FAULT		(((arm_lpae_iopte)0) << 6)
#define ARM_LPAE_PTE_HAP_READ		(((arm_lpae_iopte)1) << 6)
#define ARM_LPAE_PTE_HAP_WRITE		(((arm_lpae_iopte)2) << 6)
#define ARM_LPAE_PTE_MEMATTR_OIWB	(((arm_lpae_iopte)0xf) << 2)
#define ARM_LPAE_PTE_MEMATTR_NC		(((arm_lpae_iopte)0x5) << 2)
#define ARM_LPAE_PTE_MEMATTR_DEV	(((arm_lpae_iopte)0x1) << 2)

/* Register bits */
#define ARM_LPAE_VTCR_SL0_MASK		0x3

#define ARM_LPAE_TCR_T0SZ_SHIFT		0

#define ARM_LPAE_VTCR_PS_SHIFT		16
#define ARM_LPAE_VTCR_PS_MASK		0x7

#define ARM_LPAE_MAIR_ATTR_SHIFT(n)	((n) << 3)
#define ARM_LPAE_MAIR_ATTR_MASK		0xff
#define ARM_LPAE_MAIR_ATTR_DEVICE	0x04
#define ARM_LPAE_MAIR_ATTR_NC		0x44
#define ARM_LPAE_MAIR_ATTR_INC_OWBRWA	0xf4
#define ARM_LPAE_MAIR_ATTR_WBRWA	0xff
#define ARM_LPAE_MAIR_ATTR_IDX_NC	0
#define ARM_LPAE_MAIR_ATTR_IDX_CACHE	1
#define ARM_LPAE_MAIR_ATTR_IDX_DEV	2
#define ARM_LPAE_MAIR_ATTR_IDX_INC_OCACHE	3

#define ARM_MALI_LPAE_TTBR_ADRMODE_TABLE (3u << 0)
#define ARM_MALI_LPAE_TTBR_READ_INNER	BIT(2)
#define ARM_MALI_LPAE_TTBR_SHARE_OUTER	BIT(4)

#define ARM_MALI_LPAE_MEMATTR_IMP_DEF	0x88ULL
#define ARM_MALI_LPAE_MEMATTR_WRITE_ALLOC 0x8DULL

#define APPLE_UAT_MEMATTR_PRIV		(((arm_lpae_iopte)0x0) << 2)
#define APPLE_UAT_MEMATTR_DEV		(((arm_lpae_iopte)0x1) << 2)
#define APPLE_UAT_MEMATTR_SHARED	(((arm_lpae_iopte)0x2) << 2)
#define APPLE_UAT_GPU_ACCESS			(((arm_lpae_iopte)1) << 55)
#define APPLE_UAT_UXN				(((arm_lpae_iopte)1) << 54)
#define APPLE_UAT_PXN				(((arm_lpae_iopte)1) << 53)
#define APPLE_UAT_AP1				(((arm_lpae_iopte)1) << 7)
#define APPLE_UAT_AP0				(((arm_lpae_iopte)1) << 6)

/* Struct accessors */
#define io_pgtable_to_data(x)                      \
    ((struct arm_lpae_io_pgtable *)(x))

#define io_pgtable_ops_to_data(x)                  \
    io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

/* Calculate the right shift amount to get to the portion describing level l */
#define ARM_LPAE_LVL_SHIFT(l,d)                   \
    (((ARM_LPAE_MAX_LEVELS - (l)) * (d)->bits_per_level) +  \
    ilog2(sizeof(arm_lpae_iopte)))

#define ARM_LPAE_GRANULE(d)                       \
    (sizeof(arm_lpae_iopte) << (d)->bits_per_level)
#define ARM_LPAE_PGD_SIZE(d)                      \
    (sizeof(arm_lpae_iopte) << (d)->pgd_bits)

#define ARM_LPAE_PTES_PER_TABLE(d)                \
    (ARM_LPAE_GRANULE(d) >> ilog2(sizeof(arm_lpae_iopte)))

#define ARM_LPAE_LVL_IDX(addr, lvl, data) \
    (((addr) >> ARM_LPAE_LVL_SHIFT(lvl, data)) & \
     ((1UL << (data)->bits_per_level) - 1))

#define ARM_LPAE_BLOCK_SIZE(lvl, data) \
    (1UL << ARM_LPAE_LVL_SHIFT((lvl), data))

#define ARM_LPAE_ERR(fmt, ...) printf("arm_lpae_alloc_pages: " fmt "\n", ##__VA_ARGS__)

/* Data structures */

typedef uint64_t arm_lpae_iopte;

struct arm_lpae_io_pgtable {
    struct io_pgtable    iop;

    int         pgd_bits;
    int         start_level;
    int         bits_per_level;

    arm_lpae_iopte    *pgd;
};

/* Function prototypes */

static arm_lpae_iopte paddr_to_iopte(paddr_t paddr,
                     struct arm_lpae_io_pgtable *data);
static paddr_t iopte_to_paddr(arm_lpae_iopte pte,
                  struct arm_lpae_io_pgtable *data);

/* Implement missing functions */

static inline paddr_t getphys(void *vaddr)
{
    paddr_t pa;
    pmap_extract(pmap_kernel(), (vaddr_t)vaddr, &pa);
    return pa;
}

static inline int get_order(unsigned long size) {
    int order = 0;
    size = (size - 1) >> PAGE_SHIFT;
    while (size > 0) {
        size >>= 1;
        order++;
    }
    return order;
}

/* Default allocator: Allocates DMA-able memory using bus_dmamem_alloc */
static void *
arm_lpae_default_alloc_pages(size_t size, struct io_pgtable_cfg *cfg, int order)
{
    bus_dma_segment_t *segs = malloc(sizeof(bus_dma_segment_t), M_DEVBUF, M_WAITOK);
    void *pages = NULL;
    int error, nsegs = 1;

    /* Allocate DMA-able memory */
    error = bus_dmamem_alloc(cfg->dmat, size, PAGE_SIZE_16K, 0, segs, nsegs, &nsegs, BUS_DMA_NOWAIT | BUS_DMA_ZERO);
    if (error != 0) {
        ARM_LPAE_ERR("bus_dmamem_alloc failed with error %d", error);
        return NULL;
    }

    error = bus_dmamem_map(cfg->dmat, segs, nsegs, size, (caddr_t*)&pages, BUS_DMA_NOWAIT);
    if (error != 0) {
        ARM_LPAE_ERR("bus_dmamap_load failed with error %d", error);
        bus_dmamem_free(cfg->dmat, segs, nsegs);
        return NULL;
    }

    return pages;
}

/* Default freer: Note that actual freeing is handled by arm_lpae_free_pages */
static void
arm_lpae_default_free_pages(void *pages, int order)
{
    /* No action needed here as freeing is handled by arm_lpae_free_pages */
}

/* Memory allocation and deallocation functions */
static void *
__arm_lpae_alloc_pages(size_t size, gfp_t flags,
                     struct io_pgtable_cfg *cfg,
                     void *cookie)
{
    struct device *dev = cfg->iommu_dev;
    int order = get_order(size);
    void *pages;
    int error;

    /* Assert no high memory allocation */
    if (flags & __GFP_HIGHMEM)
        panic("arm_lpae_alloc_pages: highmem not supported");

    /* Allocate pages using custom allocator if provided, else use default */
    if (cfg->alloc)
        pages = cfg->alloc(cookie, size, flags);
    else
        pages = arm_lpae_default_alloc_pages(size, cfg, order);

    if (!pages) {
        printf("pages is null");
        return NULL;
    }

    /* Perform DMA mapping if walks are not coherent */
    if (!cfg->coherent_walk) {
        /* Load the DMA map and retrieve the DMA address */
        error = bus_dmamap_load(cfg->dmat, cfg->dmamap, pages, size, NULL, BUS_DMA_NOWAIT);
        if (error != 0)
            goto out_free;
    }

    return pages;

out_free:
    /* Free allocated pages using custom free function if provided, else use default */
    if (cfg->free)
        cfg->free(cookie, pages, size);
    else
        arm_lpae_default_free_pages(pages, order);

    return NULL;
}

static void __arm_lpae_free_pages(void *pages, size_t size,
                                  struct io_pgtable_cfg *cfg,
                                  void *cookie)
{
    // Assuming coherent_walk equivalent or handled differently in OpenBSD
    if (!cfg->coherent_walk) {
        // Use bus_dmamap_unload and bus_dmamap_destroy
        bus_dmamap_unload(cfg->dmat, cfg->dmamap);
        bus_dmamap_destroy(cfg->dmat, cfg->dmamap);
    }

    if (cfg->free)
        cfg->free(cookie, pages, size);
    else
        free(pages, M_DEVBUF, size);
}

/* Synchronization functions */

static void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries)
{
    membar_producer();
}

/* Mapping and unmapping functions */

static int __arm_lpae_map(struct arm_lpae_io_pgtable *data, vaddr_t iova,
              paddr_t paddr, size_t size, size_t pgcount,
              arm_lpae_iopte prot, int lvl, arm_lpae_iopte *ptep,
              gfp_t flags, size_t *mapped)
{
    vsize_t offset;
    vsize_t end_addr = iova + size*pgcount;
    int error = 0;

    /* Align the addresses to the page size */
    iova = trunc_page(iova);
    paddr = trunc_page(paddr);

    /* Iterate through the range of addresses and map them */
    for (offset = 0; offset < size*pgcount; offset += size) {
        vaddr_t curr_vaddr = iova + offset;
        paddr_t curr_paddr = paddr + offset;

        /* Insert the page table entry using OpenBSD's pmap_enter */
        for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
            error = pmap_enter(pmap_kernel(), curr_vaddr + offset*PAGE_SIZE, curr_paddr + offset*PAGE_SIZE, prot, flags | PMAP_CANFAIL);
            if (error != 0) {
                printf("Failed to map vaddr: %lx to paddr: %lx with error: %d\n", curr_vaddr, curr_paddr, error);
                return error;
            }
        }
        *mapped += size;
    }

    /* Ensure that changes to the page table take effect */
    pmap_update(pmap_kernel());

    return 0;
#ifdef notyet
    /* Implement mapping logic */

    /* Calculate the index at the current level */
    int idx = ARM_LPAE_LVL_IDX(iova, lvl, data);
    ptep += idx;

    /* If we can install a leaf entry at this level, then do so */
    if (size == ARM_LPAE_BLOCK_SIZE(lvl, data)) {
        arm_lpae_iopte pte = paddr_to_iopte(paddr, data) | prot;

        *ptep = pte;
        __arm_lpae_sync_pte(ptep, 1);
        *mapped += size;
        return 0;
    }

    /* We can't allocate tables at the final level */
    if (lvl >= ARM_LPAE_MAX_LEVELS - 1)
        return EINVAL;

    /* Grab a pointer to the next level */
    arm_lpae_iopte pte = *ptep;
    arm_lpae_iopte *cptep;

    if (!(pte & ARM_LPAE_PTE_TYPE_MASK)) {
        /* Allocate a new table */
        cptep = __arm_lpae_alloc_pages(ARM_LPAE_GRANULE(data), flags, &data->iop.cfg, data->iop.cookie);
        if (!cptep)
            return ENOMEM;

        /* Install the new table */
        *ptep = paddr_to_iopte((paddr_t)cptep, data) | ARM_LPAE_PTE_TYPE_TABLE;
        __arm_lpae_sync_pte(ptep, 1);
    } else if ((pte & ARM_LPAE_PTE_TYPE_MASK) == ARM_LPAE_PTE_TYPE_TABLE) {
        cptep = (arm_lpae_iopte *)(uintptr_t)iopte_to_paddr(pte, data);
    } else {
        /* We require an unmap first */
        return EEXIST;
    }

    /* Recurse to the next level */
    return __arm_lpae_map(data, iova, paddr, size, pgcount, prot,
                          lvl + 1, cptep, flags, mapped);
#endif
}

static int arm_lpae_map_pages(struct io_pgtable_ops *ops, vaddr_t iova,
                  paddr_t paddr, size_t pgsize, size_t pgcount,
                  int prot, gfp_t flags, size_t *mapped)
{
    struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
    arm_lpae_iopte *ptep = data->pgd;
    int ret, lvl = data->start_level;

    *mapped = 0;

    ret = __arm_lpae_map(data, iova, paddr, pgsize, pgcount, prot,
                         lvl, ptep, flags, mapped);

    /* Ensure all PTE updates are visible before any table walk */
    membar_producer();

    return ret;
}

static size_t __arm_lpae_unmap(struct arm_lpae_io_pgtable *data,
                   struct iommu_iotlb_gather *gather,
                   vaddr_t iova, size_t size, size_t pgcount,
                   int lvl, arm_lpae_iopte *ptep)
{
    vsize_t offset;
    vsize_t end_addr = iova + size*pgcount;

    /* Align the addresses to the page size */
    iova = trunc_page(iova);

    /* Unmap the virtual address range */
    for (offset = 0; offset < size*pgcount; offset += size) {
        vaddr_t curr_vaddr = iova + offset;

        pmap_kremove(curr_vaddr, size);
    }

    /* Ensure that the page table changes are synchronized */
    pmap_update(pmap_kernel());

    return size*pgcount;
#ifdef notyet
    /* Implement unmapping logic */

    /* Calculate the index at the current level */
    int idx = ARM_LPAE_LVL_IDX(iova, lvl, data);
    ptep += idx;

    arm_lpae_iopte pte = *ptep;

    if (!(pte & ARM_LPAE_PTE_TYPE_MASK))
        return 0;

    if (size == ARM_LPAE_BLOCK_SIZE(lvl, data)) {
        *ptep = 0;
        __arm_lpae_sync_pte(ptep, 1);
        return size;
    } else if ((pte & ARM_LPAE_PTE_TYPE_MASK) == ARM_LPAE_PTE_TYPE_TABLE) {
        /* Recurse to the next level */
        arm_lpae_iopte *cptep = (arm_lpae_iopte *)(uintptr_t)iopte_to_paddr(pte, data);
        return __arm_lpae_unmap(data, gather, iova, size, pgcount, lvl + 1, cptep);
    } else {
        /* Cannot unmap a block of incorrect size */
        return 0;
    }
#endif
}

static size_t arm_lpae_unmap_pages(struct io_pgtable_ops *ops, vaddr_t iova,
                       size_t pgsize, size_t pgcount, struct iommu_iotlb_gather *gather)
{
    struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
    struct io_pgtable_cfg *cfg = &data->iop.cfg;
	arm_lpae_iopte *ptep = data->pgd;
	long iaext = (s64)iova >> cfg->ias;

    if (cfg->quirks & IO_PGTABLE_QUIRK_ARM_TTBR1)
		iaext = ~iaext;

    return __arm_lpae_unmap(data, gather, iova, pgsize, pgcount,
				data->start_level, ptep);
}

/* Address translation function */

static paddr_t arm_lpae_iova_to_phys(struct io_pgtable_ops *ops,
                     vaddr_t iova)
{
    paddr_t phys;
    if (pmap_extract(pmap_kernel(), iova, &phys)) {
        return phys;
    } else {
        return (paddr_t)-1;
    }
#ifdef notyet
    struct arm_lpae_io_pgtable *data = io_pgtable_ops_to_data(ops);
    arm_lpae_iopte pte, *ptep = data->pgd;
    int lvl = data->start_level;

    do {
        /* Valid PTE pointer? */
        if (!ptep)
            return 0;

        /* Grab the PTE we're interested in */
        int idx = ARM_LPAE_LVL_IDX(iova, lvl, data);
        ptep += idx;
        pte = *ptep;

        /* Valid entry? */
        if (!(pte & ARM_LPAE_PTE_TYPE_MASK))
            return 0;

        /* Leaf entry? */
        if ((pte & ARM_LPAE_PTE_TYPE_MASK) == ARM_LPAE_PTE_TYPE_BLOCK ||
            (pte & ARM_LPAE_PTE_TYPE_MASK) == ARM_LPAE_PTE_TYPE_PAGE)
            break;

        /* Take it to the next level */
        ptep = (arm_lpae_iopte *)(uintptr_t)iopte_to_paddr(pte, data);
    } while (++lvl < ARM_LPAE_MAX_LEVELS);

    /* Compute physical address */
    paddr_t paddr = iopte_to_paddr(pte, data);
    paddr |= iova & (ARM_LPAE_BLOCK_SIZE(lvl, data) - 1);

    return paddr;
#endif
}

/* Helper functions */

static arm_lpae_iopte paddr_to_iopte(paddr_t paddr, struct arm_lpae_io_pgtable *data)
{
    return (arm_lpae_iopte)paddr;
}

static paddr_t iopte_to_paddr(arm_lpae_iopte pte, struct arm_lpae_io_pgtable *data)
{
    return (paddr_t)(pte & ~((1ULL << data->bits_per_level) - 1));
}

/* Initialization and cleanup functions */

static struct arm_lpae_io_pgtable *
arm_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
    struct arm_lpae_io_pgtable *data;

    /* No quirks for this implementation */
    if (cfg->quirks)
        return NULL;

    /* Validate input and output address sizes */
    if (cfg->ias > MAX_IAS || cfg->oas > MAX_OAS)
        return NULL;

    /* Limit to supported page sizes (e.g., 4K and 16K) */
    cfg->pgsize_bitmap &= SUPPORTED_PAGE_SIZES;

    /* Allocate the arm_lpae_io_pgtable structure */
    data = malloc(sizeof(*data), M_DEVBUF, M_WAITOK | M_ZERO);
    if (!data)
        return NULL;

    /* Initialize fields */
    data->iop.cfg = *cfg;
    data->iop.cookie = cookie;

    /* Allocate the PGD with the required size and DMA mapping */
    data->pgd = __arm_lpae_alloc_pages(PAGE_SIZE_16K, GFP_KERNEL, cfg, cookie);
    if (!data->pgd)
        goto out_free_data;

    /* Ensure the PGD is visible before writing TTBR */
    membar_producer();

    /* Set the Translation Table Base Register */
    cfg->arm_lpae_s1_cfg.ttbr = getphys(data->pgd);

    /* Assign page table operations */
    data->iop.ops = (struct io_pgtable_ops) {
        .map_pages = arm_lpae_map_pages,
        .unmap_pages = arm_lpae_unmap_pages,
        .iova_to_phys = arm_lpae_iova_to_phys,
    };

    /* Return the initialized io_pgtable_ops structure */
    return data;

out_free_data:
    /* Free allocated resources */
    if (data)
        free(data, M_DEVBUF, sizeof(*data));
    return NULL;
}

static void arm_lpae_free_pgtable(struct io_pgtable *iop)
{
    struct arm_lpae_io_pgtable *data = io_pgtable_to_data(iop);

    __arm_lpae_free_pages(data->pgd, ARM_LPAE_PGD_SIZE(data), &data->iop.cfg, data->iop.cookie);
    //free(data, M_DEVBUF, sizeof(*data));
}

static struct io_pgtable *
apple_uat_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
    struct arm_lpae_io_pgtable *data;

    /* No quirks for UAT (hopefully) */
    if (cfg->quirks)
        return NULL;

    if (cfg->ias > 48 || cfg->oas > 42)
        return NULL;

    /* Only 16K page size is supported */
    cfg->pgsize_bitmap &= PAGE_SIZE_16K;

    data = arm_lpae_alloc_pgtable(cfg, cookie);
    if (!data)
        return NULL;

    /* UAT needs full 16K aligned pages for the pgd */
    data->pgd = __arm_lpae_alloc_pages(PAGE_SIZE_16K, M_WAITOK | M_ZERO, cfg, cookie);
    if (!data->pgd)
        goto out_free_data;

    /* Ensure the empty pgd is visible before the TTBAT can be written */
    membar_producer();

    cfg->apple_uat_cfg.ttbr = getphys(data->pgd);

    return &data->iop;

out_free_data:
    arm_lpae_free_pgtable((struct io_pgtable *)data);
    return NULL;
}

struct io_pgtable_init_fns io_pgtable_apple_uat_init_fns = {
	.alloc	= apple_uat_alloc_pgtable,
	.free	= arm_lpae_free_pgtable,
};
