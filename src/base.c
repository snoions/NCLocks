#include "ncl/base.h"

#include <string.h>

unsigned ncl_backoff_cap = 1024;

const char *const ncl_path_names[NCL_PATH__COUNT] = {
    [NCL_PATH_LAMPORT_FAST] = "lamport_fast",
    [NCL_PATH_LAMPORT_SLOW] = "lamport_slow",
    [NCL_PATH_TOURNAMENT] = "tournament",
    [NCL_PATH_HYBRID_FAST] = "hybrid_fast",
    [NCL_PATH_HYBRID_SLOW] = "hybrid_slow",
};

static int pow2ceil(int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

static size_t lam_lines(int P) { return 2 + (size_t)P; }
static size_t tree_lines(int P) { return 3 * (size_t)(pow2ceil(P) - 1); }
static size_t top_lines(int P) { return 2 + (size_t)P; }

size_t ncl_base_nlines(ncl_alg_t alg, int P)
{
    switch (alg) {
    case NCL_ALG_LAMPORT: return lam_lines(P);
    case NCL_ALG_TOURNAMENT: return tree_lines(P) ? tree_lines(P) : 1;
    case NCL_ALG_HYBRID: return lam_lines(P) + tree_lines(P) + top_lines(P);
    }
    return 0;
}

void ncl_base_bind(struct ncl_base *b, ncl_alg_t alg, int P, ncl_line_t *lines)
{
    memset(b, 0, sizeof *b);
    b->alg = alg;
    b->P = P;
    b->leaves = pow2ceil(P);
    switch (alg) {
    case NCL_ALG_LAMPORT:
        b->lam = lines;
        break;
    case NCL_ALG_TOURNAMENT:
        b->tree = lines;
        break;
    case NCL_ALG_HYBRID:
        b->lam = lines;
        b->tree = lines + lam_lines(P);
        b->top = b->tree + tree_lines(P);
        break;
    }
}

/* ---- (a) Lamport's fast mutex ------------------------------------------ */

#define LAM_X(b) (&(b)->lam[0])
#define LAM_Y(b) (&(b)->lam[1])
#define LAM_B(b, i) (&(b)->lam[2 + (i)])

static void await_eq(ncl_line_t *l, uint64_t v)
{
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    while (ncl_uc_load(l) != v)
        ncl_backoff(&bo);
}

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
        for (int j = 0; j < b->P; j++)
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

static void peterson_lock(ncl_line_t *flag_me, ncl_line_t *flag_other, ncl_line_t *turn,
                          uint64_t side)
{
    ncl_uc_store(flag_me, 1);
    ncl_fence_ss();
    ncl_uc_store(turn, side); /* "victim" convention */
    ncl_fence();
    struct ncl_backoff bo;
    ncl_backoff_init(&bo);
    while (ncl_uc_load(flag_other) && ncl_uc_load(turn) == side)
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

/* ---- (d) Hybrid --------------------------------------------------------
 *
 * See docs/hybrid.md for the protocol and the (not yet model-checked)
 * argument for the splitter reset rule. Summary:
 *
 *   fast:  b[i]=1; x=i; F; y!=0 -> fail; y=i; F; x!=i -> fail
 *          winner: top Peterson side FAST, flag = fflag[i] (per participant)
 *   slow:  tournament, then top Peterson side SLOW (flag = sflag)
 *   fast release (holding top): y=0; b[i]=0; fflag[i]=0
 *   slow release (holding top): if y!=0 and all b[]==0 then y=0;
 *          sflag=0; tournament unlock
 */

#define TOP_SFLAG(b) (&(b)->top[0])
#define TOP_TURN(b) (&(b)->top[1])
#define TOP_FFLAG(b, i) (&(b)->top[2 + (i)])
enum { SIDE_FAST = 0, SIDE_SLOW = 1 };

static int any_fflag(struct ncl_base *b, struct ncl_base_stats *st)
{
    for (int j = 0; j < b->P; j++) {
        st->hybrid_fflag_loads++;
        if (ncl_uc_load(TOP_FFLAG(b, j)))
            return 1;
    }
    return 0;
}

static int hybrid_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    const uint64_t me = (uint64_t)i + 1;
    struct ncl_backoff bo;

    /* Splitter fast path. */
    ncl_uc_store(LAM_B(b, i), 1);
    ncl_fence_ss();
    ncl_uc_store(LAM_X(b), me);
    ncl_fence();
    uint64_t y = ncl_uc_load(LAM_Y(b));
    if (y != 0) {
        st->hybrid_fail_busy++;
#ifdef NCL_DIAG
        /* Approximate: b[] can change right after this read. */
        if (y <= (uint64_t)b->P && ncl_uc_load(LAM_B(b, (int)y - 1)) == 0)
            st->hybrid_fail_busy_stale++;
#endif
        goto fail;
    }
    ncl_uc_store(LAM_Y(b), me);
    ncl_fence();
    if (ncl_uc_load(LAM_X(b)) != me) {
        st->hybrid_fail_lost++;
        goto fail;
    }

    /* Won the splitter; b[i] stays 1 until release. */
    ncl_uc_store(TOP_FFLAG(b, i), 1);
    ncl_fence_ss();
    ncl_uc_store(TOP_TURN(b), SIDE_FAST);
    ncl_fence();
    ncl_backoff_init(&bo);
    while (ncl_uc_load(TOP_SFLAG(b)) && ncl_uc_load(TOP_TURN(b)) == SIDE_FAST)
        ncl_backoff(&bo);
    ncl_fence();
    return NCL_PATH_HYBRID_FAST;

fail:
    ncl_uc_store(LAM_B(b, i), 0);
    tournament_lock(b, i);
    ncl_uc_store(TOP_SFLAG(b), 1);
    ncl_fence_ss();
    ncl_uc_store(TOP_TURN(b), SIDE_SLOW);
    ncl_fence();
    ncl_backoff_init(&bo);
    while (ncl_uc_load(TOP_TURN(b)) == SIDE_SLOW && any_fflag(b, st))
        ncl_backoff(&bo);
    ncl_fence();
    return NCL_PATH_HYBRID_SLOW;
}

static void hybrid_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st)
{
    ncl_fence();
    if (path == NCL_PATH_HYBRID_FAST) {
        /* Clear splitter state while still holding the top lock, so no slow
         * holder can run its reset concurrently. */
        ncl_uc_store(LAM_Y(b), 0);
        ncl_fence_ss();
        ncl_uc_store(LAM_B(b, i), 0);
        ncl_fence_ss();
        ncl_uc_store(TOP_FFLAG(b, i), 0);
        return;
    }

    /* Slow holder: reset a stale splitter (y set, nobody won it). Safe only
     * if no fast attempt is in flight and no fast winner exists, i.e. every
     * b[j] == 0; otherwise skip and leave it to a later slow holder. */
    uint64_t y = ncl_uc_load(LAM_Y(b));
    if (y != 0) {
        ncl_fence();
        /* Start at y's writer: if y belongs to a live fast winner (the common
         * case) its b[] is set and one load decides the skip. Reading order
         * does not affect safety: a reset still requires every b[j] == 0. */
        int first = y <= (uint64_t)b->P ? (int)y - 1 : 0;
        int clear = 1;
        for (int k = 0; k < b->P && clear; k++) {
            int j = first + k < b->P ? first + k : first + k - b->P;
            st->hybrid_scan_loads++;
            if (ncl_uc_load(LAM_B(b, j)))
                clear = 0;
        }
        if (clear) {
            ncl_fence();
            ncl_uc_store(LAM_Y(b), 0);
            st->hybrid_resets++;
        } else {
            st->hybrid_reset_skips++;
        }
    }
    ncl_fence_ss();
    ncl_uc_store(TOP_SFLAG(b), 0);
    ncl_fence_ss();
    tournament_unlock_nofence(b, i);
}

/* ---- dispatch ----------------------------------------------------------- */

int ncl_base_lock(struct ncl_base *b, int i, struct ncl_base_stats *st)
{
    switch (b->alg) {
    case NCL_ALG_LAMPORT: return lamport_lock(b, i, st);
    case NCL_ALG_TOURNAMENT: tournament_lock(b, i); return NCL_PATH_TOURNAMENT;
    case NCL_ALG_HYBRID: return hybrid_lock(b, i, st);
    }
    return -1;
}

void ncl_base_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st)
{
    switch (b->alg) {
    case NCL_ALG_LAMPORT: lamport_unlock(b, i); break;
    case NCL_ALG_TOURNAMENT: ncl_fence(); tournament_unlock_nofence(b, i); break;
    case NCL_ALG_HYBRID: hybrid_unlock(b, i, path, st); break;
    }
}

const char *ncl_alg_name(ncl_alg_t a)
{
    switch (a) {
    case NCL_ALG_LAMPORT: return "lamport";
    case NCL_ALG_TOURNAMENT: return "tournament";
    case NCL_ALG_HYBRID: return "hybrid";
    }
    return "?";
}

int ncl_alg_parse(const char *s, ncl_alg_t *out)
{
    if (!strcmp(s, "lamport") || !strcmp(s, "a")) *out = NCL_ALG_LAMPORT;
    else if (!strcmp(s, "tournament") || !strcmp(s, "b")) *out = NCL_ALG_TOURNAMENT;
    else if (!strcmp(s, "hybrid") || !strcmp(s, "d")) *out = NCL_ALG_HYBRID;
    else return -1;
    return 0;
}
