#include "ncl/base.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

unsigned ncl_backoff_cap = 1024;
int ncl_fast_release = NCL_FASTREL_SCAN; /* hint/always are UNSAFE, see base.h */
uint64_t ncl_debug_reset_delay = 0;

const char *const ncl_path_names[NCL_PATH__COUNT] = {
    [NCL_PATH_LAMPORT_FAST] = "lamport_fast",
    [NCL_PATH_LAMPORT_SLOW] = "lamport_slow",
    [NCL_PATH_TOURNAMENT] = "tournament",
    [NCL_PATH_HYBRID_FAST] = "hybrid_fast",
    [NCL_PATH_HYBRID_SLOW] = "hybrid_slow",
    [NCL_PATH_SERVER] = "server",
    [NCL_PATH_FASTSRV_FAST] = "fastsrv_fast",
    [NCL_PATH_FASTSRV_SLOW] = "fastsrv_slow",
    [NCL_PATH_LOCAL_HANDOFF] = "local_handoff",
    [NCL_PATH_GLOBAL_HANDOFF] = "global_handoff",
    [NCL_PATH_PTHREAD] = "pthread",
    [NCL_PATH_BAKERY_FAST] = "bakery_fast",
    [NCL_PATH_BAKERY_WAIT] = "bakery_wait",
};

static int pow2ceil(int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

static size_t lam_lines(int P) { (void)P; return 2; }   /* x, y; b[] is in scan */
static size_t split_lines(int P) { (void)P; return 2; } /* race, door; present[] in scan */
static size_t tree_lines(int P) { return 3 * (size_t)(pow2ceil(P) - 1); }
static size_t top_lines(void) { return 3; }
static size_t ak_lines(int P) { return 4 + 2 * (size_t)P; }

int ncl_bakery_arity = 0;

/* BAKERY tree (arity k; k < 2 or k >= P: one flat node): level l has cnt[l]
 * entities (cnt[0] = P participants, cnt[l+1] = ceil(cnt[l] / k)); its nodes
 * hold the cnt[l] seats in groups of k, so a node's width is the number of
 * children that exist. Levels stop when one entity is left (the holder).
 * Returns the slot lines (sum of cnt[l] over the levels). */
static size_t bak_tree_lines(int P, int k, int *levels, int *cnt)
{
    if (k < 2 || k >= P)
        k = P;
    size_t lines = 0;
    int n = P, L = 0;
    while (n > 1) {
        if (cnt)
            cnt[L] = n;
        lines += (size_t)n;
        L++;
        n = (n + k - 1) / k;
    }
    if (P == 1) { /* one participant: one node of width 1 */
        if (cnt)
            cnt[0] = 1;
        lines = 1;
        L = 1;
    }
    if (levels)
        *levels = L;
    return lines;
}

size_t ncl_base_nlines(ncl_alg_t alg, int P)
{
    switch (alg) {
    case NCL_ALG_LAMPORT: return lam_lines(P);
    case NCL_ALG_TOURNAMENT:
        return (ncl_tour_hot_probe ? 1 : 0) + (ncl_hot_k ? 4 * (size_t)ncl_hot_k + 1 : 0) +
               (tree_lines(P) ? tree_lines(P) : 1);
    case NCL_ALG_HYBRID: return split_lines(P) + tree_lines(P) + top_lines();
    case NCL_ALG_SERVER: return 0;
    case NCL_ALG_FASTSRV: return split_lines(P) + top_lines();
    case NCL_ALG_AK: return ak_lines(P) + tree_lines(P) + top_lines();
    case NCL_ALG_AKSRV: return ak_lines(P) + top_lines();
    case NCL_ALG_PTHREAD: return 1;
    case NCL_ALG_REACTIVE: return 1 + lam_lines(P) + (tree_lines(P) ? tree_lines(P) : 1);
    case NCL_ALG_BAKERY: return 0; /* slots are in scan */
    }
    return 0;
}

size_t ncl_base_nscan(ncl_alg_t alg, int P)
{
    switch (alg) {
    case NCL_ALG_LAMPORT:            /* b[] */
    case NCL_ALG_REACTIVE:           /* Lamport's b[] */
    case NCL_ALG_HYBRID: return (size_t)P; /* present[] */
    case NCL_ALG_BAKERY: return bak_tree_lines(P, ncl_bakery_arity, NULL, NULL);
    case NCL_ALG_FASTSRV: return (size_t)P + 1; /* present[] + server reset marker */
    case NCL_ALG_TOURNAMENT:
    case NCL_ALG_SERVER:
    case NCL_ALG_AK:
    case NCL_ALG_AKSRV:
    case NCL_ALG_PTHREAD: return 0;
    }
    return 0;
}

void ncl_base_bind(struct ncl_base *b, ncl_alg_t alg, int P, ncl_line_t *lines, ncl_line_t *scan)
{
    memset(b, 0, sizeof *b);
    b->alg = alg;
    b->P = P;
    b->leaves = pow2ceil(P);
    b->reset_hint = -1;
    switch (alg) {
    case NCL_ALG_LAMPORT:
        b->lam = lines;
        b->lam_b = scan;
        break;
    case NCL_ALG_TOURNAMENT: /* [hot_mode (ncl_tour_hot_probe)][tree], or (ncl_hot_k)
                                [hot_mode][hot_owner[k]][R][H][tree] */
        b->mode = NULL;
        b->hot_mode = NULL;
        atomic_store_explicit(&b->my_slot, -1, memory_order_relaxed);
        if (ncl_tour_hot_probe) {
            b->mode = lines;
            b->tree = lines + 1;
        } else if (ncl_hot_k) {
            b->hot_mode = lines;
            b->hot_owner = lines + 1;
            b->hot_r = b->hot_owner + ncl_hot_k;
            b->hot_tree = b->hot_r + 3;
            b->tree = b->hot_tree + 3 * (ncl_hot_k - 1);
        } else {
            b->tree = lines;
        }
        break;
    case NCL_ALG_HYBRID:
        b->split = lines;
        b->present = scan;
        b->npresent = P;
        b->tree = lines + split_lines(P);
        b->top = b->tree + tree_lines(P);
        break;
    case NCL_ALG_SERVER:
        break;
    case NCL_ALG_FASTSRV:
        b->split = lines;
        b->present = scan;
        b->npresent = P + 1;
        b->top = lines + split_lines(P);
        break;
    case NCL_ALG_AK:
        b->ak = lines;
        b->tree = lines + ak_lines(P);
        b->top = b->tree + tree_lines(P);
        break;
    case NCL_ALG_AKSRV:
        b->ak = lines;
        b->top = lines + ak_lines(P);
        break;
    case NCL_ALG_PTHREAD:
        b->mtx = (pthread_mutex_t *)(void *)lines;
        break;
    case NCL_ALG_BAKERY:
        b->bak = scan;
        bak_tree_lines(P, ncl_bakery_arity, &b->bak_levels, b->bak_cnt);
        b->bak_k = ncl_bakery_arity >= 2 && ncl_bakery_arity < P ? ncl_bakery_arity : P;
        break;
    case NCL_ALG_REACTIVE: /* [mode][x, y][tree] or (ncl_react_layout) [x, y][mode][tree]; b[] in scan */
        if (ncl_react_layout) {
            b->lam = lines;
            b->mode = lines + lam_lines(P);
        } else {
            b->mode = lines;
            b->lam = lines + 1;
        }
        b->lam_b = scan;
        b->tree = lines + 1 + lam_lines(P);
        /* Start from the current mode (set by the creator, or 0). */
        b->rx_guess = b->rx_lastm = ncl_uc_load(b->mode) == 1;
        break;
    }
}

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(ncl_line_t), "pthread_mutex_t must fit one line");

int ncl_base_init_shared(ncl_alg_t alg, ncl_line_t *lines)
{
    if (alg != NCL_ALG_PTHREAD)
        return 0;
    pthread_mutexattr_t a;
    int rc = pthread_mutexattr_init(&a);
    if (!rc)
        rc = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (!rc)
        rc = pthread_mutex_init((pthread_mutex_t *)(void *)lines, &a);
    pthread_mutexattr_destroy(&a);
    if (rc)
        fprintf(stderr, "ncl: pthread_mutex_init: %s\n", strerror(rc));
    return rc ? -1 : 0;
}

static void await_eq(ncl_line_t *l, uint64_t v)
{
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    while (ncl_uc_load(l) != v)
        ncl_backoff(&bo);
}

/* ---- (a) Lamport's fast mutex ------------------------------------------ */

#define LAM_X(b) (&(b)->lam[0])
#define LAM_Y(b) (&(b)->lam[1])
#define LAM_B(b, i) (&(b)->lam_b[(i)])
/* REACTIVE: y while the tournament is active (never a participant + 1). */
#define RX_INVALID (~(uint64_t)0)

/* Wait until y == 0 (returns 0) or y == RX_INVALID (returns 1). */
static int await_y_free(struct ncl_base *b)
{
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    for (;;) {
        uint64_t v = ncl_uc_load(LAM_Y(b));
        if (v == 0)
            return 0;
        if (v == RX_INVALID)
            return 1;
        ncl_backoff(&bo);
    }
}

uint64_t ncl_debug_lamport_delay = 0, ncl_debug_mode_delay = 0;
int ncl_debug_react = 0;

/* Returns the path, or -1 if the Lamport lock is RX_INVALID (REACTIVE: the
 * tournament is active; we hold nothing and b[i] is 0). Plain Lamport never
 * sees it. takeover (REACTIVE, a tournament holder switching back): treat
 * RX_INVALID as free.
 * A late y store can overwrite RX_INVALID: a node reads y == 0, stalls, and
 * stores y = me after another node acquired, switched to the tournament and
 * released y = RX_INVALID. That node wrote x after ours, so the late node
 * always takes the slow path; a slow-path success therefore re-reads `mode`
 * and, if the tournament is active, restores RX_INVALID (it holds the Lamport
 * lock by Lamport's rules, so nobody else writes y meanwhile) and gives up. A
 * fast-path success needs x unchanged since our store, so no switch can have
 * happened in between. */
static int lamport_lock_x(struct ncl_base *b, int i, struct ncl_base_stats *st, int takeover)
{
    const uint64_t me = (uint64_t)i + 1; /* 0 means empty */
    int slow = 0;
start:
    ncl_uc_store(LAM_B(b, i), 1);
    ncl_fence_ss();
    ncl_uc_store(LAM_X(b), me);
    ncl_fence(); /* (1) x = me before reading y */
    uint64_t y = ncl_uc_load(LAM_Y(b));
    if (y != 0 && !(takeover && y == RX_INVALID)) {
        ncl_uc_store(LAM_B(b, i), 0);
        if ((y == RX_INVALID || await_y_free(b)) && !takeover)
            return -1;
        NCL_STAT(st->lamport_retries++);
        st->contended++;
        goto start;
    }
    if (NCL_UNLIKELY(ncl_debug_lamport_delay))
        ncl_lat_spin_cycles(ncl_debug_lamport_delay); /* widen the late-y-store window */
    ncl_uc_store(LAM_Y(b), me);
    ncl_fence(); /* (2) y = me before reading x */
    if (ncl_uc_load(LAM_X(b)) != me) {
        slow = 1;
        st->contended++;
        ncl_uc_store(LAM_B(b, i), 0);
        ncl_fence(); /* b[i] = 0 before scanning b[] */
        /* Wait for every b[j] == 0, in index order. A batched first pass
         * reads all slots at once; only nonzero ones are then awaited. */
        uint64_t nl = 0;
        for (int j = 0; (j = ncl_uc_scan_first(LAM_B(b, 0), b->P, j, b->P - j, -1, &nl)) >= 0; j++)
            await_eq(LAM_B(b, j), 0);
        if (ncl_uc_load(LAM_Y(b)) != me) {
            if (await_y_free(b) && !takeover)
                return -1;
            NCL_STAT(st->lamport_retries++);
            goto start;
        }
        if (b->alg == NCL_ALG_REACTIVE && !takeover)
            NCL_STAT(st->react_slow_mode_reads++);
        if (b->alg == NCL_ALG_REACTIVE && !takeover && !(ncl_debug_react & 16) &&
            ncl_uc_load(b->mode) == 1) {
            /* Our y overwrote RX_INVALID: restore it and go to the tournament. */
            ncl_fence();
            ncl_uc_store(LAM_Y(b), RX_INVALID);
            ncl_fence();
            return -1;
        }
    }
    ncl_fence(); /* lock acquired before critical-section accesses */
    return slow ? NCL_PATH_LAMPORT_SLOW : NCL_PATH_LAMPORT_FAST;
}

static int lamport_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    return lamport_lock_x(b, i, st, 0);
}

static void lamport_unlock(struct ncl_base *b, int i)
{
    ncl_fence(); /* critical-section effects (incl. data writeback) first */
    ncl_uc_store(LAM_Y(b), 0);
    ncl_fence_ss();
    ncl_uc_store(LAM_B(b, i), 0);
}

/* ---- (b) Tournament of Peterson locks ---------------------------------- */

/* Internal node k (1..leaves-1): [flag0, flag1, turn]. */
#define T_FLAG(b, k, s) (&(b)->tree[3 * ((k) - 1) + (s)])
#define T_TURN(b, k) (&(b)->tree[3 * ((k) - 1) + 2])

static void peterson_announce(ncl_line_t *flag_me, ncl_line_t *turn, uint64_t side)
{
    ncl_uc_store(flag_me, 1);
    ncl_fence_ss();
    ncl_uc_store(turn, side); /* "victim" convention */
    ncl_fence();
}

static int peterson_may_enter(ncl_line_t *flag_other, ncl_line_t *turn, uint64_t side)
{
    if (ncl_meta_batching_for(flag_other)) { /* both reads in one batch; either order is safe */
        ncl_line_t *ls[2] = {flag_other, turn};
        uint64_t v[2];
        ncl_uc_load_many(ls, 2, v);
        return !(v[0] && v[1] == side);
    }
    return !(ncl_uc_load(flag_other) && ncl_uc_load(turn) == side);
}

/* Returns 1 if it had to wait (the other side was competing). */
static int peterson_lock(ncl_line_t *flag_me, ncl_line_t *flag_other, ncl_line_t *turn,
                         uint64_t side)
{
    peterson_announce(flag_me, turn, side);
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    int waited = 0;
    while (!peterson_may_enter(flag_other, turn, side)) {
        waited = 1;
        ncl_backoff(&bo);
    }
    return waited;
}

static void tournament_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    for (int pos = b->leaves + i; pos > 1; pos >>= 1) {
        int k = pos >> 1, s = pos & 1;
        if (peterson_lock(T_FLAG(b, k, s), T_FLAG(b, k, 1 - s), T_TURN(b, k), (uint64_t)s) && st)
            st->contended++;
    }
    ncl_fence();
}

/* Release root to leaf: releasing a lower level first would let a sibling
 * climb onto a side we still occupy higher up. Store-store ordered. */
static void tournament_unlock_nofence(struct ncl_base *b, int i)
{
    int path[32], depth = 0;
    for (int pos = b->leaves + i; pos > 1; pos >>= 1)
        path[depth++] = pos;
    for (int d = depth - 1; d >= 0; d--) {
        int pos = path[d];
        ncl_uc_store(T_FLAG(b, pos >> 1, pos & 1), 0);
        if (d)
            ncl_fence_ss();
    }
}

/* ---- (b) with prefix retention (design §4g g1) ---------------------------
 * Level l (1 = leaf, depth = root) of participant i: position pos = (leaves +
 * i) >> (l - 1) in the heap-numbered tree; node pos >> 1, side pos & 1. */

int ncl_retain = 0;
int ncl_retain_check = NCL_RETAIN_CHECK_ACQUIRE; /* measured on CXL: the release check costs more than it saves */
int ncl_retain_max = 0;
int ncl_retain_probe = 16;
int ncl_tour_hot_probe;
int ncl_hot_k, ncl_hot_window_us = 1000, ncl_hot_lease_us = 1000, ncl_hot_admit_always;
uint64_t ncl_hot_window_cycles, ncl_hot_lease_cycles;
_Atomic int ncl_hot_held;
int ncl_debug_hot;
uint64_t ncl_debug_hot_delay;
_Atomic int ncl_retained_locks;
int ncl_debug_retain = 0;
uint64_t ncl_debug_retain_delay = 0;

int ncl_tour_depth(const struct ncl_base *b) { return __builtin_ctz((unsigned)b->leaves); }

static int lvl_pos(const struct ncl_base *b, int i, int l) { return (b->leaves + i) >> (l - 1); }
static ncl_line_t *lvl_flag_me(const struct ncl_base *b, int i, int l)
{
    int pos = lvl_pos(b, i, l);
    return T_FLAG(b, pos >> 1, pos & 1);
}
static ncl_line_t *lvl_flag_other(const struct ncl_base *b, int i, int l)
{
    int pos = lvl_pos(b, i, l);
    return T_FLAG(b, pos >> 1, 1 - (pos & 1));
}

void ncl_tour_kept_lines(const struct ncl_base *b, int i, int kept, ncl_line_t **out)
{
    for (int l = 1; l <= kept; l++)
        out[l - 1] = lvl_flag_other(b, i, l);
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void set_kept(struct ncl_base *b, int kept)
{
    int old = atomic_load_explicit(&b->kept_levels, memory_order_relaxed);
    if (!old && kept)
        atomic_fetch_add_explicit(&ncl_retained_locks, 1, memory_order_relaxed);
    else if (old && !kept)
        atomic_fetch_sub_explicit(&ncl_retained_locks, 1, memory_order_relaxed);
    atomic_store_explicit(&b->kept_levels, kept, memory_order_relaxed);
}

/* Release levels hi down to lo (hi >= lo), root side first. A store-store fence
 * separates levels: a sibling must not win a lower level while we still occupy
 * its side higher up. */
static void drop_levels(struct ncl_base *b, int i, int hi, int lo)
{
    for (int l = hi; l >= lo; l--) {
        ncl_uc_store(lvl_flag_me(b, i, l), 0);
        if (l > lo)
            ncl_fence_ss();
    }
}

/* Learned keep height (holding the submutex). keep_need == 0: not set up. */
static void keep_setup(struct ncl_base *b, int depth)
{
    if (!b->keep_need) {
        b->keep_cap = depth - 1;
        b->keep_need = ncl_retain_probe;
        b->keep_run = 0;
        b->keep_probing = 0;
    }
}

/* A waiter on level w: keep below it, probe again later. */
static void keep_lower(struct ncl_base *b, int w, struct ncl_base_stats *st)
{
    if (ncl_retain_probe <= 0 || !b->keep_need || w < 1)
        return;
    if (w - 1 < b->keep_cap) {
        b->keep_cap = w - 1;
        if (st)
            NCL_STAT(st->ret_cap_lowers++);
    }
    if (b->keep_need < NCL_RETAIN_PROBE_MAX)
        b->keep_need *= 2;
    b->keep_run = 0;
    b->keep_probing = 0;
}

/* At every global release: the cap for this release. */
static int keep_tick(struct ncl_base *b, int depth, struct ncl_base_stats *st)
{
    if (ncl_retain_probe <= 0)
        return depth - 1;
    keep_setup(b, depth);
    if (++b->keep_run >= b->keep_need) {
        b->keep_run = 0;
        if (b->keep_probing && b->keep_need > ncl_retain_probe)
            b->keep_need /= 2; /* the last probe held for a whole interval */
        b->keep_probing = 0;
        if (b->keep_cap < depth - 1) {
            b->keep_cap++;
            b->keep_probing = 1;
            NCL_STAT(st->ret_cap_raises++);
        }
    }
    return b->keep_cap;
}

void ncl_tour_drop(struct ncl_base *b, int i, int lowest, int waiter, struct ncl_base_stats *st)
{
    int kept = atomic_load_explicit(&b->kept_levels, memory_order_relaxed);
    if (lowest < 1)
        lowest = 1;
    if (kept < lowest)
        return;
    if (waiter)
        keep_lower(b, waiter, st);
    drop_levels(b, i, kept, lowest);
    ncl_fence(); /* the drop is visible before the submutex goes to a climber */
    set_kept(b, lowest - 1);
}

/* Wait at level l (announced) until Peterson lets us in. At C's top with g2
 * (ncl_hot_k), the entry check also reads hot_mode, ordered after the level's
 * flag and turn (ncl_fence_ll; one round trip on cached-flush), exactly as
 * reactive reads `mode` (tournament_lock_mode): the value of the successful
 * check counts. Returns hot_mode (0 elsewhere). */
static uint64_t level_wait(struct ncl_base *b, int i, int l, int depth, int waited,
                           struct ncl_base_stats *st)
{
    const int pos = lvl_pos(b, i, l);
    const uint64_t side = (uint64_t)(pos & 1);
    ncl_line_t *fot = lvl_flag_other(b, i, l), *turn = T_TURN(b, pos >> 1);
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    if (!ncl_hot_k || l != depth) {
        while (!peterson_may_enter(fot, turn, side)) {
            waited = 1;
            ncl_backoff(&bo);
        }
        if (waited && st)
            st->contended++;
        return 0;
    }
    uint64_t stale = 0;
    if (NCL_UNLIKELY(ncl_debug_hot & 8)) {
        /* UNSAFE: hot_mode read before the entry check, so it can predate the
         * release the check observes (a claim in between is missed). */
        stale = ncl_uc_load(b->hot_mode);
        if (ncl_debug_hot_delay)
            ncl_lat_spin_cycles(ncl_debug_hot_delay);
    }
    for (;;) {
        ncl_line_t *ls[3] = {fot, turn, b->hot_mode};
        uint64_t v[3];
        ncl_uc_load_many_ordered(ls, (ncl_debug_hot & 8) ? 2 : 3, v);
        if (!(v[0] && v[1] == side)) {
            if (waited && st)
                st->contended++;
            return (ncl_debug_hot & 8) ? stale : v[2];
        }
        waited = 1;
        ncl_backoff(&bo);
    }
}

/* Climb levels from..depth with plain Peterson. Returns hot_mode as read at C's
 * top (level_wait), 0 if from > depth. */
static uint64_t climb(struct ncl_base *b, int i, int from, int depth, struct ncl_base_stats *st)
{
    uint64_t mode = 0;
    for (int l = from; l <= depth; l++) {
        int pos = lvl_pos(b, i, l);
        peterson_announce(lvl_flag_me(b, i, l), T_TURN(b, pos >> 1), (uint64_t)(pos & 1));
        mode = level_wait(b, i, l, depth, 0, st);
    }
    return mode;
}

/* Returns hot_mode as read at C's top (g2), else 0. */
static uint64_t tournament_lock_retain(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    const int depth = ncl_tour_depth(b);
    uint64_t mode;
    int j = atomic_load_explicit(&b->kept_levels, memory_order_relaxed);
    if (NCL_UNLIKELY(ncl_debug_retain_delay) && j)
        ncl_lat_spin_cycles(ncl_debug_retain_delay); /* widen read-kept -> climb */
    if (!j) {
        mode = climb(b, i, 1, depth, st);
        ncl_fence();
        return mode;
    }
    NCL_STAT(st->ret_hits++);
    NCL_STAT(st->ret_levels_skipped += (uint64_t)j);
    const int l = j + 1, pos = lvl_pos(b, i, l);
    const uint64_t side = (uint64_t)(pos & 1);
    ncl_line_t *fme = lvl_flag_me(b, i, l), *fot = lvl_flag_other(b, i, l), *turn = T_TURN(b, pos >> 1);
    const int top_mode = ncl_hot_k && l == depth && !(ncl_debug_hot & 8);
    peterson_announce(fme, turn, side);
    int ok, waited = 1;
    mode = 0;
    if (ncl_retain_check & NCL_RETAIN_CHECK_ACQUIRE) {
        /* Level l's entry check plus the kept levels' opposing flags, one batch;
         * at C's top with g2 also hot_mode, last, ordered after flag and turn. */
        ncl_line_t *ls[35];
        uint64_t v[35];
        ls[0] = fot;
        ls[1] = turn;
        ncl_tour_kept_lines(b, i, j, ls + 2);
        if (top_mode) {
            ls[j + 2] = b->hot_mode;
            ncl_uc_load_many_ordered(ls, j + 3, v);
        } else {
            ncl_uc_load_many(ls, j + 2, v);
        }
        int w = 0;
        for (int k = 1; k <= j && !w; k++)
            if (v[k + 1])
                w = k;
        if (w) {
            /* A waiter on kept level w: withdraw from l, drop j..w, climb again
             * from w (Peterson's turn lets the waiter go first). */
            NCL_STAT(st->ret_acq_drops++);
            keep_lower(b, w, st);
            ncl_uc_store(fme, 0);
            ncl_fence_ss();
            drop_levels(b, i, j, w);
            ncl_fence();
            set_kept(b, 0);
            mode = climb(b, i, w, depth, st);
            ncl_fence();
            return mode;
        }
        ok = !(v[0] && v[1] == side);
        if (ok && top_mode)
            mode = v[j + 2];
    } else {
        ok = !top_mode && peterson_may_enter(fot, turn, side);
        waited = ok || !top_mode; /* top_mode: the check happens in level_wait */
    }
    if (ok && ncl_hot_k && l == depth && (ncl_debug_hot & 8)) {
        ok = 0; /* DEBUG: take level_wait's stale hot_mode read */
        waited = 0;
    }
    set_kept(b, 0); /* held now: the whole path */
    if (!ok)
        mode = level_wait(b, i, l, depth, waited, st);
    if (l < depth)
        mode = climb(b, i, l + 1, depth, st);
    ncl_fence();
    return mode;
}

static void tournament_unlock_retain(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    const int depth = ncl_tour_depth(b);
    ncl_fence(); /* critical-section effects (incl. data writeback) first */
    if (depth == 0)
        return;
    ncl_uc_store(lvl_flag_me(b, i, depth), 0); /* the root: the lock is free */
    int keep = keep_tick(b, depth, st);
    if (b->hot_keep0) { /* g2: this release claimed a hot slot; C is not used again */
        b->hot_keep0 = 0;
        keep = 0;
    }
    if (!keep && depth > 1)
        NCL_STAT(st->ret_policy_skips++); /* learned: nothing to keep, no release check */
    if (keep && ncl_retain_max &&
        atomic_load_explicit(&ncl_retained_locks, memory_order_relaxed) >= ncl_retain_max) {
        NCL_STAT(st->ret_cap_skips++);
        keep = 0;
    }
    if (keep && (ncl_retain_check & NCL_RETAIN_CHECK_RELEASE)) {
        ncl_line_t *ls[32];
        uint64_t v[32];
        ncl_tour_kept_lines(b, i, keep, ls);
        ncl_uc_load_many(ls, keep, v);
        for (int k = 1; k <= keep; k++)
            if (v[k - 1]) {
                NCL_STAT(st->ret_rel_drops++);
                keep_lower(b, k, st);
                if (NCL_UNLIKELY(ncl_debug_retain & 2)) {
                    /* UNSAFE: drop only the waiter's level, keep the ones above. */
                    ncl_fence_ss();
                    drop_levels(b, i, depth - 1, keep + 1);
                    ncl_fence_ss();
                    ncl_uc_store(lvl_flag_me(b, i, k), 0);
                    set_kept(b, keep);
                    atomic_store_explicit(&b->kept_since, mono_ns(), memory_order_relaxed);
                    return;
                }
                keep = k - 1;
                break;
            }
    }
    if (keep < depth - 1) {
        ncl_fence_ss();
        drop_levels(b, i, depth - 1, keep + 1);
    }
    if (keep)
        atomic_store_explicit(&b->kept_since, mono_ns(), memory_order_relaxed);
    set_kept(b, keep);
}

/* ---- splitter + top lock (shared by (d) and (e)) -------------------------
 * Design §4d. race: 0 = none, else participant+1. door: OPEN = 0 (the
 * zeroed initial state), CLOSED = 1. */

#define SP_RACE(b) (&(b)->split[0])
#define SP_DOOR(b) (&(b)->split[1])
#define SP_PRESENT(b, i) (&(b)->present[(i)])
#define TOP_FLAG(b, s) (&(b)->top[(s)])
#define TOP_TURN(b) (&(b)->top[2])
enum { DOOR_OPEN = 0, DOOR_CLOSED = 1 };

int ncl_splitter_enter(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    const uint64_t me = (uint64_t)i + 1;
    ncl_uc_store(SP_PRESENT(b, i), 1); /* visible before touching the splitter */
    ncl_fence_ss();
    ncl_uc_store(SP_RACE(b), me);
    ncl_fence(); /* (1) */
    if (ncl_uc_load(SP_DOOR(b)) == DOOR_CLOSED) {
        NCL_STAT(st->split_fail_closed++); /* "right" */
        return 0;
    }
    ncl_uc_store(SP_DOOR(b), DOOR_CLOSED);
    ncl_fence(); /* (2) */
    if (ncl_uc_load(SP_RACE(b)) != me) {
        NCL_STAT(st->split_fail_lost++); /* "down" */
        return 0;
    }
    return 1; /* "stop" */
}

void ncl_splitter_open(struct ncl_base *b) { ncl_uc_store(SP_DOOR(b), DOOR_OPEN); }

void ncl_splitter_open_unless_others(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    /* Start at the slot last seen set: under contention one load decides. */
    const int np = b->npresent;
    int start = b->reset_hint >= 0 && b->reset_hint != i ? b->reset_hint : (i + 1) % np;
    if (ncl_fast_release == NCL_FASTREL_ALWAYS) {
        ncl_splitter_open(b);
        NCL_STAT(st->split_resets++);
        return;
    }
    if (ncl_fast_release == NCL_FASTREL_HINT) {
        /* Probe only the remembered slot; a miss reopens even if some other
         * node is present. UNSAFE: that node may be a slow releaser in the
         * middle of its reset check (see base.h). Measurement only. */
        if (start != i) {
            NCL_STAT(st->split_scan_loads++);
            if (ncl_uc_load(SP_PRESENT(b, start))) {
                NCL_STAT(st->split_fast_keep_closed++);
                return;
            }
        }
        ncl_fence();
        ncl_uc_store(SP_DOOR(b), DOOR_OPEN);
        return;
    }
    int j = ncl_uc_scan_first(SP_PRESENT(b, 0), np, start, np, i, &st->split_scan_loads);
    if (j >= 0) {
        b->reset_hint = j;
        NCL_STAT(st->split_fast_keep_closed++);
        return; /* leave CLOSED: someone is on the slow path */
    }
    ncl_fence(); /* scan before the store (load->store order, explicit) */
    ncl_uc_store(SP_DOOR(b), DOOR_OPEN);
}

void ncl_present_clear(struct ncl_base *b, int i) { ncl_uc_store(SP_PRESENT(b, i), 0); }

void ncl_splitter_server_reset(struct ncl_base *b, struct ncl_base_stats *st)
{
    const int m = b->npresent - 1; /* the server's marker slot */
    ncl_uc_store(SP_PRESENT(b, m), 1);
    ncl_fence(); /* marker visible before the door read and scan (Dekker with the
                    fast winner: its present[] set, fence, scan incl. slot m) */
    ncl_splitter_reset_if_quiet(b, m, st);
    ncl_fence_ss(); /* any door = OPEN store before the marker clears */
    ncl_present_clear(b, m);
}

void ncl_splitter_reset_if_quiet(struct ncl_base *b, int skip, struct ncl_base_stats *st)
{
    /* Pre-check (optimization only): under contention some other node is
     * almost always present, typically the one seen last time. One load, no
     * fence, instead of door load + fence + scan. Skipping is always safe: it
     * only leaves the fast path closed longer. */
    const int np = b->npresent;
    int hint = b->reset_hint;
    if (hint < 0 || hint == skip)
        hint = skip >= 0 ? (skip + 1) % np : 0;
    if (hint != skip && ncl_uc_load(SP_PRESENT(b, hint))) {
        NCL_STAT(st->split_precheck_skips++);
        NCL_STAT(st->split_reset_skips++);
        return;
    }

    if (ncl_uc_load(SP_DOOR(b)) != DOOR_CLOSED)
        return;
    /* door read before present[] reads: a node that read door == OPEN and is
     * about to close it set present[] before its fence (1), so we see it. */
    ncl_fence();
    if (NCL_UNLIKELY(ncl_debug_reset_delay))
        ncl_lat_spin_cycles(ncl_debug_reset_delay);
    /* Start after `skip` so the scan spreads over nodes; early exit on the
     * first set flag. Order doesn't affect safety: a reset requires all 0. */
    int start = skip >= 0 ? (skip + 1) % np : 0;
    int j = ncl_uc_scan_first(SP_PRESENT(b, 0), np, start, np, skip, &st->split_scan_loads);
    if (j >= 0) {
        b->reset_hint = j;
        NCL_STAT(st->split_reset_skips++);
        return;
    }
    if (NCL_UNLIKELY(ncl_debug_reset_delay))
        ncl_lat_spin_cycles(ncl_debug_reset_delay);
    ncl_fence();
    ncl_uc_store(SP_DOOR(b), DOOR_OPEN);
    NCL_STAT(st->split_resets++);
}

void ncl_top_announce(struct ncl_base *b, int side)
{
    peterson_announce(TOP_FLAG(b, side), TOP_TURN(b), (uint64_t)side);
}

int ncl_top_may_enter(struct ncl_base *b, int side)
{
    return peterson_may_enter(TOP_FLAG(b, 1 - side), TOP_TURN(b), (uint64_t)side);
}

void ncl_top_lock(struct ncl_base *b, int side)
{
    peterson_lock(TOP_FLAG(b, side), TOP_FLAG(b, 1 - side), TOP_TURN(b), (uint64_t)side);
    ncl_fence();
}

void ncl_top_unlock(struct ncl_base *b, int side) { ncl_uc_store(TOP_FLAG(b, side), 0); }

int ncl_top_other_flag(struct ncl_base *b, int side) { return ncl_uc_load(TOP_FLAG(b, 1 - side)) != 0; }

/* ---- (d) Hybrid: splitter + tournament + top Peterson (design §4d) ------- */

static int hybrid_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    if (ncl_splitter_enter(b, i, st)) {
        ncl_top_lock(b, NCL_SIDE_FAST);
        return NCL_PATH_HYBRID_FAST;
    }
    tournament_lock(b, i, st);
    ncl_top_lock(b, NCL_SIDE_SLOW);
    return NCL_PATH_HYBRID_SLOW;
}

/* Order follows design §4d exactly (Open: confirm against Aspnes §18.5.2 and
 * model-check). */
static void hybrid_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st)
{
    ncl_fence();
    if (path == NCL_PATH_HYBRID_FAST) {
        ncl_top_unlock(b, NCL_SIDE_FAST);
        ncl_fence();
        ncl_splitter_open_unless_others(b, i, st); /* safety + fairness rule */
    } else {
        ncl_top_unlock(b, NCL_SIDE_SLOW);
        ncl_fence();
        ncl_splitter_reset_if_quiet(b, i, st); /* door CLOSED && all other present == 0 */
        ncl_fence_ss();
        tournament_unlock_nofence(b, i);
    }
    ncl_fence_ss();
    ncl_present_clear(b, i);
}


/* ---- AK: Anderson-Kim fast path + tournament + top Peterson ------------- */

/* Anderson & Kim, "A new fast-path mechanism for mutual exclusion", Distrib.
 * Comput. 14 (2001), Figure 4 (bounded), statement numbers in comments.
 * ENTRY N/EXIT N = our tournament, ENTRY 2/EXIT 2 = the top Peterson lock
 * (FAST = side 0, SLOW = side 1). Statements 13-18 and 29-37 run "within the
 * critical section": here, at release, before the top lock is released.
 *
 * Ytype (free, indx) is ONE word: bit 0 = 1 if NOT free, indx << 1, so the
 * zeroed initial state is (true, 0). X holds p+1 (0 = none). indx and
 * Obstacle[] range over participants 0..P-1 (indx counts mod P).
 *
 * Fences: every store whose order against a later load matters is a Dekker
 * pair here: X (1) vs Y (2); Y (3) + Obstacle (4) vs X/Infast (5-6);
 * Name_Taken (7) vs Reset (8), against the reopener's Reset (14/33) vs
 * Obstacle/Name_Taken (15/34-35). */
#define AK_X(b) (&(b)->ak[0])
#define AK_Y(b) (&(b)->ak[1])
#define AK_RESET(b) (&(b)->ak[2])
#define AK_INFAST(b) (&(b)->ak[3])
#define AK_NT(b, k) (&(b)->ak[4 + (k)])
#define AK_OBS(b, k) (&(b)->ak[4 + (b)->P + (k)])
static uint64_t ak_word(int free, uint64_t indx) { return (indx << 1) | (free ? 0u : 1u); }
#define AK_FREE(w) (((w) & 1) == 0)
#define AK_INDX(w) ((int)((w) >> 1))

static void ak_check(struct ncl_base *b, uint64_t w)
{
    if (AK_INDX(w) >= b->P) {
        fprintf(stderr, "ncl: ak: corrupt Y/Reset word %#llx\n", (unsigned long long)w);
        abort();
    }
}

/* Statements 1-10: returns 1 with Infast set and *k = y.indx if this process
 * may take the fast path (caller then runs ENTRY 2(0)); 0 if deflected, with
 * *slow2 = 1 for SLOW 2 (it closed Y and its Obstacle[p] is set). */
static int ak_try_fast(struct ncl_base *b, int i, struct ncl_base_stats *st, int *k, int *slow2)
{
    const uint64_t me = (uint64_t)i + 1;
    ncl_uc_store(AK_X(b), me);                     /* 1: X := p */
    ncl_fence();
    uint64_t y = ncl_uc_load(AK_Y(b));             /* 2: y := Y */
    ak_check(b, y);
    if (!AK_FREE(y)) {
        NCL_STAT(st->split_fail_closed++);
        *slow2 = 0;                                /* SLOW 1 */
        return 0;
    }
    ncl_uc_store(AK_Y(b), ak_word(0, 0));          /* 3: Y := (false, 0) */
    ncl_fence_ss();
    ncl_uc_store(AK_OBS(b, i), 1);                 /* 4: Obstacle[p] := true */
    ncl_fence();
    *slow2 = 1;
    if (ncl_uc_load(AK_X(b)) != me ||              /* 5: X != p */
        ncl_uc_load(AK_INFAST(b))) {               /* 6: Infast */
        NCL_STAT(st->split_fail_lost++);
        return 0;
    }
    *k = AK_INDX(y);
    ncl_uc_store(AK_NT(b, *k), 1);                 /* 7: Name_Taken[y.indx] := true */
    ncl_fence();
    if (ncl_uc_load(AK_RESET(b)) != y) {           /* 8: Reset != y */
        ncl_uc_store(AK_NT(b, *k), 0);             /* 9 */
        NCL_STAT(st->split_fail_lost++);
        return 0;
    }
    ncl_uc_store(AK_INFAST(b), 1);                 /* 10: Infast := true */
    ncl_fence_ss();
    return 1;
}

static int ak_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    int k = 0, slow2;
    if (ak_try_fast(b, i, st, &k, &slow2)) {
        ncl_top_lock(b, NCL_SIDE_FAST);            /* 11: ENTRY 2(0) */
        b->ak_k = k;                               /* holder-only from here */
        return NCL_PATH_HYBRID_FAST;
    }
    tournament_lock(b, i, st);                     /* 21/26: ENTRY N(p) */
    ncl_top_lock(b, NCL_SIDE_SLOW);                /* 22/27: ENTRY 2(1) */
    b->ak_slow2 = slow2;
    return NCL_PATH_HYBRID_SLOW;
}

/* Reopen check shared by statements 15-17 and 34-37: Reset := (false, k) was
 * just stored; reopen with indx k+1 unless index k is still in use. */
static void ak_reopen_if_clear(struct ncl_base *b, int k, int check_nt, struct ncl_base_stats *st)
{
    ncl_fence();
    uint64_t nt = 0, ob;
    if (check_nt && ncl_meta_batching_for(AK_NT(b, k))) {
        ncl_line_t *ls[2] = {AK_NT(b, k), AK_OBS(b, k)};
        uint64_t v[2];
        ncl_uc_load_many(ls, 2, v);
        nt = v[0];
        ob = v[1];
    } else {
        if (check_nt)
            nt = ncl_uc_load(AK_NT(b, k));
        ob = nt ? 0 : ncl_uc_load(AK_OBS(b, k));
    }
    NCL_STAT(st->split_scan_loads += (uint64_t)(check_nt + 1));
    if (nt || ob) {
        NCL_STAT(st->split_reset_skips++);
        return;
    }
    uint64_t w = ak_word(1, (uint64_t)((k + 1) % b->P));
    ncl_uc_store(AK_RESET(b), w);                  /* 16/36 */
    ncl_fence_ss();
    ncl_uc_store(AK_Y(b), w);                      /* 17/37: fast path open */
    NCL_STAT(st->split_resets++);
}

/* Statements 13-20 (fast-path release), caller has fenced. */
static void ak_fast_exit(struct ncl_base *b, int i, int k, struct ncl_base_stats *st)
{
    ncl_uc_store(AK_OBS(b, i), 0);                 /* 13: Obstacle[p] := false */
    ncl_fence_ss();
    ncl_uc_store(AK_RESET(b), ak_word(0, (uint64_t)k)); /* 14 */
    if (ncl_fast_release != NCL_FASTREL_ALWAYS && ncl_top_other_flag(b, NCL_SIDE_FAST)) {
        /* Fairness variant: a slow-path process (or, for AKSRV, the server
         * with queued requests) waits at the top lock; leave the fast path
         * closed (always safe). The slow side's reset reopens it. */
        NCL_STAT(st->split_fast_keep_closed++);
    } else {
        ak_reopen_if_clear(b, k, 0, st);           /* 15-17: if !Obstacle[y.indx] */
    }
    ncl_fence_ss();
    ncl_uc_store(AK_NT(b, k), 0);                  /* 18: Name_Taken[y.indx] := false */
    ncl_fence_ss();
    ncl_top_unlock(b, NCL_SIDE_FAST);              /* 19: EXIT 2(0) */
    ncl_fence_ss();
    ncl_uc_store(AK_INFAST(b), 0);                 /* 20: Infast := false */
}

/* Statements 29-37 by a holder of the top lock's SLOW side; xval is its X
 * value (p+1, or P+1 for an AKSRV server); own_obs = this holder's Obstacle
 * slot to clear at 32 (-1: none, cleared by the grantee itself). */
static void ak_slow_reset(struct ncl_base *b, uint64_t xval, int own_obs, struct ncl_base_stats *st)
{
    ncl_uc_store(AK_Y(b), ak_word(0, 0));          /* 29: Y := (false, 0) */
    ncl_fence_ss();
    ncl_uc_store(AK_X(b), xval);                   /* 30: X := p */
    uint64_t r = ncl_uc_load(AK_RESET(b));         /* 31: y := Reset (only written in CS) */
    ak_check(b, r);
    if (own_obs >= 0)
        ncl_uc_store(AK_OBS(b, own_obs), 0);       /* 32: Obstacle[p] := false */
    ncl_fence_ss();
    ncl_uc_store(AK_RESET(b), ak_word(0, (uint64_t)AK_INDX(r))); /* 33 */
    ak_reopen_if_clear(b, AK_INDX(r), 1, st);      /* 34-37 */
}

static void ak_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st)
{
    ncl_fence(); /* critical-section effects (incl. data writeback) first */
    if (path == NCL_PATH_HYBRID_FAST) {
        ak_fast_exit(b, i, b->ak_k, st);
        return;
    }
    /* As published only SLOW 2 resets; the fairness variant has every
     * slow-path holder reset, since a fast winner may have left it closed.
     * The reset's safety argument (Name_Taken/Reset and Obstacle/Reset Dekker
     * pairs, Infast, statements 29-30) does not depend on the holder having
     * closed Y itself; a SLOW 1 holder's Obstacle is already false. */
    if (b->ak_slow2 || ncl_fast_release != NCL_FASTREL_ALWAYS)
        ak_slow_reset(b, (uint64_t)i + 1, i, st);
    ncl_fence_ss();
    ncl_top_unlock(b, NCL_SIDE_SLOW);              /* 24/38: EXIT 2(1) */
    ncl_fence_ss();
    tournament_unlock_nofence(b, i);               /* 25/39: EXIT N(p) */
}

/* ---- AKSRV: AK fast path, lock-server slow path ------------------------- */

int ncl_aksrv_try_fast(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    int k = 0, slow2;
    if (ak_try_fast(b, i, st, &k, &slow2)) {
        ncl_top_lock(b, NCL_SIDE_FAST);            /* 11: ENTRY 2(0) */
        b->ak_k = k;
        return 1;
    }
    b->ak_slow2 = slow2; /* node-local; read by this node's release only */
    return 0;
}

void ncl_aksrv_fast_release(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    ncl_fence();
    ak_fast_exit(b, i, b->ak_k, st);
}

void ncl_aksrv_slow_release(struct ncl_base *b, int i)
{
    if (b->ak_slow2)
        ncl_uc_store(AK_OBS(b, i), 0);             /* 32, by the grantee in its CS */
}

void ncl_aksrv_server_reset(struct ncl_base *b, struct ncl_base_stats *st)
{
    ak_slow_reset(b, (uint64_t)b->P + 1, -1, st);
}

/* ---- REACTIVE: Lamport <-> tournament per lock (design §4f) ---------------
 * mode: 0 = Lamport active, 1 = tournament active. While the tournament is
 * active, Lamport's y holds RX_INVALID, so the Lamport lock cannot be
 * acquired (lamport_lock returns -1 on seeing it). Hence
 *   (I) the Lamport lock is acquirable only while mode == 0:
 *       0 -> 1 is done by a Lamport holder, which stores mode = 1 while still
 *       holding it and releases it with y = RX_INVALID; 1 -> 0 by a tournament
 *       holder, which first takes over the INVALID Lamport lock (y = itself)
 *       and releases it normally later.
 * Safety: a node is inside either holding Lamport (so mode == 0 then, by I) or
 * holding the tournament having read mode == 1 after acquiring it. A switch
 * needs the current mode's lock and keeps the new one until the switcher's
 * release. If P (Lamport) and Q (tournament) were inside at once, the switch
 * between P's acquisition and Q's check (mode 0 -> 1), or between Q's check
 * and P's acquisition (1 -> 0), was made by P, resp. Q, which held the other's
 * lock until its release: contradiction. Two Lamport or two tournament holders
 * are excluded by those locks. Fences: each lock ends with one before the
 * mode load; switches fence after storing mode; unlocks start with one.
 * Liveness: a stale node gives up the wrong lock without waiting while
 * holding it. NOT model-checked. */

/* Tournament acquisition that also reads `mode` in the root's entry check,
 * after the root's flag and turn loads in load order (ncl_fence_ll: free on
 * x86, an acquire fence elsewhere), instead of a separate round trip after
 * the closing fence. On the batched backend (cached-flush) the three loads
 * share one round trip. Why the value is fresh enough: a switcher stores
 * `mode` before releasing the tournament (its release starts with a fence
 * and clears the root flag first). We enter the root either on seeing that
 * release (flag clear: the later load sees the earlier mode store) or on
 * turn, written by a process of the other side that announced after
 * acquiring its lower levels, i.e. after observing the release that followed
 * any earlier mode store (x86 is multi-copy atomic; elsewhere the fences on
 * that path). A holder on our own side released our lower levels after its
 * mode store, and we acquired them before this check. Returns the mode. */
static uint64_t tournament_lock_mode(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    uint64_t mode = 0;
    int got = 0;
    for (int pos = b->leaves + i; pos > 1; pos >>= 1) {
        int k = pos >> 1, s = pos & 1;
        if (k != 1) {
            if (peterson_lock(T_FLAG(b, k, s), T_FLAG(b, k, 1 - s), T_TURN(b, k), (uint64_t)s))
                st->contended++;
            continue;
        }
        peterson_announce(T_FLAG(b, 1, s), T_TURN(b, 1), (uint64_t)s);
        if (NCL_UNLIKELY(ncl_debug_mode_delay))
            ncl_lat_spin_cycles(ncl_debug_mode_delay); /* widen the window before the check */
        struct ncl_backoff bo;
        ncl_backoff_init(&bo);
        int waited = 0;
        for (;;) {
            ncl_line_t *ls[3] = {T_FLAG(b, 1, 1 - s), T_TURN(b, 1), b->mode};
            uint64_t v[3];
            if (NCL_UNLIKELY(ncl_debug_react & 32)) {
                /* UNSAFE: mode loaded first, so it can predate the release
                 * the flag/turn loads observe; ncl_debug_mode_delay widens
                 * the gap (in the real order the loads are adjacent). */
                v[2] = ncl_uc_load(b->mode);
                if (ncl_debug_mode_delay)
                    ncl_lat_spin_cycles(ncl_debug_mode_delay);
                ncl_uc_load_many_ordered(ls, 2, v);
            } else {
                ncl_uc_load_many_ordered(ls, 3, v);
            }
            if (!(v[0] && v[1] == (uint64_t)s)) {
                mode = v[2];
                got = 1;
                break;
            }
            waited = 1;
            ncl_backoff(&bo);
        }
        if (waited)
            st->contended++;
    }
    ncl_fence(); /* lock acquired before critical-section accesses */
    if (!got) /* one participant: no levels */
        mode = ncl_uc_load(b->mode);
    return mode;
}

unsigned ncl_react_up = 8, ncl_react_down = 4, ncl_react_dwell = 64;
int ncl_react_layout = 1; /* [x][y][mode]: measured faster (docs/cxl_results.md) */
int ncl_react_off = 0;

int ncl_react_parse(const char *s)
{
    if (!strcmp(s, "off")) {
        ncl_react_off = 1;
        return 0;
    }
    unsigned u, d, w = 64;
    int k = sscanf(s, "%u:%u:%u", &u, &d, &w);
    if (k < 2 || u > 16 || d > 64 || w < 64 || w > 1000000)
        return -1;
    ncl_react_up = u;
    ncl_react_down = d;
    ncl_react_dwell = w;
    ncl_react_off = 0;
    return 0;
}

static int reactive_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    int m, path;
    uint64_t c0;
    for (;;) {
        m = b->rx_guess;
        c0 = st->contended;
        if (m == 0) {
            path = lamport_lock(b, i, st);
            if (path >= 0)
                break; /* acquirable => mode == 0 (I): no mode load */
            b->rx_guess = 1;
        } else {
            path = NCL_PATH_TOURNAMENT;
            if (tournament_lock_mode(b, i, st) == 1 || (ncl_debug_react & 2))
                break; /* mode read in the root's entry check */
            ncl_fence();
            tournament_unlock_nofence(b, i);
            b->rx_guess = 0;
        }
        NCL_STAT(st->react_stale++);
    }
    b->rx_held = 1 << m;
    b->rx_inval = 0;
    if (m != b->rx_lastm) {
        /* Another node switched since our last acquisition: our history and
         * dwell count describe the other mode. */
        b->rx_hist = 0;
        b->rx_n = 0;
        b->rx_lastm = m;
    }
    b->rx_hist = (b->rx_hist << 1) | (st->contended != c0);
    if (b->rx_n < 1000000)
        b->rx_n++;
    int to = m;
    if (ncl_react_off)
        ;
    else if (ncl_debug_react & 8)
        to = 1 - m;
    else if (m == 0 && __builtin_popcountll(b->rx_hist & 0xffff) >= (int)ncl_react_up)
        to = 1;
    else if (m == 1 && b->rx_n >= ncl_react_dwell &&
             __builtin_popcountll(b->rx_hist) <= (int)ncl_react_down)
        to = 0;
    if (to != m) {
        struct ncl_base_stats ign = {0};
        if (to == 1) {
            /* Holding Lamport: take the tournament too, switch, and release
             * Lamport as INVALID (reactive_unlock). */
            if (!(ncl_debug_react & 1)) {
                tournament_lock(b, i, &ign);
                b->rx_held |= 2;
            }
            ncl_uc_store(b->mode, 1);
            ncl_fence();
            b->rx_inval = !(ncl_debug_react & 4);
            NCL_STAT(st->react_up++);
        } else {
            /* Holding the tournament: acquire the INVALID Lamport lock
             * (treating RX_INVALID as free: a late y store may be in flight,
             * see lamport_lock_x), then switch. Released normally. */
            lamport_lock_x(b, i, &ign, 1);
            ncl_uc_store(b->mode, 0);
            ncl_fence();
            b->rx_held |= 1;
            NCL_STAT(st->react_down++);
        }
        b->rx_guess = to;
        b->rx_lastm = to;
        b->rx_hist = 0;
        b->rx_n = 0;
    }
    return path;
}

static void reactive_unlock(struct ncl_base *b, int i)
{
    if (b->rx_held & 2) {
        ncl_fence(); /* critical section (and a mode store) first */
        tournament_unlock_nofence(b, i);
    }
    if (b->rx_held & 1) {
        if (b->rx_inval) {
            /* Switched to the tournament: leave the Lamport lock unacquirable. */
            ncl_fence();
            ncl_uc_store(LAM_Y(b), RX_INVALID);
            ncl_fence_ss();
            ncl_uc_store(LAM_B(b, i), 0);
        } else {
            lamport_unlock(b, i);
        }
    }
    b->rx_held = 0;
    b->rx_inval = 0;
}

void ncl_base_reactive_set_mode(ncl_line_t *lines, int P, int mode)
{
    (void)P;
    /* [mode][x][y] or (ncl_react_layout) [x][y][mode] */
    ncl_uc_store(&lines[ncl_react_layout ? 2 : 0], mode ? 1 : 0);          /* mode */
    ncl_uc_store(&lines[ncl_react_layout ? 1 : 2], mode ? RX_INVALID : 0); /* Lamport's y */
    ncl_fence();
}

/* ---- BAKERY: flat bakery, choosing merged into the ticket (design §4g) ------
 * slot[i] = {choosing (bit 63), ticket (bits 0-62)}, one line each, written
 * only by participant i. acquire: {choosing, 0}; fence; read all; t = 1 +
 * max ticket; {0, t} (publishes the ticket and clears choosing in one store);
 * fence; read all; then wait until no j blocks: j blocks while choosing, or
 * while its ticket is nonzero and (ticket, j) < (t, i). A j once seen not
 * blocking is never re-checked: one read with choosing == 0 and a
 * non-blocking ticket satisfies both of classic bakery's waits for j. If j
 * is later seen choosing again, its doorway started after t was visible, so
 * it draws a larger ticket.
 * Waiting (g6): poll only the predecessor (largest (ticket, j) blocker below
 * us; a choosing blocker if there is none) until its slot changes, then
 * re-read the remaining blockers in one batch. "Changes", not "becomes 0":
 * the predecessor may release and re-enter between two polls. NOT
 * model-checked.
 * --bakery-wait classic (default): a blocker j once seen with choosing == 0
 * (any read after our round-3 fence) is waited on by ticket only; seeing it
 * choosing again drops it (its new doorway started after t was visible, so it
 * draws a larger ticket, and a choosing slot holds ticket 0). --bakery-wait
 * strict (the spec's first pseudocode) keeps waiting until j's new ticket is
 * published. */

int ncl_bakery_wait_classic = 1;
int ncl_debug_bakery = 0;
uint64_t ncl_debug_bakery_delay = 0;
int ncl_bakery_poll_all = 0;

#define BAK_CHOOSING ((uint64_t)1 << 63)
#define BAK_REREAD_POLLS 64 /* full re-read of the blockers at least this often */

/* clr: j was seen with choosing == 0 after our ticket was visible (classic rule). */
static int bak_blocks(uint64_t v, int j, uint64_t t, int i, int clr)
{
    if (v & BAK_CHOOSING)
        return !clr && !(ncl_debug_bakery & 2); /* DEBUG 2: ignore choosing (UNSAFE) */
    return v != 0 && (v < t || (v == t && j < i));
}

/* One bakery node: seat i of the w slots at `slots`. Returns 1 if it waited. */
static int bak_node_lock(ncl_line_t *slots, int w, int i, struct ncl_base_stats *st)
{
    const int P = w;
    ncl_line_t *ls[NCL_MAX_PARTICIPANTS];
    int idx[NCL_MAX_PARTICIPANTS];
    uint64_t v[NCL_MAX_PARTICIPANTS];
    unsigned char clr[NCL_MAX_PARTICIPANTS]; /* classic rule: seen not choosing */
    int n = 0;
    for (int j = 0; j < P; j++)
        if (j != i) {
            idx[n] = j;
            ls[n++] = &slots[j];
        }

    if (!(ncl_debug_bakery & 1)) /* DEBUG 1: no choosing store (UNSAFE) */
        ncl_uc_store(&slots[i], BAK_CHOOSING);  /* round 1 */
    ncl_fence();                                      /* choosing visible before the reads */
    ncl_uc_load_many(ls, n, v);                       /* round 2 */
    uint64_t t = 0;
    for (int k = 0; k < n; k++) {
        /* DEBUG 8 (UNSAFE, classic only): clear j from this read, made before
         * our ticket was visible. */
        clr[k] = (ncl_debug_bakery & 8) && !(v[k] & BAK_CHOOSING);
        uint64_t tk = v[k] & ~BAK_CHOOSING;
        if (tk > t)
            t = tk;
    }
    t++;
    if (t >= BAK_CHOOSING)
        abort(); /* 2^63 tickets: unreachable in practice */
    if (NCL_UNLIKELY(ncl_debug_bakery_delay))
        ncl_lat_spin_cycles(ncl_debug_bakery_delay); /* widen the doorway */
    ncl_uc_store(&slots[i], t);                  /* round 3 */
    ncl_fence();                                      /* ticket visible before the reads */
    ncl_uc_load_many(ls, n, v);                       /* round 4 */

    /* Keep the blockers, with the value last seen. */
    int m = 0, saw_choosing = 0;
    for (int k = 0; k < n; k++) {
        int c = ncl_bakery_wait_classic && (clr[k] || !(v[k] & BAK_CHOOSING));
        if (bak_blocks(v[k], idx[k], t, i, c)) {
            saw_choosing |= (v[k] & BAK_CHOOSING) != 0;
            ls[m] = ls[k];
            idx[m] = idx[k];
            clr[m] = (unsigned char)c;
            v[m++] = v[k];
        } else if (c && (v[k] & BAK_CHOOSING)) {
            NCL_STAT(st->bak_classic_drops++);
        }
    }
    if (!m)
        return 0;
    st->contended++;
    unsigned capped = 0; /* polls made at the backoff cap, over the whole wait */
    struct ncl_backoff bo_all; /* --bakery-poll all: one backoff over the whole wait */
    ncl_backoff_init(&bo_all);
    while (m) {
        /* Predecessor: the largest non-choosing (ticket, j) blocker, else a choosing one. */
        int p = -1;
        for (int k = 0; k < m; k++) {
            if (v[k] & BAK_CHOOSING) {
                if (p < 0)
                    p = k;
                continue;
            }
            if (p < 0 || (v[p] & BAK_CHOOSING) || v[k] > v[p] || (v[k] == v[p] && idx[k] > idx[p]))
                p = k;
        }
        struct ncl_backoff bo;
        ncl_backoff_init(&bo);
        if (ncl_bakery_poll_all) { /* ablation: every poll re-reads all blockers */
            if (bo_all.cur >= ncl_backoff_cap && ++capped > 16)
                sched_yield();
            ncl_backoff(&bo_all);
        }
        for (int polls = 0; !ncl_bakery_poll_all && polls < BAK_REREAD_POLLS; polls++) {
            NCL_STAT(st->bak_pred_polls++);
            if (ncl_uc_load(ls[p]) != v[p])
                break;
            /* FCFS: a descheduled predecessor stalls everyone behind it, so
             * once fully backed off, yield (oversubscribed runs; returns at
             * once when nothing else is runnable). */
            if (bo.cur >= ncl_backoff_cap && ++capped > 16)
                sched_yield();
            ncl_backoff(&bo);
        }
        NCL_STAT(st->bak_rescans++);
        ncl_uc_load_many(ls, m, v);
        int m2 = 0;
        for (int k = 0; k < m; k++) {
            if (bak_blocks(v[k], idx[k], t, i, clr[k])) {
                saw_choosing |= (v[k] & BAK_CHOOSING) != 0;
                ls[m2] = ls[k];
                idx[m2] = idx[k];
                clr[m2] = clr[k] || (ncl_bakery_wait_classic && !(v[k] & BAK_CHOOSING));
                v[m2++] = v[k];
            } else if (clr[k] && (v[k] & BAK_CHOOSING)) {
                NCL_STAT(st->bak_classic_drops++); /* strict would have waited */
            }
        }
        m = m2;
    }
    if (saw_choosing)
        NCL_STAT(st->bak_choosing_seen++);
    return 1;
}

/* Node of entity e at level l: slots, width and seat. Level l's seats start
 * after the lower levels' (sum of cnt[0..l-1]); node e / k holds seats
 * (e / k) * k .. min(+k, cnt[l]) - 1. */
static ncl_line_t *bak_node(struct ncl_base *b, int l, int e, int *w, int *seat)
{
    size_t off = 0;
    for (int m = 0; m < l; m++)
        off += (size_t)b->bak_cnt[m];
    int k = b->bak_k, first = (e / k) * k;
    int left = b->bak_cnt[l] - first;
    *w = left < k ? left : k;
    *seat = e - first;
    return b->bak + off + first;
}

/* Climb leaf to root, one bakery node per level (flat: one level). */
static int bakery_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    int waited = 0, e = i;
    for (int l = 0; l < b->bak_levels; l++, e /= b->bak_k) {
        int w, seat;
        ncl_line_t *slots = bak_node(b, l, e, &w, &seat);
        waited |= bak_node_lock(slots, w, seat, st);
    }
    ncl_fence(); /* lock acquired before critical-section accesses */
    return waited ? NCL_PATH_BAKERY_WAIT : NCL_PATH_BAKERY_FAST;
}

/* Release root to leaf, store-store ordered: a seat at a higher node is shared
 * over time by its whole subtree, and releasing a lower node first would let
 * the next subtree winner write the seat we still hold (two writers). */
static void bakery_unlock(struct ncl_base *b, int i)
{
    int ent[32];
    for (int l = 0, e = i; l < b->bak_levels; l++, e /= b->bak_k)
        ent[l] = e;
    ncl_fence(); /* critical-section effects (incl. data writeback) first */
    for (int k = b->bak_levels - 1; k >= 0; k--) {
        int l = (ncl_debug_bakery & 4) ? b->bak_levels - 1 - k : k; /* DEBUG 4: leaf first (UNSAFE) */
        int w, seat;
        ncl_line_t *slots = bak_node(b, l, ent[l], &w, &seat);
        ncl_uc_store(&slots[seat], 0);
        if (k) {
            ncl_fence_ss();
            if (NCL_UNLIKELY(ncl_debug_bakery_delay))
                ncl_lat_spin_cycles(ncl_debug_bakery_delay); /* widen the gap between levels */
        }
    }
}

/* ---- (g2) hot-slot subtree under an asymmetric root ------------------------
 * Design §4g g2, with g1 in the cold tree C. NOT model-checked. R: side 0 = H's
 * winner, side 1 = C's winner; cold nodes take R only while hot_mode is set (read
 * at C's top, level_wait). hot_owner[s] has one writer at a time: a claimer holding
 * the global lock (0 -> i+1), or the owner freeing it under its submutex outside an
 * acquisition (i+1 -> 0). hot_mode is written only by lock holders: set by a
 * claimer before its slot, cleared by a holder that reads every slot free. */

#define H_FLAG(b, k, s) (&(b)->hot_tree[3 * ((k) - 1) + (s)])
#define H_TURN(b, k) (&(b)->hot_tree[3 * ((k) - 1) + 2])
#define R_FLAG(b, s) (&(b)->hot_r[(s)])
#define R_TURN(b) (&(b)->hot_r[2])
enum { R_HOT = 0, R_COLD = 1 };

static void hot_delay(void)
{
    if (ncl_debug_hot_delay)
        ncl_lat_spin_cycles(ncl_debug_hot_delay);
}

/* Peterson at R from `side`; returns 1 if it waited. */
static int r_lock(struct ncl_base *b, int side, struct ncl_base_stats *st)
{
    int w = peterson_lock(R_FLAG(b, side), R_FLAG(b, 1 - side), R_TURN(b), (uint64_t)side);
    if (w) {
        NCL_STAT(st->hot_r_waits++);
        st->contended++;
    }
    return w;
}

/* Climb H from slot s. H's first entry check also reads R's turn (same batch: no
 * extra round on cached-flush): the side of R's last entrant, so R_COLD means a
 * cold node has entered R since the last hot acquisition (b->hot_rcold). */
static void hot_climb(struct ncl_base *b, int s, struct ncl_base_stats *st)
{
    int pos = ncl_hot_k + s, k = pos >> 1, sd = pos & 1;
    peterson_announce(H_FLAG(b, k, sd), H_TURN(b, k), (uint64_t)sd);
    ncl_line_t *ls[3] = {H_FLAG(b, k, 1 - sd), H_TURN(b, k), R_TURN(b)};
    uint64_t v[3];
    ncl_uc_load_many(ls, 3, v);
    b->hot_rcold = v[2] == R_COLD;
    if (v[0] && v[1] == (uint64_t)sd) {
        struct ncl_backoff bo;
        ncl_backoff_init(&bo);
        st->contended++;
        while (!peterson_may_enter(ls[0], ls[1], (uint64_t)sd))
            ncl_backoff(&bo);
    }
    for (pos >>= 1; pos > 1; pos >>= 1) {
        k = pos >> 1;
        sd = pos & 1;
        if (peterson_lock(H_FLAG(b, k, sd), H_FLAG(b, k, 1 - sd), H_TURN(b, k), (uint64_t)sd))
            st->contended++;
    }
}

/* Skip the next admissible releases, doubling per call (full table, eviction). */
static void hot_adm_backoff(struct ncl_base *b)
{
    b->hot_adm_backoff = b->hot_adm_backoff ? b->hot_adm_backoff * 2 : 16;
    if (b->hot_adm_backoff > NCL_HOT_BACKOFF_MAX)
        b->hot_adm_backoff = NCL_HOT_BACKOFF_MAX;
    b->hot_adm_skip = b->hot_adm_backoff;
}

/* H's levels of slot s, top-down, store-store ordered. */
static void hot_unlock_h(struct ncl_base *b, int s)
{
    int path[8], n = 0;
    for (int pos = ncl_hot_k + s; pos > 1; pos >>= 1)
        path[n++] = pos;
    for (int d = n - 1; d >= 0; d--) {
        ncl_uc_store(H_FLAG(b, path[d] >> 1, path[d] & 1), 0);
        if (d)
            ncl_fence_ss();
    }
}

/* Placement: the free slot whose lowest common ancestor with every occupied slot
 * is highest (hot nodes meet near H's top); -1 if none is free. */
static int hot_pick(const uint64_t *own, int k)
{
    int best = -1, bscore = -1;
    for (int s = 0; s < k; s++) {
        if (own[s])
            continue;
        int score = 64;
        for (int o = 0; o < k; o++)
            if (own[o]) {
                int lv = 32 - __builtin_clz((unsigned)(s ^ o)); /* LCA level in H */
                if (lv < score)
                    score = lv;
            }
        if (score > bscore) {
            bscore = score;
            best = s;
        }
    }
    return best;
}

/* Holding the global lock (through C): claim a free slot for node i. */
static int hot_claim(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    if (!b->hot_mode_seen) {
        /* Before the slot: every cold node that wins C's top after our release
         * reads it and enters R. */
        ncl_uc_store(b->hot_mode, 1);
        NCL_STAT(st->hot_mode_sets++);
        b->hot_mode_seen = 1;
        ncl_fence();
    }
    ncl_line_t *ls[32];
    uint64_t own[32];
    for (int s = 0; s < ncl_hot_k; s++)
        ls[s] = &b->hot_owner[s];
    ncl_uc_load_many(ls, ncl_hot_k, own);
    int s = hot_pick(own, ncl_hot_k);
    if (s < 0) {
        NCL_STAT(st->hot_full++);
        hot_adm_backoff(b); /* don't re-read a full table at every release */
        return 0;
    }
    if (NCL_UNLIKELY(ncl_debug_hot & 2))
        hot_delay(); /* widen read-table -> claim for the unlocked claimer */
    ncl_uc_store(&b->hot_owner[s], (uint64_t)i + 1);
    ncl_fence();
    atomic_store_explicit(&b->my_slot, s, memory_order_relaxed);
    b->hot_hist = 0;
    b->hot_nhist = 0;
    atomic_fetch_add_explicit(&ncl_hot_held, 1, memory_order_relaxed);
    NCL_STAT(st->hot_claims++);
    return 1;
}

void ncl_tour_hot_free(struct ncl_base *b, struct ncl_base_stats *st)
{
    (void)st;
    int s = atomic_load_explicit(&b->my_slot, memory_order_relaxed);
    if (s < 0)
        return;
    ncl_uc_store(&b->hot_owner[s], 0); /* our hot flags are clear: freed after a release */
    ncl_fence();
    atomic_store_explicit(&b->my_slot, -1, memory_order_relaxed);
    b->hot_clear_hint = 1;
    atomic_fetch_sub_explicit(&ncl_hot_held, 1, memory_order_relaxed);
}

/* Holding the global lock through R's cold side: clear hot_mode if every slot is
 * free. Slots are freed only outside acquisitions and claimed only by holders, so
 * no node is in H or on R's hot side now, and none gets there before the next
 * claim, which sets hot_mode again. */
static void hot_clear_check(struct ncl_base *b, struct ncl_base_stats *st)
{
    NCL_STAT(st->hot_clear_checks++);
    if (!(ncl_debug_hot & 16)) { /* DEBUG 16: UNSAFE, clear without reading the slots */
        ncl_line_t *ls[32];
        uint64_t own[32];
        for (int s = 0; s < ncl_hot_k; s++)
            ls[s] = &b->hot_owner[s];
        ncl_uc_load_many(ls, ncl_hot_k, own);
        for (int s = 0; s < ncl_hot_k; s++)
            if (own[s])
                return;
    }
    ncl_uc_store(b->hot_mode, 0);
    b->hot_mode_seen = 0;
    NCL_STAT(st->hot_mode_clears++);
}

/* Admission (at a cold release, holding the lock): recent reuse, and g1's learned
 * cap shows contenders meeting low enough that H + R is shorter than the rest of C. */
static int hot_admit(const struct ncl_base *b, int depth)
{
    uint64_t now = atomic_load_explicit(&b->hot_last, memory_order_relaxed);
    if (!b->hot_prev || now - b->hot_prev > ncl_hot_window_cycles)
        return 0;
    if (ncl_hot_admit_always)
        return 1;
    if (ncl_retain_probe <= 0 || !b->keep_need)
        return 0;
    return __builtin_ctz((unsigned)ncl_hot_k) + 1 < depth - b->keep_cap;
}

static void tournament_lock_hot(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    b->hot_prev = atomic_load_explicit(&b->hot_last, memory_order_relaxed);
    atomic_store_explicit(&b->hot_last, ncl_cycles(), memory_order_relaxed);
    int s = atomic_load_explicit(&b->my_slot, memory_order_relaxed);
    if (s >= 0) {
        if (NCL_UNLIKELY(ncl_debug_hot & 1))
            hot_delay(); /* widen choose-path -> climb against an unlocked lease free */
        b->hot_slot_acq = s; /* the release clears what this acquisition took */
        hot_climb(b, s, st);
        b->hot_rwaited = r_lock(b, R_HOT, st);
        ncl_fence();
        b->hot_path = NCL_HOT_PATH_HOT;
        NCL_STAT(st->hot_acqs++);
        return;
    }
    if (NCL_UNLIKELY(ncl_debug_hot & 2)) {
        b->hot_mode_seen = 0; /* UNSAFE: claim without holding the global lock */
        hot_claim(b, i, st);
    }
    uint64_t mode = tournament_lock_retain(b, i, st);
    b->hot_mode_seen = mode != 0;
    if (mode) {
        b->hot_rwaited = r_lock(b, R_COLD, st);
        ncl_fence();
        b->hot_path = NCL_HOT_PATH_COLD_R;
        NCL_STAT(st->hot_r_acqs++);
    } else {
        b->hot_path = NCL_HOT_PATH_COLD;
    }
}

static void tournament_unlock_hot(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    if (b->hot_path == NCL_HOT_PATH_HOT) {
        int s = b->hot_slot_acq;
        b->hot_hist = ((b->hot_hist << 1) | (b->hot_rcold ? 1u : 0u)) & 0xffffu;
        if (b->hot_nhist < 64 && ++b->hot_nhist == 64)
            b->hot_adm_backoff = 0; /* the slot paid off for a while: admit freely again */
        int evict = b->hot_nhist >= 16 && __builtin_popcount(b->hot_hist) >= 8;
        ncl_fence(); /* critical-section effects first */
        if (NCL_UNLIKELY(ncl_debug_hot & 4)) { /* UNSAFE: levels below before R */
            hot_unlock_h(b, s);
            hot_delay();
            ncl_fence_ss();
            ncl_uc_store(R_FLAG(b, R_HOT), 0);
        } else {
            ncl_uc_store(R_FLAG(b, R_HOT), 0);
            ncl_fence_ss();
            hot_unlock_h(b, s);
        }
        if (evict) { /* cold share: other nodes use this lock; R taxes them */
            ncl_fence();
            ncl_tour_hot_free(b, st);
            hot_adm_backoff(b);
            NCL_STAT(st->hot_free_fair++);
        }
        return;
    }
    const int path = b->hot_path;
    int admit = atomic_load_explicit(&b->my_slot, memory_order_relaxed) < 0 &&
                hot_admit(b, ncl_tour_depth(b));
    if (admit && b->hot_adm_skip > 0) {
        b->hot_adm_skip--;
        admit = 0;
    }
    if (admit) {
        if (hot_claim(b, i, st))
            b->hot_keep0 = 1;
    } else if (path == NCL_HOT_PATH_COLD_R) {
        if (!b->hot_rwaited)
            b->hot_clear_cnt++;
        if (b->hot_clear_cnt >= 16 || b->hot_clear_hint) {
            b->hot_clear_cnt = 0;
            b->hot_clear_hint = 0;
            hot_clear_check(b, st);
        }
    } else {
        b->hot_clear_hint = 0; /* bypassed R: hot_mode is already 0 */
    }
    if (path != NCL_HOT_PATH_COLD_R) {
        tournament_unlock_retain(b, i, st);
        return;
    }
    ncl_fence();
    if (NCL_UNLIKELY(ncl_debug_hot & 4)) { /* UNSAFE: C before R */
        tournament_unlock_retain(b, i, st);
        hot_delay();
        ncl_fence_ss();
        ncl_uc_store(R_FLAG(b, R_COLD), 0);
    } else {
        ncl_uc_store(R_FLAG(b, R_COLD), 0);
        ncl_fence_ss();
        tournament_unlock_retain(b, i, st);
    }
}

/* ---- dispatch ----------------------------------------------------------- */

int ncl_base_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    switch (b->alg) {
    case NCL_ALG_LAMPORT: return lamport_lock(b, i, st);
    case NCL_ALG_TOURNAMENT:
        if (ncl_retain && ncl_hot_k)
            tournament_lock_hot(b, i, st);
        else if (ncl_retain)
            tournament_lock_retain(b, i, st);
        else if (ncl_tour_hot_probe && b->mode) {
            /* g2's bypass test: read hot_mode as the cold tree's top would. It is
             * never set, so the lock behaves as the plain tournament. */
            if (tournament_lock_mode(b, i, st))
                NCL_STAT(st->hot_mode_set++);
        } else
            tournament_lock(b, i, st);
        return NCL_PATH_TOURNAMENT;
    case NCL_ALG_REACTIVE: return reactive_lock(b, i, st);
    case NCL_ALG_HYBRID: return hybrid_lock(b, i, st);
    case NCL_ALG_AK: return ak_lock(b, i, st);
    case NCL_ALG_BAKERY: return bakery_lock(b, i, st);
    case NCL_ALG_PTHREAD:
        if (pthread_mutex_lock(b->mtx))
            abort();
        return NCL_PATH_PTHREAD;
    case NCL_ALG_SERVER:
    case NCL_ALG_FASTSRV:
    case NCL_ALG_AKSRV: break; /* handled in src/server.c */
    }
    return -1;
}

void ncl_base_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st)
{
    switch (b->alg) {
    case NCL_ALG_LAMPORT: lamport_unlock(b, i); break;
    case NCL_ALG_TOURNAMENT:
        if (ncl_retain && ncl_hot_k) {
            tournament_unlock_hot(b, i, st);
            break;
        }
        if (ncl_retain) {
            tournament_unlock_retain(b, i, st);
            break;
        }
        ncl_fence();
        tournament_unlock_nofence(b, i);
        break;
    case NCL_ALG_REACTIVE: reactive_unlock(b, i); break;
    case NCL_ALG_HYBRID: hybrid_unlock(b, i, path, st); break;
    case NCL_ALG_AK: ak_unlock(b, i, path, st); break;
    case NCL_ALG_BAKERY: bakery_unlock(b, i); break;
    case NCL_ALG_PTHREAD:
        if (pthread_mutex_unlock(b->mtx))
            abort();
        break;
    case NCL_ALG_SERVER:
    case NCL_ALG_FASTSRV:
    case NCL_ALG_AKSRV: break;
    }
}

const char *ncl_alg_name(ncl_alg_t a)
{
    switch (a) {
    case NCL_ALG_LAMPORT: return "lamport";
    case NCL_ALG_TOURNAMENT: return "tournament";
    case NCL_ALG_HYBRID: return "hybrid";
    case NCL_ALG_AK: return "ak";
    case NCL_ALG_AKSRV: return "aksrv";
    case NCL_ALG_PTHREAD: return "pthread";
    case NCL_ALG_SERVER: return "server";
    case NCL_ALG_FASTSRV: return "fastsrv";
    case NCL_ALG_REACTIVE: return "reactive";
    case NCL_ALG_BAKERY: return "bakery";
    }
    return "?";
}

int ncl_alg_parse(const char *s, ncl_alg_t *out)
{
    if (!strcmp(s, "lamport") || !strcmp(s, "a")) *out = NCL_ALG_LAMPORT;
    else if (!strcmp(s, "tournament") || !strcmp(s, "b")) *out = NCL_ALG_TOURNAMENT;
    else if (!strcmp(s, "hybrid") || !strcmp(s, "d")) *out = NCL_ALG_HYBRID;
    else if (!strcmp(s, "ak")) *out = NCL_ALG_AK;
    else if (!strcmp(s, "aksrv")) *out = NCL_ALG_AKSRV;
    else if (!strcmp(s, "pthread")) *out = NCL_ALG_PTHREAD;
    else if (!strcmp(s, "server") || !strcmp(s, "c")) *out = NCL_ALG_SERVER;
    else if (!strcmp(s, "fastsrv") || !strcmp(s, "e")) *out = NCL_ALG_FASTSRV;
    else if (!strcmp(s, "reactive") || !strcmp(s, "f")) *out = NCL_ALG_REACTIVE;
    else if (!strcmp(s, "bakery") || !strcmp(s, "g")) *out = NCL_ALG_BAKERY;
    else return -1;
    return 0;
}
