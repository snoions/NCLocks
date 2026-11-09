/*
 * Global ("base") mutual-exclusion algorithms over P participants indexed
 * 0..P-1 (design §4). They use only plain 8-byte loads/stores on hot metadata
 * plus explicit fences — no cross-node atomics.
 *
 *   (a) LAMPORT     Lamport's fast mutex.
 *   (b) TOURNAMENT  Binary tree of two-party Peterson locks.
 *   (d) HYBRID      Splitter fast path + tournament fallback, joined by a top
 *                   two-party Peterson lock. The fast-path reset rule is
 *                   specified in docs/hybrid.md and is NOT yet model-checked.
 *
 * Every variable is its own ncl_line_t. The per-lock block of lines is laid
 * out by ncl_base_nlines()/ncl_base_bind(); callers place it in the metadata
 * region.
 */
#ifndef NCL_BASE_H
#define NCL_BASE_H

#include "ncl/mem.h"

typedef enum {
    NCL_ALG_LAMPORT = 0,
    NCL_ALG_TOURNAMENT = 1,
    NCL_ALG_HYBRID = 2,
} ncl_alg_t;

/* Path taken by an acquisition (for stats and, for HYBRID, needed at release). */
enum {
    NCL_PATH_LAMPORT_FAST = 0,
    NCL_PATH_LAMPORT_SLOW,     /* went through the b[] scan */
    NCL_PATH_TOURNAMENT,
    NCL_PATH_HYBRID_FAST,
    NCL_PATH_HYBRID_SLOW,
    NCL_PATH__COUNT,
};

extern const char *const ncl_path_names[NCL_PATH__COUNT];

/* Process-local view of one lock's base-algorithm state. */
struct ncl_base {
    ncl_alg_t alg;
    int P;          /* participants */
    int leaves;     /* tournament leaves: next power of two >= P */
    ncl_line_t *lam;  /* LAMPORT/HYBRID: [x, y, b[0..P-1]] */
    ncl_line_t *tree; /* TOURNAMENT/HYBRID: 3 lines per internal node */
    ncl_line_t *top;  /* HYBRID: [sflag, turn, fflag[0..P-1]] */
};

struct ncl_base_stats {
    uint64_t hybrid_resets;       /* stale splitter reset by a slow holder */
    uint64_t hybrid_reset_skips;  /* stale splitter seen but b[] not clear */
    uint64_t lamport_retries;     /* Lamport `goto start` taken */
};

size_t ncl_base_nlines(ncl_alg_t alg, int P);
void ncl_base_bind(struct ncl_base *b, ncl_alg_t alg, int P, ncl_line_t *lines);

/* Acquire as participant i; returns the NCL_PATH_* taken. */
int ncl_base_lock(struct ncl_base *b, int i, struct ncl_base_stats *st);
/* Release as participant i; `path` is the value returned by ncl_base_lock. */
void ncl_base_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st);

const char *ncl_alg_name(ncl_alg_t a);
int ncl_alg_parse(const char *s, ncl_alg_t *out);

/* Exponential backoff for spin loops (pause iterations, capped). */
extern unsigned ncl_backoff_cap;

struct ncl_backoff { unsigned cur; };
static inline void ncl_backoff_init(struct ncl_backoff *bo) { bo->cur = 1; }
static inline void ncl_backoff(struct ncl_backoff *bo)
{
    for (unsigned k = 0; k < bo->cur; k++)
        ncl_cpu_relax();
    if (bo->cur < ncl_backoff_cap)
        bo->cur <<= 1;
}

#endif
