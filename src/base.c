#include "ncl/base.h"

#include <string.h>

unsigned ncl_backoff_cap = 1024;
int ncl_fast_release = NCL_FASTREL_HINT;

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
};

static int pow2ceil(int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

static size_t lam_lines(int P) { return 2 + (size_t)P; }
static size_t split_lines(int P) { return 2 + (size_t)P; }
static size_t tree_lines(int P) { return 3 * (size_t)(pow2ceil(P) - 1); }
static size_t top_lines(void) { return 3; }

size_t ncl_base_nlines(ncl_alg_t alg, int P)
{
    switch (alg) {
    case NCL_ALG_LAMPORT: return lam_lines(P);
    case NCL_ALG_TOURNAMENT: return tree_lines(P) ? tree_lines(P) : 1;
    case NCL_ALG_HYBRID: return split_lines(P) + tree_lines(P) + top_lines();
    case NCL_ALG_SERVER: return 0;
    case NCL_ALG_FASTSRV: return split_lines(P) + top_lines();
    }
    return 0;
}

void ncl_base_bind(struct ncl_base *b, ncl_alg_t alg, int P, ncl_line_t *lines)
{
    memset(b, 0, sizeof *b);
    b->alg = alg;
    b->P = P;
    b->leaves = pow2ceil(P);
    b->reset_hint = -1;
    switch (alg) {
    case NCL_ALG_LAMPORT:
        b->lam = lines;
        break;
    case NCL_ALG_TOURNAMENT:
        b->tree = lines;
        break;
    case NCL_ALG_HYBRID:
        b->split = lines;
        b->tree = lines + split_lines(P);
        b->top = b->tree + tree_lines(P);
        break;
    case NCL_ALG_SERVER:
        break;
    case NCL_ALG_FASTSRV:
        b->split = lines;
        b->top = lines + split_lines(P);
        break;
    }
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
#define LAM_B(b, i) (&(b)->lam[2 + (i)])

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
    if (ncl_meta_batching()) { /* both reads in one batch; either order is safe */
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
#define SP_PRESENT(b, i) (&(b)->split[2 + (i)])
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
    int start = b->reset_hint >= 0 && b->reset_hint != i ? b->reset_hint : (i + 1) % b->P;
    if (ncl_fast_release == NCL_FASTREL_HINT) {
        /* Cheap variant: probe only the remembered slot. A miss reopens even
         * if some other node is present (the design's original unconditional
         * reopen, so still safe); fairness is weaker. */
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
    int j = ncl_uc_scan_first(SP_PRESENT(b, 0), b->P, start, b->P, i, &st->split_scan_loads);
    if (j >= 0) {
        b->reset_hint = j;
        st->split_fast_keep_closed++;
        return; /* leave CLOSED: someone is on the slow path */
    }
    ncl_fence(); /* scan before the store (load->store order, explicit) */
    ncl_uc_store(SP_DOOR(b), DOOR_OPEN);
}

void ncl_present_clear(struct ncl_base *b, int i) { ncl_uc_store(SP_PRESENT(b, i), 0); }

void ncl_splitter_reset_if_quiet(struct ncl_base *b, int skip, struct ncl_base_stats *st)
{
    /* Pre-check (optimization only): under contention some other node is
     * almost always present, typically the one seen last time. One load, no
     * fence, instead of door load + fence + scan. Skipping is always safe: it
     * only leaves the fast path closed longer. */
    int hint = b->reset_hint;
    if (hint < 0 || hint == skip)
        hint = skip >= 0 ? (skip + 1) % b->P : 0;
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
    /* Start after `skip` so the scan spreads over nodes; early exit on the
     * first set flag. Order doesn't affect safety: a reset requires all 0. */
    int start = skip >= 0 ? skip + 1 : 0;
    int j = ncl_uc_scan_first(SP_PRESENT(b, 0), b->P, start, b->P, skip, &st->split_scan_loads);
    if (j >= 0) {
        b->reset_hint = j;
        st->split_reset_skips++;
        return;
    }
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
        ncl_splitter_open_unless_others(b, i, st); /* fairness rule (was: unconditional) */
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

/* ---- dispatch ----------------------------------------------------------- */

int ncl_base_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    switch (b->alg) {
    case NCL_ALG_LAMPORT: return lamport_lock(b, i, st);
    case NCL_ALG_TOURNAMENT: tournament_lock(b, i); return NCL_PATH_TOURNAMENT;
    case NCL_ALG_HYBRID: return hybrid_lock(b, i, st);
    case NCL_ALG_SERVER:
    case NCL_ALG_FASTSRV: break; /* handled in src/server.c */
    }
    return -1;
}

void ncl_base_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st)
{
    switch (b->alg) {
    case NCL_ALG_LAMPORT: lamport_unlock(b, i); break;
    case NCL_ALG_TOURNAMENT: ncl_fence(); tournament_unlock_nofence(b, i); break;
    case NCL_ALG_HYBRID: hybrid_unlock(b, i, path, st); break;
    case NCL_ALG_SERVER:
    case NCL_ALG_FASTSRV: break;
    }
}

const char *ncl_alg_name(ncl_alg_t a)
{
    switch (a) {
    case NCL_ALG_LAMPORT: return "lamport";
    case NCL_ALG_TOURNAMENT: return "tournament";
    case NCL_ALG_HYBRID: return "hybrid";
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
    else if (!strcmp(s, "server") || !strcmp(s, "c")) *out = NCL_ALG_SERVER;
    else if (!strcmp(s, "fastsrv") || !strcmp(s, "e")) *out = NCL_ALG_FASTSRV;
    else return -1;
    return 0;
}
