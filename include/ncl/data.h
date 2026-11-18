/*
 * Data maintenance layer (design §2.1): writeback and invalidation of
 * protected data, separate from the lock algorithms.
 *
 * A lock tells this layer only two things:
 *  - after acquiring: whether the lock ARRIVED at the node (cross-node) or was
 *    passed by a local handoff within the node's current run of ownership;
 *  - at release, before its own release work: whether the lock is DEPARTING
 *    (leaving the node). The lock fences before its handoff store, so departure
 *    writebacks may be issued without their own fence when it asks for that.
 *
 * Policies (struct ncl_data_cfg.precise):
 *  - conservative: invalidate the context's whole range on arrival; on
 *    departure write back the run's dirty hints, or the whole range if some
 *    release gave none or the list overflowed;
 *  - precise: invalidate only the ranges named at acquire (on arrival, and on
 *    a local handoff unless fetched in this run; dirty parts written back
 *    first); dirty hints are mandatory and written back on departure, early
 *    on overflow, never as the whole range.
 * Both track ranges per context (per lock), so data must stay bound to one
 * lock (design §2.1, "Scope").
 */
#ifndef NCL_DATA_H
#define NCL_DATA_H

#include "ncl/mem.h"

#include <stdio.h>
#include <stdlib.h>

struct ncl_range {
    const void *p;
    size_t len;
};

#define NCL_DIRTY_MAX 16 /* dirty ranges remembered per run */
#define NCL_FRESH_MAX 32 /* PRECISE: fetched ranges remembered per run */

struct ncl_data_cfg {
    int precise;       /* precise policy (cfg.precise_data) */
    int skip_wb;       /* DEBUG: no writebacks (the emulator must catch it) */
    int skip_inv;      /* DEBUG: no invalidates */
    int debug_precise; /* DEBUG, precise: 1 = no refresh on local handoffs,
                          2 = no writeback of dirty lines before invalidating */
};

/* Node-local state of one lock's data, written only by the lock's holder. */
struct ncl_data_ctx {
    void *data;  /* registered range (NULL, 0 = none) */
    size_t len;
    int ndirty, dirty_full;
    struct ncl_range dirty[NCL_DIRTY_MAX];
    int nfresh;
    struct { uintptr_t lo, hi; } fresh[NCL_FRESH_MAX];
};

/* Each node, before its threads run. */
extern struct ncl_data_cfg ncl_dcfg; /* node-local copy */
void ncl_data_configure(const struct ncl_data_cfg *c);
static inline void ncl_data_ctx_init(struct ncl_data_ctx *d, void *data, size_t len)
{
    d->data = data;
    d->len = len;
    d->ndirty = d->dirty_full = d->nfresh = 0;
}

/* ncl_data_acquired: after the lock is held and its acquire fence issued.
 * arrived: the lock came from another node (else a local handoff). r, n:
 * precise policy only.
 * ncl_data_release: at release, before the lock's release work. hints ==
 * NULL: unknown writes (conservative only). departing: the lock leaves the
 * node. nofence: the lock fences before its handoff store, so issue
 * writebacks without a trailing fence.
 * Inline: they sit on every acquire and release. */

static inline void ncl__line_bounds(const struct ncl_range *r, uintptr_t *lo, uintptr_t *hi)
{
    *lo = (uintptr_t)r->p & ~(uintptr_t)(NCL_CACHELINE - 1);
    *hi = ((uintptr_t)r->p + r->len + NCL_CACHELINE - 1) & ~(uintptr_t)(NCL_CACHELINE - 1);
}

static inline void ncl__check_hint(const struct ncl_range *h)
{
    const char *p = h->p;
    if (p < ncl_mem.data || p + h->len > ncl_mem.data + ncl_mem.data_size) {
        fprintf(stderr, "ncl: dirty hint outside data region\n");
        abort();
    }
}

/* PRECISE: make the acquired ranges current. */
static inline void ncl__precise_refresh(struct ncl_data_ctx *D, const struct ncl_range *r, int n,
                                        int arrival)
{
    const uintptr_t dlo = (uintptr_t)D->data, dhi = dlo + D->len;
    for (int k = 0; k < n; k++) {
        uintptr_t lo, hi;
        ncl__line_bounds(&r[k], &lo, &hi);
        if (r[k].len == 0)
            continue;
        if ((uintptr_t)r[k].p < dlo || (uintptr_t)r[k].p + r[k].len > dhi) {
            fprintf(stderr, "ncl: acquire range outside the lock's data\n");
            abort();
        }
        /* Fetched earlier in this run: current (no other node can have
         * written it, and the node is coherent). */
        int fresh = !arrival && ncl_dcfg.debug_precise == 1;
        for (int f = 0; f < D->nfresh && !fresh; f++)
            fresh = D->fresh[f].lo <= lo && hi <= D->fresh[f].hi;
        if (fresh)
            continue;
        /* Dirty in this run: write that part back before dropping it
         * (clflushopt does so in hardware; the emulator's invalidate would
         * lose it). */
        for (int d = 0; d < D->ndirty && ncl_dcfg.debug_precise != 2; d++) {
            uintptr_t a, b;
            ncl__line_bounds(&D->dirty[d], &a, &b);
            if (a < lo) a = lo;
            if (b > hi) b = hi;
            if (a < b)
                ncl_data_writeback_nofence((const void *)a, b - a);
        }
        if (!ncl_dcfg.skip_inv)
            ncl_data_invalidate((const void *)lo, hi - lo); /* fences before and after */
        if (D->nfresh < NCL_FRESH_MAX) {
            D->fresh[D->nfresh].lo = lo;
            D->fresh[D->nfresh].hi = hi;
            D->nfresh++;
        }
    }
}

static inline void ncl_data_acquired(struct ncl_data_ctx *D, int arrived,
                                     const struct ncl_range *r, int n)
{
    if (arrived) {
        /* New run of ownership on this node. */
        D->ndirty = 0;
        D->dirty_full = 0;
        D->nfresh = 0;
    }
    if (ncl_dcfg.precise)
        ncl__precise_refresh(D, r, n, arrived);
    else if (arrived && D->len && !ncl_dcfg.skip_inv)
        ncl_data_invalidate(D->data, D->len); /* drop stale cached data */
    /* Conservative local handoff: everything was invalidated on arrival and
     * the node is coherent, so nothing to do. */
}

/* Remember a release's dirty ranges for the departure. */
static inline void ncl__accumulate_dirty(struct ncl_data_ctx *D, const struct ncl_range *hints,
                                         int nhints)
{
    if (!hints) {
        D->dirty_full = 1;
        return;
    }
    for (int k = 0; k < nhints && !D->dirty_full; k++) {
        ncl__check_hint(&hints[k]);
        if (D->ndirty == NCL_DIRTY_MAX && ncl_dcfg.precise) {
            /* PRECISE never falls back to the whole range (lines this run
             * never fetched may be stale): write the list back now. */
            for (int d = 0; d < D->ndirty; d++)
                ncl_data_writeback_nofence(D->dirty[d].p, D->dirty[d].len);
            ncl_fence();
            D->ndirty = 0;
        }
        if (D->ndirty == NCL_DIRTY_MAX)
            D->dirty_full = 1;
        else
            D->dirty[D->ndirty++] = hints[k];
    }
}

static inline void ncl_data_release(struct ncl_data_ctx *D, const struct ncl_range *hints,
                                    int nhints,
                      int departing, int nofence)
{
    if (ncl_dcfg.precise && !hints) {
        fprintf(stderr, "ncl: precise_data: release needs dirty hints (nhints may be 0)\n");
        abort();
    }
    ncl__accumulate_dirty(D, hints, nhints);
    if (!departing || ncl_dcfg.skip_wb)
        return;
    /* Departure: write back eagerly (design §2 rule 5). */
    void (*wb)(const void *, size_t) = nofence ? ncl_data_writeback_nofence : ncl_data_writeback;
    if (D->dirty_full) {
        if (D->len)
            wb(D->data, D->len);
    } else {
        for (int k = 0; k < D->ndirty; k++)
            wb(D->dirty[k].p, D->dirty[k].len);
    }
}

#endif
