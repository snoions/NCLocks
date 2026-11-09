/*
 * Log-linear latency histogram: 16 sub-buckets per power of two, so a reported
 * percentile is the bucket's upper bound and overestimates by at most 1/16
 * (6.25%). Exact for values < 16. Values in nanoseconds; range up to 2^40 ns
 * (~18 min), larger values clamp to the last bucket. Max is tracked exactly.
 */
#ifndef NCL_BENCH_HIST_H
#define NCL_BENCH_HIST_H

#include <stdint.h>
#include <string.h>

#define HIST_SUB_BITS 4
#define HIST_SUB (1u << HIST_SUB_BITS)
#define HIST_MAX_EXP 40
#define HIST_NB ((HIST_MAX_EXP - HIST_SUB_BITS + 2) * HIST_SUB)

struct hist {
    uint64_t n, sum, max;
    uint64_t b[HIST_NB];
};

static inline unsigned hist_idx(uint64_t v)
{
    if (v < HIST_SUB)
        return (unsigned)v;
    unsigned e = 63u - (unsigned)__builtin_clzll(v);
    if (e > HIST_MAX_EXP)
        return HIST_NB - 1;
    unsigned sub = (unsigned)(v >> (e - HIST_SUB_BITS)) & (HIST_SUB - 1);
    return (e - HIST_SUB_BITS + 1) * HIST_SUB + sub;
}

static inline uint64_t hist_upper(unsigned idx)
{
    if (idx < HIST_SUB)
        return idx;
    unsigned e = idx / HIST_SUB + HIST_SUB_BITS - 1, sub = idx % HIST_SUB;
    uint64_t lo = (uint64_t)(HIST_SUB + sub) << (e - HIST_SUB_BITS);
    return lo + ((uint64_t)1 << (e - HIST_SUB_BITS)) - 1;
}

static inline void hist_add(struct hist *h, uint64_t v)
{
    h->n++;
    h->sum += v;
    if (v > h->max)
        h->max = v;
    h->b[hist_idx(v)]++;
}

static inline void hist_merge(struct hist *d, const struct hist *s)
{
    d->n += s->n;
    d->sum += s->sum;
    if (s->max > d->max)
        d->max = s->max;
    for (unsigned i = 0; i < HIST_NB; i++)
        d->b[i] += s->b[i];
}

/* q in [0,1]. Returns the upper bound of the bucket holding the q-quantile,
 * capped at the exact max. */
static inline uint64_t hist_pct(const struct hist *h, double q)
{
    if (!h->n)
        return 0;
    uint64_t target = (uint64_t)(q * (double)(h->n - 1)), acc = 0;
    for (unsigned i = 0; i < HIST_NB; i++) {
        acc += h->b[i];
        if (acc > target) {
            uint64_t u = hist_upper(i);
            return u < h->max ? u : h->max;
        }
    }
    return h->max;
}

static inline double hist_mean(const struct hist *h)
{
    return h->n ? (double)h->sum / (double)h->n : 0.0;
}

#endif
