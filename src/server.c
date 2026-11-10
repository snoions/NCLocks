#define _GNU_SOURCE
#include "server.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MSG_REQ = 1, MSG_REL = 2 };
enum { REPLY_GRANT = 1 };

#define M_POS_SHIFT 48
#define M_TYPE_SHIFT 46
#define M_LOCK_SHIFT 26
#define M_GSLOT_SHIFT 14
#define MASK(bits) ((1ull << (bits)) - 1)

static inline uint64_t msg_encode(uint64_t pos, unsigned type, uint32_t lock, uint32_t gslot,
                                  uint32_t reqid)
{
    return ((pos & 0xffff) << M_POS_SHIFT) | ((uint64_t)type << M_TYPE_SHIFT) |
           ((uint64_t)lock << M_LOCK_SHIFT) | ((uint64_t)gslot << M_GSLOT_SHIFT) |
           (reqid & MASK(NCL_SRV_REQID_BITS));
}

static inline uint64_t reply_encode(uint32_t lock, unsigned status, uint32_t reqid)
{
    return ((uint64_t)lock << 16) | ((uint64_t)status << 14) | (reqid & MASK(NCL_SRV_REQID_BITS));
}

/* ---- layout ------------------------------------------------------------- */

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

int ncl_srv_home(int lock, int nnodes) { return (int)(mix64((uint64_t)lock + 1) % (uint64_t)nnodes); }

static uint32_t pow2ceil32(uint32_t n)
{
    uint32_t p = 2;
    while (p < n)
        p <<= 1;
    return p;
}

size_t ncl_srv_layout(const struct ncl_sys_cfg *c, struct ncl_srv_layout *L, ncl_off_t base_off)
{
    memset(L, 0, sizeof *L);
    if (c->nlocks > (1 << NCL_SRV_LOCK_BITS) || c->threads_per_node < 1 ||
        c->threads_per_node > (1 << NCL_SRV_GSLOT_BITS)) {
        fprintf(stderr, "ncl: server: nlocks <= %d and threads/node <= %d required\n",
                1 << NCL_SRV_LOCK_BITS, 1 << NCL_SRV_GSLOT_BITS);
        return 0;
    }
    uint32_t homed[NCL_MAX_NODES] = {0};
    for (int l = 0; l < c->nlocks; l++)
        homed[ncl_srv_home(l, c->nnodes)]++;

    ncl_off_t off = base_off;
    for (int h = 0; h < c->nnodes; h++) {
        L->cap[h] = pow2ceil32(2 * homed[h]);
        if (L->cap[h] > NCL_SRV_MAX_CAP) {
            fprintf(stderr, "ncl: server: node %d homes %u locks; queue cap %u > %d\n", h,
                    homed[h], L->cap[h], NCL_SRV_MAX_CAP);
            return 0;
        }
        L->q_off[h] = off;
        off += (ncl_off_t)c->nnodes * L->cap[h] * NCL_CACHELINE;
    }
    L->grants_per_node = (uint32_t)c->threads_per_node;
    L->grant_off = off;
    off += (ncl_off_t)c->nnodes * L->grants_per_node * NCL_CACHELINE;
    return (size_t)(off - base_off);
}

/* ---- node state ---------------------------------------------------------- */

struct waiter {
    uint8_t node;
    uint16_t gslot;
    uint16_t reqid;
};

enum { SLOW_NONE, SLOW_TRYING, SLOW_HOLDING };

/* Server-side per-lock state (node-local, server thread only). */
struct srv_lock {
    uint32_t lock;
    int held;    /* granted to a node that has not released yet */
    int holder;
    int qh, qn;  /* FIFO of waiting requests; <= 1 per node with the submutex */
    struct waiter q[NCL_MAX_NODES];
    /* (e) only */
    int slow;    /* SLOW_*: state of the top lock's SLOW side */
    int grants_this_hold; /* grants since the SLOW side was last acquired */
    int in_trying;
    struct ncl_base b;
};

struct tail {
    _Atomic uint64_t v;
} NCL_ALIGNED(NCL_CACHELINE);

static struct {
    struct ncl_sys_cfg cfg;
    struct ncl_srv_layout L;
    int node, N, fast;
    unsigned backoff_cap;
    struct tail tail[NCL_MAX_NODES]; /* producer side: next position into h */

    /* server thread */
    pthread_t tid;
    int running;
    _Atomic int stop;
    uint64_t head[NCL_MAX_NODES];   /* next position expected from source s */
    int *local_idx;                 /* lock -> index into slocks, -1 if not homed */
    struct srv_lock *slocks;
    int *trying, ntrying;           /* (e): locks whose SLOW side is being acquired */
    struct ncl_srv_stats st;
} S;

static ncl_line_t *q_slot(int src, int h, uint64_t pos)
{
    ncl_line_t *base = (ncl_line_t *)ncl_meta_ptr(S.L.q_off[h]);
    return base + (size_t)src * S.L.cap[h] + (pos & (S.L.cap[h] - 1));
}

static ncl_line_t *grant_slot(int node, uint32_t g)
{
    return (ncl_line_t *)ncl_meta_ptr(S.L.grant_off) + (size_t)node * S.L.grants_per_node + g;
}

static void send(int h, unsigned type, uint32_t lock, uint32_t gslot, uint32_t reqid)
{
    uint64_t pos = atomic_fetch_add_explicit(&S.tail[h].v, 1, memory_order_relaxed);
    /* Keep reserve->publish tiny: a preempted producer stalls server h. */
    ncl_fence(); /* prior effects (data writeback, present[i]=0) before publishing */
    ncl_uc_store(q_slot(S.node, h, pos), msg_encode(pos, type, lock, gslot, reqid));
}

/* ---- server thread -------------------------------------------------------- */

static void grant_next(struct srv_lock *sl)
{
    struct waiter wt = sl->q[sl->qh];
    sl->qh = (sl->qh + 1) % S.N;
    sl->qn--;
    sl->held = 1;
    sl->holder = wt.node;
    sl->grants_this_hold++;
    ncl_fence(); /* the message(s) / top-lock reads that allow this grant come first */
    ncl_uc_store(grant_slot(wt.node, wt.gslot), reply_encode(sl->lock, REPLY_GRANT, wt.reqid));
    S.st.grants++;
}

/* Drive one lock's state machine as far as it can go without waiting.
 * (c): grant whenever free and someone is queued.
 * (e) (design §4e): the SLOW side of the top lock must be held to grant; it
 * is acquired when the queue becomes non-empty and released (followed by the
 * splitter-reset check) when a release finds the queue empty. */
static void advance(struct srv_lock *sl)
{
    if (S.fast) {
        if (sl->slow == SLOW_NONE && sl->qn) {
            ncl_top_announce(&sl->b, NCL_SIDE_SLOW);
            sl->slow = SLOW_TRYING;
        }
        if (sl->slow == SLOW_TRYING) {
            S.st.top_wait_polls++;
            if (!ncl_top_may_enter(&sl->b, NCL_SIDE_SLOW)) {
                if (!sl->in_trying) { /* poll again in later rounds */
                    sl->in_trying = 1;
                    S.trying[S.ntrying++] = (int)(sl - S.slocks);
                }
                return;
            }
            sl->slow = SLOW_HOLDING;
            sl->grants_this_hold = 0;
            S.st.top_acquires++;
        }
        /* Starvation fix: yield to a waiting fast winner between grants, but
         * only after granting at least once in this hold (else a stream of
         * fast winners could starve the queue instead). */
        if (sl->slow == SLOW_HOLDING && !sl->held && sl->qn && sl->grants_this_hold > 0 &&
            ncl_top_other_flag(&sl->b, NCL_SIDE_SLOW)) {
            ncl_fence();
            ncl_top_unlock(&sl->b, NCL_SIDE_SLOW);
            S.st.top_yields++;
            ncl_top_announce(&sl->b, NCL_SIDE_SLOW); /* turn = SLOW: fast goes first */
            sl->slow = SLOW_TRYING;
            if (!sl->in_trying) {
                sl->in_trying = 1;
                S.trying[S.ntrying++] = (int)(sl - S.slocks);
            }
            return;
        }
    }
    if (!sl->held && sl->qn) {
        grant_next(sl);
    } else if (S.fast && !sl->held && !sl->qn && sl->slow == SLOW_HOLDING) {
        ncl_fence();
        ncl_top_unlock(&sl->b, NCL_SIDE_SLOW);
        sl->slow = SLOW_NONE;
        ncl_fence();
        /* Releasing grantee cleared present[] before its REL, so check all. */
        ncl_splitter_reset_if_quiet(&sl->b, -1, &S.st.base);
    }
}

static void process(int src, uint64_t w)
{
    unsigned type = (unsigned)(w >> M_TYPE_SHIFT) & 3;
    uint32_t lock = (uint32_t)(w >> M_LOCK_SHIFT) & MASK(NCL_SRV_LOCK_BITS);
    uint32_t gslot = (uint32_t)(w >> M_GSLOT_SHIFT) & MASK(NCL_SRV_GSLOT_BITS);
    uint32_t reqid = (uint32_t)w & MASK(NCL_SRV_REQID_BITS);
    S.st.msgs++;

    /* Bounds-check everything before acting on the message's behalf. */
    if (lock >= (uint32_t)S.cfg.nlocks || S.local_idx[lock] < 0 || gslot >= S.L.grants_per_node) {
        S.st.errors++;
        return;
    }
    struct srv_lock *sl = &S.slocks[S.local_idx[lock]];
    if (type == MSG_REQ) {
        if (sl->qn >= S.N) {
            S.st.errors++; /* > 1 request per node per lock: protocol violation */
            return;
        }
        sl->q[(sl->qh + sl->qn) % S.N] =
            (struct waiter){(uint8_t)src, (uint16_t)gslot, (uint16_t)reqid};
        sl->qn++;
        if (sl->held)
            S.st.queued++;
    } else if (type == MSG_REL) {
        if (!sl->held || sl->holder != src) {
            S.st.errors++;
            return;
        }
        sl->held = 0;
    } else {
        S.st.errors++;
        return;
    }
    advance(sl);
}

static void poll_trying(void)
{
    int k = 0;
    while (k < S.ntrying) {
        struct srv_lock *sl = &S.slocks[S.trying[k]];
        advance(sl);
        if (sl->slow != SLOW_TRYING) {
            sl->in_trying = 0;
            S.trying[k] = S.trying[--S.ntrying];
        } else {
            k++;
        }
    }
}

static void *server_main(void *arg)
{
    (void)arg;
    if (S.cfg.server_pin) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET((int)((S.N * S.cfg.threads_per_node + S.node) % ncpu), &cs);
        pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    }
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    while (!atomic_load_explicit(&S.stop, memory_order_relaxed)) {
        int got = 0;
        S.st.rounds++;
        /* At most one message per source per round: round-robin fairness. */
        for (int src = 0; src < S.N; src++) {
            uint64_t pos = S.head[src];
            uint64_t w = ncl_uc_load(q_slot(src, S.node, pos));
            if ((w >> M_POS_SHIFT) == (pos & 0xffff) && ((w >> M_TYPE_SHIFT) & 3) != 0) {
                S.head[src] = pos + 1;
                process(src, w);
                got = 1;
            }
        }
        if (S.ntrying) {
            poll_trying();
            got = 1; /* waiting out a fast-path holder: keep polling promptly */
        }
        if (got) {
            ncl_backoff_init(&bo);
        } else {
            S.st.idle_rounds++;
            ncl_backoff_capped(&bo, S.backoff_cap); /* adaptive: back off while idle */
            if (bo.cur >= S.backoff_cap)
                sched_yield(); /* fully backed off: don't hog an oversubscribed CPU */
        }
    }
    return NULL;
}

int ncl_srv_node_start(const struct ncl_sys_cfg *c, const struct ncl_srv_layout *L, int node,
                       ncl_line_t *blocks, size_t lines_per_lock)
{
    memset(&S, 0, sizeof S);
    S.cfg = *c;
    S.L = *L;
    S.node = node;
    S.N = c->nnodes;
    S.fast = c->alg == NCL_ALG_FASTSRV;
    S.backoff_cap = c->server_backoff_cap ? c->server_backoff_cap : 1024;

    S.local_idx = malloc(sizeof(int) * (size_t)c->nlocks);
    if (!S.local_idx)
        return -1;
    int nh = 0;
    for (int l = 0; l < c->nlocks; l++)
        S.local_idx[l] = ncl_srv_home(l, S.N) == node ? nh++ : -1;
    S.slocks = calloc((size_t)(nh ? nh : 1), sizeof *S.slocks);
    S.trying = calloc((size_t)(nh ? nh : 1), sizeof *S.trying);
    if (!S.slocks || !S.trying)
        return -1;
    for (int l = 0; l < c->nlocks; l++) {
        if (S.local_idx[l] < 0)
            continue;
        struct srv_lock *sl = &S.slocks[S.local_idx[l]];
        sl->lock = (uint32_t)l;
        if (S.fast)
            ncl_base_bind(&sl->b, c->alg, S.N, blocks + (size_t)l * lines_per_lock);
    }
    if (pthread_create(&S.tid, NULL, server_main, NULL) != 0)
        return -1;
    S.running = 1;
    return 0;
}

void ncl_srv_node_stop(void)
{
    if (!S.running)
        return;
    atomic_store(&S.stop, 1);
    pthread_join(S.tid, NULL);
    S.running = 0;
    free(S.local_idx);
    free(S.slocks);
    free(S.trying);
    S.local_idx = NULL;
    S.slocks = NULL;
    S.trying = NULL;
}

/* ---- client side ---------------------------------------------------------- */

void ncl_srv_acquire(struct ncl_thread *t, int lock)
{
    int h = ncl_srv_home(lock, S.N);
    uint32_t g = (uint32_t)t->local_idx;
    uint32_t reqid = (++t->reqid) & MASK(NCL_SRV_REQID_BITS);
    if (!reqid)
        reqid = ++t->reqid & MASK(NCL_SRV_REQID_BITS);
    uint64_t want = reply_encode((uint32_t)lock, REPLY_GRANT, reqid);

    send(h, MSG_REQ, (uint32_t)lock, g, reqid);
    ncl_line_t *gs = grant_slot(S.node, g);
    unsigned spins = 0;
    while (ncl_uc_load(gs) != want) /* stale replies never match */
        ncl_spin_wait(&spins); /* the server thread must run to grant */
    ncl_fence();
}

void ncl_srv_release(int lock)
{
    send(ncl_srv_home(lock, S.N), MSG_REL, (uint32_t)lock, 0, 0);
}

int ncl_fastsrv_acquire(struct ncl_thread *t, int lock, struct ncl_base *b,
                        struct ncl_base_stats *st)
{
    if (ncl_splitter_enter(b, S.node, st)) {
        ncl_top_lock(b, NCL_SIDE_FAST); /* server not involved */
        return NCL_PATH_FASTSRV_FAST;
    }
    /* Deflected: present[node] stays set until release (design §4e). */
    ncl_srv_acquire(t, lock);
    return NCL_PATH_FASTSRV_SLOW;
}

void ncl_fastsrv_release(int lock, struct ncl_base *b, int path, struct ncl_base_stats *st)
{
    ncl_fence();
    if (path == NCL_PATH_FASTSRV_FAST) {
        ncl_top_unlock(b, NCL_SIDE_FAST);
        ncl_fence();
        ncl_splitter_open_unless_others(b, S.node, st); /* fairness rule */
        ncl_fence_ss();
        ncl_present_clear(b, S.node);
    } else {
        /* Clear present[] BEFORE the release message, so the server's reset
         * check (run when this release empties its queue) can see it clear. */
        ncl_present_clear(b, S.node);
        ncl_srv_release(lock); /* send() fences before publishing */
    }
}

void ncl_srv_stats(struct ncl_srv_stats *out) { *out = S.st; }
