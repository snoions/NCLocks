/*
 * Node-local submutex (design §3.1): MCS lock on ordinary coherent memory.
 *
 * Supports local-waiter detection (ncl_sub_has_waiter) for cohort handoff in
 * Phase 2a. The holder's queue node is recorded in the submutex itself, so
 * unlock needs no thread-local state (thread-obliviousness, design §3.2).
 */
#ifndef NCL_SUBMUTEX_H
#define NCL_SUBMUTEX_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "ncl/config.h"

struct ncl_qnode {
    _Atomic(struct ncl_qnode *) next;
    _Atomic int locked;
} NCL_ALIGNED(NCL_CACHELINE);

struct ncl_submutex {
    _Atomic(struct ncl_qnode *) tail;
    struct ncl_qnode *holder; /* written only by the holder */
} NCL_ALIGNED(NCL_CACHELINE);

static inline void ncl_sub_init(struct ncl_submutex *m)
{
    atomic_init(&m->tail, NULL);
    m->holder = NULL;
}

static inline void ncl_sub_lock(struct ncl_submutex *m, struct ncl_qnode *q)
{
    atomic_store_explicit(&q->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&q->locked, 1, memory_order_relaxed);
    struct ncl_qnode *prev = atomic_exchange_explicit(&m->tail, q, memory_order_acq_rel);
    if (prev) {
        atomic_store_explicit(&prev->next, q, memory_order_release);
        unsigned spins = 0;
        while (atomic_load_explicit(&q->locked, memory_order_acquire))
            ncl_spin_wait(&spins); /* predecessor may be preempted */
    }
    m->holder = q;
}

/* True if another thread is queued behind the holder. Caller holds m. */
static inline bool ncl_sub_has_waiter(struct ncl_submutex *m)
{
    struct ncl_qnode *q = m->holder;
    return atomic_load_explicit(&q->next, memory_order_acquire) != NULL ||
           atomic_load_explicit(&m->tail, memory_order_acquire) != q;
}

static inline void ncl_sub_unlock(struct ncl_submutex *m)
{
    struct ncl_qnode *q = m->holder;
    struct ncl_qnode *next = atomic_load_explicit(&q->next, memory_order_acquire);
    if (!next) {
        struct ncl_qnode *expect = q;
        if (atomic_compare_exchange_strong_explicit(&m->tail, &expect, NULL,
                                                    memory_order_release,
                                                    memory_order_relaxed))
            return;
        while (!(next = atomic_load_explicit(&q->next, memory_order_acquire)))
            ncl_cpu_relax();
    }
    atomic_store_explicit(&next->locked, 0, memory_order_release);
}

#endif
