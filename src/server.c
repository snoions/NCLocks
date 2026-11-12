#define _GNU_SOURCE
#include "server.h"
#include "ncl/cpus.h"

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

/* MODE_HANDOFF (chain) extras in the reply word, above the bits a waiter matches. */
#define R_MATCH_MASK MASK(36)
#define R_SEQ_SHIFT 36
#define R_HINT (1ull << 50)   /* holder should read succ[] at release */
#define R_PASSED (1ull << 51) /* written by the previous holder, not the server */
#define SEQ_MASK MASK(NCL_SRV_REQID_BITS) /* REL carries seq in the reqid field */

static inline uint64_t chain_reply(uint32_t lock, uint32_t reqid, uint32_t seq, int hint, int passed)
{
    return reply_encode(lock, REPLY_GRANT, reqid) | ((uint64_t)(seq & SEQ_MASK) << R_SEQ_SHIFT) |
           (hint ? R_HINT : 0) | (passed ? R_PASSED : 0);
}

/* succ[] word, written only by the lock's home server:
 *   [valid 63][seq 40..53][node 26..31][gslot 14..25][reqid 0..13] */
#define S_VALID (1ull << 63)
#define S_SEQ_SHIFT 40
#define S_NODE_SHIFT 26

static inline uint64_t succ_encode(uint32_t seq, unsigned node, uint32_t gslot, uint32_t reqid)
{
    return S_VALID | ((uint64_t)(seq & SEQ_MASK) << S_SEQ_SHIFT) | ((uint64_t)node << S_NODE_SHIFT) |
           ((uint64_t)gslot << M_GSLOT_SHIFT) | (reqid & MASK(NCL_SRV_REQID_BITS));
}

uint32_t ncl_srv_chain_lines(const struct ncl_sys_cfg *c)
{
    if (c->chain_lines)
        return (uint32_t)c->chain_lines; /* validated by ncl_sys_create */
    uint32_t r = 2;
    while (r < 4u * (uint32_t)c->nnodes)
        r <<= 1;
    return r; /* power of two, divides 2^14, so (seq & SEQ_MASK) % R == seq % R */
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

size_t ncl_srv_layout(const struct ncl_sys_cfg *c, struct ncl_srv_layout *L, ncl_off_t base_off,
                      ncl_off_t scan_off, size_t *scan_bytes)
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

    ncl_off_t off = scan_off; /* queue rings: read by a batched poll, scan region */
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
    *scan_bytes = (size_t)(off - scan_off);
    off = base_off; /* grant slots: polled one word at a time, meta region */
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
    /* MODE_HANDOFF (chain): grant numbers cur..next-1 are assigned, asg[n % R].
     * cur = the number the server last granted itself, or the one after the
     * last REL; holders may have passed beyond it without telling us. */
    uint32_t cur, next;
    struct waiter asg[NCL_SRV_CHAIN_MAX];
    ncl_line_t *succ; /* succ[R] uncached lines */
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
    int ak;        /* AKSRV: AK fast path; the server runs AK's reset at drain */
    int chain;     /* MODE_HANDOFF for SERVER: holder-to-waiter passing via succ[] */
    uint32_t R;    /* chain: succ[] lines per lock = assignment window */
    ncl_line_t *blocks;
    size_t lines_per_lock;
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
    ncl_line_t *base = (ncl_line_t *)ncl_scan_ptr(S.L.q_off[h]);
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
    } else if (S.fast && !sl->held && !sl->qn && sl->slow == SLOW_HOLDING && S.ak) {
        /* AKSRV: AK's reset (29-37) while still holding the SLOW side, which
         * excludes the fast holder's 13-18 as AK's critical section does. */
        ncl_fence();
        ncl_aksrv_server_reset(&sl->b, &S.st.base);
        ncl_fence();
        ncl_top_unlock(&sl->b, NCL_SIDE_SLOW);
        sl->slow = SLOW_NONE;
    } else if (S.fast && !sl->held && !sl->qn && sl->slow == SLOW_HOLDING) {
        ncl_fence();
        ncl_top_unlock(&sl->b, NCL_SIDE_SLOW);
        sl->slow = SLOW_NONE;
        ncl_fence();
        /* Releasing grantee cleared present[] before its REL, so check all
         * node slots; the server is visible in its marker slot meanwhile. */
        ncl_splitter_server_reset(&sl->b, &S.st.base);
    }
}

/* ---- chain mode (MODE_HANDOFF for SERVER) ----------------------------------
 * Each REQ gets a grant number n (FIFO order). If n is not the first, the
 * server publishes succ[(n-1) % R] = (n-1, waiter n): "holder of n-1, pass to
 * this waiter". The holder of k reads succ[k % R] once at release; tag k ->
 * it stores the grant (k+1) into that waiter's grant slot itself, no message.
 * Otherwise it sends REL(k), and the server grants k+1 (if assigned). Each
 * holder decides exactly once and the server grants k+1 only after REL(k),
 * so k+1 is granted exactly once. The server never learns about passes, so
 * at most R numbers are assigned past `cur`; a full window holds REQs back,
 * which ends the chain within R handoffs and resynchronizes the server.
 * Numbers below `cur` are finished, so slot n % R may be reused for n. */
static ncl_line_t *succ_line(struct srv_lock *sl, uint32_t n) { return sl->succ + (n & (S.R - 1)); }

static void chain_grant(struct srv_lock *sl)
{
    struct waiter wt = sl->asg[sl->cur & (S.R - 1)];
    int hint = sl->next - sl->cur > 1; /* succ[cur] already published */
    ncl_fence(); /* REL / succ publication before the grant */
    ncl_uc_store(grant_slot(wt.node, wt.gslot), chain_reply(sl->lock, wt.reqid, sl->cur, hint, 0));
    S.st.grants++;
}

static void chain_advance(struct srv_lock *sl, int grant)
{
    while (sl->qn && sl->next - sl->cur < S.R) {
        struct waiter wt = sl->q[sl->qh];
        sl->qh = (sl->qh + 1) % S.N;
        sl->qn--;
        uint32_t n = sl->next++;
        sl->asg[n & (S.R - 1)] = wt;
        /* Clear n's own slot: it still holds an entry for n - R (done, since
         * n - R < cur). Without this a slot the chain never refills keeps its
         * entry until the 14-bit tag wraps and matches again: a pass to a long
         * finished request, which loses the lock. */
        ncl_uc_store(succ_line(sl, n), 0);
        if (n != sl->cur) {
            ncl_fence();
            ncl_uc_store(succ_line(sl, n - 1), succ_encode(n - 1, wt.node, wt.gslot, wt.reqid));
            S.st.published++;
        } else if (!sl->held) {
            sl->held = 1;
            grant = 1;
        }
    }
    if (sl->qn)
        S.st.window_full++;
    if (grant)
        chain_grant(sl);
}

static void chain_process(int src, unsigned type, struct srv_lock *sl, uint32_t gslot, uint32_t reqid)
{
    if (type == MSG_REQ) {
        if (sl->qn >= S.N) {
            S.st.errors++;
            return;
        }
        sl->q[(sl->qh + sl->qn) % S.N] =
            (struct waiter){(uint8_t)src, (uint16_t)gslot, (uint16_t)reqid};
        sl->qn++;
        if (sl->held)
            S.st.queued++;
        chain_advance(sl, 0);
    } else if (type == MSG_REL) {
        /* REL(k): k in [cur, next) and assigned to src. */
        uint32_t k = sl->cur + ((reqid - sl->cur) & SEQ_MASK);
        if (!sl->held || k - sl->cur >= sl->next - sl->cur ||
            sl->asg[k & (S.R - 1)].node != src) {
            S.st.errors++;
            return;
        }
        sl->cur = k + 1;
        int grant = sl->cur != sl->next; /* k+1 assigned but not passed to */
        if (grant)
            S.st.rel_grants++;
        else
            sl->held = 0;
        chain_advance(sl, grant);
    } else {
        S.st.errors++;
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
    if (S.chain) {
        chain_process(src, type, sl, gslot, reqid);
        return;
    }
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
        int cpu = ncl_cpu_slot(S.N * S.cfg.threads_per_node + S.node);
        if (S.cfg.server_pin == NCL_SRV_PIN_SIBLING) {
            int w = ncl_cpu_slot(S.node * S.cfg.threads_per_node);
            int sib = ncl_smt_sibling(w);
            if (sib < 0)
                fprintf(stderr, "ncl: server %d: CPU %d has no SMT sibling; using CPU %d\n",
                        S.node, w, cpu);
            else
                cpu = sib;
        }
        ncl_pin_self(cpu);
    }
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    while (!atomic_load_explicit(&S.stop, memory_order_relaxed)) {
        int got = 0;
        S.st.rounds++;
        /* At most one message per source per round: round-robin fairness. */
        /* One poll round over the expected slot of every source queue.
         * Batched (cached queue lines: cached-flush, or the split layout's
         * scan region): all N slots in one batch, then process.
         * Serial (uc etc.): load and process one at a time, so a message is
         * acted on without waiting for the remaining N-1 uncached loads
         * (loading all first cost ~30% capacity on uc at N=8). */
        int batch = ncl_meta_batching_for(q_slot(0, S.node, 0));
        ncl_line_t *slots[NCL_MAX_NODES];
        uint64_t words[NCL_MAX_NODES];
        if (batch) {
            for (int src = 0; src < S.N; src++)
                slots[src] = q_slot(src, S.node, S.head[src]);
            ncl_uc_load_many(slots, S.N, words);
        }
        for (int src = 0; src < S.N; src++) {
            uint64_t pos = S.head[src];
            uint64_t w = batch ? words[src] : ncl_uc_load(q_slot(src, S.node, pos));
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
                       ncl_line_t *blocks, size_t lines_per_lock, ncl_line_t *sblocks,
                       size_t scan_per_lock)
{
    memset(&S, 0, sizeof S);
    S.cfg = *c;
    S.L = *L;
    S.node = node;
    S.N = c->nnodes;
    S.fast = ncl_alg_fastsrv_like(c->alg);
    S.ak = c->alg == NCL_ALG_AKSRV;
    S.chain = c->alg == NCL_ALG_SERVER && c->mode == NCL_MODE_HANDOFF;
    S.R = ncl_srv_chain_lines(c);
    S.blocks = blocks;
    S.lines_per_lock = lines_per_lock;
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
            ncl_base_bind(&sl->b, c->alg, S.N, blocks + (size_t)l * lines_per_lock,
                          sblocks + (size_t)l * scan_per_lock);
        if (S.chain)
            sl->succ = blocks + (size_t)l * lines_per_lock; /* SERVER has no base lines */
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

uint64_t ncl_srv_acquire(struct ncl_thread *t, int lock)
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
    uint64_t w;
    while (((w = ncl_uc_load(gs)) & R_MATCH_MASK) != want) /* stale replies never match */
        ncl_spin_wait(&spins); /* the server (or, chain mode, the holder) must run to grant */
    ncl_fence();
    return w;
}

void ncl_srv_release(int lock)
{
    send(ncl_srv_home(lock, S.N), MSG_REL, (uint32_t)lock, 0, 0);
}

int ncl_srv_grant_passed(uint64_t grant) { return (grant & R_PASSED) != 0; }

int ncl_srv_chain_release(int lock, uint64_t grant, int always_read, struct ncl_handoff_stats *h)
{
    uint32_t seq = (uint32_t)(grant >> R_SEQ_SHIFT) & SEQ_MASK;
    if (always_read || (grant & R_HINT)) {
        ncl_line_t *succ = S.blocks + (size_t)lock * S.lines_per_lock;
        uint64_t s = ncl_uc_load(succ + (seq & (S.R - 1)));
        h->announce_loads++;
        if ((s & S_VALID) && ((uint32_t)(s >> S_SEQ_SHIFT) & SEQ_MASK) == seq) {
            unsigned node = (unsigned)(s >> S_NODE_SHIFT) & 63;
            uint32_t gslot = (uint32_t)(s >> M_GSLOT_SHIFT) & MASK(NCL_SRV_GSLOT_BITS);
            uint32_t reqid = (uint32_t)s & MASK(NCL_SRV_REQID_BITS);
            if (node >= (unsigned)S.N || gslot >= S.L.grants_per_node) {
                fprintf(stderr, "ncl: corrupt succ word %#llx\n", (unsigned long long)s);
                abort();
            }
            ncl_fence(); /* data writeback before the grant */
            ncl_uc_store(grant_slot((int)node, gslot),
                         chain_reply((uint32_t)lock, reqid, seq + 1, 1, 1));
            h->handoffs_sent++;
            return 1;
        }
        h->missed_announce++; /* read, but no successor for us */
    }
    h->frees++;
    send(ncl_srv_home(lock, S.N), MSG_REL, (uint32_t)lock, 0, seq);
    return 0;
}

int ncl_fastsrv_acquire(struct ncl_thread *t, int lock, struct ncl_base *b,
                        struct ncl_base_stats *st)
{
    if (S.ak ? ncl_aksrv_try_fast(b, S.node, st) : ncl_splitter_enter(b, S.node, st)) {
        if (!S.ak)
            ncl_top_lock(b, NCL_SIDE_FAST); /* server not involved */
        return NCL_PATH_FASTSRV_FAST;
    }
    /* Deflected: present[node] stays set until release (design §4e). */
    ncl_srv_acquire(t, lock);
    return NCL_PATH_FASTSRV_SLOW;
}

void ncl_fastsrv_release(int lock, struct ncl_base *b, int path, struct ncl_base_stats *st)
{
    if (S.ak) {
        if (path == NCL_PATH_FASTSRV_FAST) {
            ncl_aksrv_fast_release(b, S.node, st);
        } else {
            ncl_fence();
            ncl_aksrv_slow_release(b, S.node); /* Obstacle[p] := false, in the CS */
            ncl_srv_release(lock);             /* send() fences before publishing */
        }
        return;
    }
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
