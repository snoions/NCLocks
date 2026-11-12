#include "ncl/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

size_t ncl_base_nlines(ncl_alg_t alg, int P)
{
    switch (alg) {
    case NCL_ALG_LAMPORT: return lam_lines(P);
    case NCL_ALG_TOURNAMENT: return tree_lines(P) ? tree_lines(P) : 1;
    case NCL_ALG_HYBRID: return split_lines(P) + tree_lines(P) + top_lines();
    case NCL_ALG_SERVER: return 0;
    case NCL_ALG_FASTSRV: return split_lines(P) + top_lines();
    case NCL_ALG_AK: return ak_lines(P) + tree_lines(P) + top_lines();
    case NCL_ALG_AKSRV: return ak_lines(P) + top_lines();
    case NCL_ALG_PTHREAD: return 1;
    }
    return 0;
}

size_t ncl_base_nscan(ncl_alg_t alg, int P)
{
    switch (alg) {
    case NCL_ALG_LAMPORT:            /* b[] */
    case NCL_ALG_HYBRID: return (size_t)P; /* present[] */
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
    case NCL_ALG_TOURNAMENT:
        b->tree = lines;
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

static int lamport_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    const uint64_t me = (uint64_t)i + 1; /* 0 means empty */
    int slow = 0;
start:
    ncl_uc_store(LAM_B(b, i), 1);
    ncl_fence_ss();
    ncl_uc_store(LAM_X(b), me);
    ncl_fence(); /* (1) x = me before reading y */
    if (ncl_uc_load(LAM_Y(b)) != 0) {
        ncl_uc_store(LAM_B(b, i), 0);
        await_eq(LAM_Y(b), 0);
        st->lamport_retries++;
        goto start;
    }
    ncl_uc_store(LAM_Y(b), me);
    ncl_fence(); /* (2) y = me before reading x */
    if (ncl_uc_load(LAM_X(b)) != me) {
        slow = 1;
        ncl_uc_store(LAM_B(b, i), 0);
        ncl_fence(); /* b[i] = 0 before scanning b[] */
        /* Wait for every b[j] == 0, in index order. A batched first pass
         * reads all slots at once; only nonzero ones are then awaited. */
        uint64_t nl = 0;
        for (int j = 0; (j = ncl_uc_scan_first(LAM_B(b, 0), b->P, j, b->P - j, -1, &nl)) >= 0; j++)
            await_eq(LAM_B(b, j), 0);
        if (ncl_uc_load(LAM_Y(b)) != me) {
            await_eq(LAM_Y(b), 0);
            st->lamport_retries++;
            goto start;
        }
    }
    ncl_fence(); /* lock acquired before critical-section accesses */
    return slow ? NCL_PATH_LAMPORT_SLOW : NCL_PATH_LAMPORT_FAST;
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

static void peterson_lock(ncl_line_t *flag_me, ncl_line_t *flag_other, ncl_line_t *turn,
                          uint64_t side)
{
    peterson_announce(flag_me, turn, side);
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    while (!peterson_may_enter(flag_other, turn, side))
        ncl_backoff(&bo);
}

static void tournament_lock(struct ncl_base *b, int i)
{
    for (int pos = b->leaves + i; pos > 1; pos >>= 1) {
        int k = pos >> 1, s = pos & 1;
        peterson_lock(T_FLAG(b, k, s), T_FLAG(b, k, 1 - s), T_TURN(b, k), (uint64_t)s);
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
        st->split_fail_closed++; /* "right" */
        return 0;
    }
    ncl_uc_store(SP_DOOR(b), DOOR_CLOSED);
    ncl_fence(); /* (2) */
    if (ncl_uc_load(SP_RACE(b)) != me) {
        st->split_fail_lost++; /* "down" */
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
        st->split_resets++;
        return;
    }
    if (ncl_fast_release == NCL_FASTREL_HINT) {
        /* Probe only the remembered slot; a miss reopens even if some other
         * node is present. UNSAFE: that node may be a slow releaser in the
         * middle of its reset check (see base.h). Measurement only. */
        if (start != i) {
            st->split_scan_loads++;
            if (ncl_uc_load(SP_PRESENT(b, start))) {
                st->split_fast_keep_closed++;
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
        st->split_fast_keep_closed++;
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
        st->split_precheck_skips++;
        st->split_reset_skips++;
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
        st->split_reset_skips++;
        return;
    }
    if (NCL_UNLIKELY(ncl_debug_reset_delay))
        ncl_lat_spin_cycles(ncl_debug_reset_delay);
    ncl_fence();
    ncl_uc_store(SP_DOOR(b), DOOR_OPEN);
    st->split_resets++;
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
    tournament_lock(b, i);
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
        st->split_fail_closed++;
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
        st->split_fail_lost++;
        return 0;
    }
    *k = AK_INDX(y);
    ncl_uc_store(AK_NT(b, *k), 1);                 /* 7: Name_Taken[y.indx] := true */
    ncl_fence();
    if (ncl_uc_load(AK_RESET(b)) != y) {           /* 8: Reset != y */
        ncl_uc_store(AK_NT(b, *k), 0);             /* 9 */
        st->split_fail_lost++;
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
    tournament_lock(b, i);                         /* 21/26: ENTRY N(p) */
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
    st->split_scan_loads += (uint64_t)(check_nt + 1);
    if (nt || ob) {
        st->split_reset_skips++;
        return;
    }
    uint64_t w = ak_word(1, (uint64_t)((k + 1) % b->P));
    ncl_uc_store(AK_RESET(b), w);                  /* 16/36 */
    ncl_fence_ss();
    ncl_uc_store(AK_Y(b), w);                      /* 17/37: fast path open */
    st->split_resets++;
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
        st->split_fast_keep_closed++;
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

/* ---- dispatch ----------------------------------------------------------- */

int ncl_base_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    switch (b->alg) {
    case NCL_ALG_LAMPORT: return lamport_lock(b, i, st);
    case NCL_ALG_TOURNAMENT: tournament_lock(b, i); return NCL_PATH_TOURNAMENT;
    case NCL_ALG_HYBRID: return hybrid_lock(b, i, st);
    case NCL_ALG_AK: return ak_lock(b, i, st);
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
    case NCL_ALG_TOURNAMENT: ncl_fence(); tournament_unlock_nofence(b, i); break;
    case NCL_ALG_HYBRID: hybrid_unlock(b, i, path, st); break;
    case NCL_ALG_AK: ak_unlock(b, i, path, st); break;
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
    else return -1;
    return 0;
}
