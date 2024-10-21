/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Generic page table allocator for IOMMUs in OpenBSD.
 *
 * Adapted from Linux's io-pgtable.c.
 *
 * Original Author: Will Deacon <will.deacon@arm.com>
 * OpenBSD Port: Your Name <your.email@example.com>
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <linux/io-pgtable.h>

/* Define the number of page table formats */
#define IO_PGTABLE_NUM_FMTS 1  /* Adjust this number as needed */

/* Initialize the page table formats */
static const struct io_pgtable_init_fns *
io_pgtable_init_table[IO_PGTABLE_NUM_FMTS] = {
    [APPLE_UAT]      = &io_pgtable_apple_uat_init_fns,
};

static int
check_custom_allocator(enum io_pgtable_fmt fmt, struct io_pgtable_cfg *cfg)
{
    /* No custom allocator, no need to check the format. */
    if (!cfg->alloc && !cfg->free)
        return 0;

    /* Both alloc and free functions should be provided. */
    if (!cfg->alloc || !cfg->free)
        return EINVAL;

    /* Make sure the format supports custom allocators. */
    if (io_pgtable_init_table[fmt]->caps & IO_PGTABLE_CAP_CUSTOM_ALLOCATOR)
        return 0;

    return EINVAL;
}

struct io_pgtable_ops *
alloc_io_pgtable_ops(enum io_pgtable_fmt fmt, struct io_pgtable_cfg *cfg, void *cookie)
{
    struct io_pgtable *iop;
    const struct io_pgtable_init_fns *fns;

    if (fmt >= IO_PGTABLE_NUM_FMTS)
        return NULL;

    if (check_custom_allocator(fmt, cfg))
        return NULL;

    fns = io_pgtable_init_table[fmt];
    if (!fns)
        return NULL;

    iop = fns->alloc(cfg, cookie);
    if (!iop)
        return NULL;

    iop->fmt    = fmt;
    iop->cookie = cookie;
    iop->cfg    = *cfg;

    return &iop->ops;
}

/*
 * It is the IOMMU driver's responsibility to ensure that the page table
 * is no longer accessible to the walker by this point.
 */
void
free_io_pgtable_ops(struct io_pgtable_ops *ops)
{
    struct io_pgtable *iop;

    if (!ops)
        return;

    iop = io_pgtable_ops_to_pgtable(ops);
    io_pgtable_tlb_flush_all(iop);
    io_pgtable_init_table[iop->fmt]->free(iop);
}
