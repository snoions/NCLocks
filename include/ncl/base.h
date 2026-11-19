/*
 * Global ("base") mutual-exclusion algorithms over P participants indexed
 * 0..P-1 (design §4). They use only plain 8-byte loads/stores on hot metadata
 * plus explicit fences — no cross-node atomics.
 *
 *   (a) LAMPORT     Lamport's fast mutex.
 *   (b) TOURNAMENT  Binary tree of two-party Peterson locks.
 *   (d) HYBRID      Splitter (race/door + present[]) fast path, tournament slow
 *                   path, top two-party Peterson lock; splitter reset per
 *                   design §4d (Aspnes §18.5.2). NOT yet model-checked.
 *   (c) SERVER      Lock servers (src/server.c); no base-algorithm lines.
 *   (e) FASTSRV     Splitter fast path + lock-server slow path; the server
 *                   holds the top lock's SLOW side (src/server.c). NOT yet
 *                   model-checked.
 *   AK              Anderson & Kim's bounded fast-path mechanism (Distrib.
 *                   Comput. 14, 2001, Fig. 4) on the same tournament and top
 *                   Peterson lock as HYBRID: the fast path is reopened with
 *                   O(1) accesses (renaming via Y.indx, Name_Taken[],
 *                   Obstacle[]) instead of scanning present[]. NOT
 *                   model-checked. docs/hybrid.md, "Anderson-Kim fast path".
 *   AKSRV           (e) with AK's fast path instead of the splitter: the
 *                   home server runs AK's reset (statements 29-37) when its
 *                   queue drains, while still holding the top SLOW side;
 *                   grantees clear their own Obstacle (32). NOT model-checked.
 *   REACTIVE        Per-lock switch between LAMPORT and TOURNAMENT (design
 *                   §4f): both locks plus a `mode` word written only by a
 *                   holder. While the tournament is active, Lamport's y holds
 *                   INVALID, so a Lamport acquisition that succeeds needs no
 *                   `mode` check; a tournament acquisition re-reads `mode`.
 *                   A holder switches by also taking the other lock (or taking
 *                   over the INVALID Lamport lock) and writing `mode`. Switches
 *                   on the rate of cross-node contention
 *                   (ncl_base_stats.contended). NOT model-checked.
 *   PTHREAD         Reference baseline, NOT a non-coherent algorithm: a
 *                   process-shared pthread_mutex_t (futex, i.e. coherent
 *                   cross-node atomics) in the lock's first metadata line.
 *                   Coherent backend and Phase 1 only (ncl_sys_create).
 *
 * Every variable is its own ncl_line_t. Each lock has two blocks of lines,
 * laid out by ncl_base_nlines()/ncl_base_nscan()/ncl_base_bind(): single
 * words in the meta region, the scanned arrays (Lamport b[], present[]) in
 * the scan region (ncl/mem.h).
 */
#ifndef NCL_BASE_H
#define NCL_BASE_H

#include "ncl/mem.h"

#include <pthread.h>

typedef enum {
    NCL_ALG_LAMPORT = 0,
    NCL_ALG_TOURNAMENT = 1,
    NCL_ALG_HYBRID = 2,
    NCL_ALG_SERVER = 3,
    NCL_ALG_FASTSRV = 4,
    NCL_ALG_AK = 5,
    NCL_ALG_AKSRV = 6,
    NCL_ALG_PTHREAD = 7,
    NCL_ALG_REACTIVE = 8,
} ncl_alg_t;

/* (e)-style: fast path + lock-server slow path holding the top SLOW side. */
static inline int ncl_alg_fastsrv_like(ncl_alg_t a) { return a == NCL_ALG_FASTSRV || a == NCL_ALG_AKSRV; }

/* Path taken by an acquisition (stats; for HYBRID/FASTSRV also needed at release). */
enum {
    NCL_PATH_LAMPORT_FAST = 0,
    NCL_PATH_LAMPORT_SLOW,     /* went through the b[] scan */
    NCL_PATH_TOURNAMENT,
    NCL_PATH_HYBRID_FAST,      /* also AK's fast path */
    NCL_PATH_HYBRID_SLOW,      /* also AK's SLOW 1 / SLOW 2 */
    NCL_PATH_SERVER,
    NCL_PATH_FASTSRV_FAST,     /* splitter winner, server not involved */
    NCL_PATH_FASTSRV_SLOW,     /* deflected: request/grant via the server */
    NCL_PATH_LOCAL_HANDOFF,    /* MODE_LOCAL/HANDOFF: submutex + global ownership passed in-node */
    NCL_PATH_GLOBAL_HANDOFF,   /* MODE_HANDOFF: owner field handed to this node by the releaser */
    NCL_PATH_PTHREAD,          /* PTHREAD baseline */
    NCL_PATH__COUNT,
};

extern const char *const ncl_path_names[NCL_PATH__COUNT];

/* Process-local view of one lock's base-algorithm state. */
struct ncl_base {
    ncl_alg_t alg;
    int P;             /* participants */
    int leaves;        /* tournament leaves: next power of two >= P */
    ncl_line_t *lam;   /* LAMPORT: [x, y] */
    ncl_line_t *lam_b; /* LAMPORT: b[0..P-1] (scan region) */
    ncl_line_t *split; /* HYBRID/FASTSRV: [race, door] */
    ncl_line_t *present; /* HYBRID/FASTSRV: present[0..npresent-1] (scan region) */
    int npresent;      /* P; FASTSRV P+1: slot P = home server's reset marker */
    ncl_line_t *tree;  /* TOURNAMENT/HYBRID: 3 lines per internal node */
    ncl_line_t *top;   /* HYBRID/FASTSRV: [flag[FAST], flag[SLOW], turn] */
    ncl_line_t *ak;    /* AK: [X, Y, Reset, Infast, Name_Taken[P], Obstacle[P]] */
    int ak_k;          /* AK, node-local, holder only: fast path's y.indx */
    int ak_slow2;      /* AK, node-local, holder only: slow path is SLOW 2 */
    pthread_mutex_t *mtx; /* PTHREAD: process-shared mutex in the lock's meta line */
    ncl_line_t *mode;  /* REACTIVE: 0 = LAMPORT, 1 = TOURNAMENT (written by a holder) */
    int rx_guess;      /* REACTIVE, node-local: mode last seen */
    int rx_held;       /* REACTIVE, holder: bit 0 = Lamport held, bit 1 = tournament */
    uint64_t rx_hist;  /* REACTIVE, holder: contended? per global acquisition (bit 0 = last) */
    unsigned rx_n;     /* REACTIVE, holder: global acquisitions in the current mode */
    int rx_lastm;      /* REACTIVE, holder: mode at this node's previous global acquisition */
    int rx_inval;      /* REACTIVE, holder: switched to the tournament, so release the
                          Lamport lock as INVALID */
    int reset_hint;    /* node-local: present[] index last seen set by a reset
                          check (-1 = none); written only by the lock holder
                          (or, for FASTSRV, the server) */
};

/* All fields are uint64_t counters (merged generically by ncl_base_stats_add).
 * AK reuses the split_* counters: fail_closed = SLOW 1 (Y not free), fail_lost
 * = SLOW 2 (deflected after closing Y), resets = fast path reopened,
 * reset_skips = reopen blocked (Name_Taken/Obstacle set), scan_loads = loads
 * in reopen checks. */
struct ncl_base_stats {
    uint64_t lamport_retries;     /* Lamport `goto start` taken */
    uint64_t split_fail_closed;   /* splitter "right": door already CLOSED */
    uint64_t split_fail_lost;     /* splitter "down": race overwritten */
    uint64_t split_resets;        /* door reopened by a slow-path releaser */
    uint64_t split_reset_skips;   /* door CLOSED but some present[j] set */
    uint64_t split_scan_loads;    /* present[] loads in reset checks (after the fence) */
    uint64_t split_precheck_skips; /* reset checks skipped by the unfenced pre-check */
    uint64_t split_fast_keep_closed; /* fast winner left door CLOSED: others present */
    uint64_t contended;   /* Lamport slow path or retry, tournament level waited on:
                             another node competed (always counted: REACTIVE uses it) */
    uint64_t react_up;    /* REACTIVE: switched Lamport -> tournament */
    uint64_t react_down;  /* REACTIVE: switched tournament -> Lamport */
    uint64_t react_stale; /* REACTIVE: acquired the wrong mode's lock, retried */
};

#define NCL_BASE_STATS_N (sizeof(struct ncl_base_stats) / sizeof(uint64_t))
static inline void ncl_base_stats_add(struct ncl_base_stats *d, const struct ncl_base_stats *s)
{
    uint64_t *dp = (uint64_t *)d;
    const uint64_t *sp = (const uint64_t *)s;
    for (size_t k = 0; k < NCL_BASE_STATS_N; k++)
        dp[k] += sp[k];
}

size_t ncl_base_nlines(ncl_alg_t alg, int P); /* meta-region lines per lock */
size_t ncl_base_nscan(ncl_alg_t alg, int P);  /* scan-region lines per lock */
void ncl_base_bind(struct ncl_base *b, ncl_alg_t alg, int P, ncl_line_t *lines,
                   ncl_line_t *scan);
/* Creator only, after the region is mapped and before any node attaches:
 * initial state that is not all-zero (PTHREAD: pthread_mutex_init with
 * PTHREAD_PROCESS_SHARED). Returns 0 or -1. */
int ncl_base_init_shared(ncl_alg_t alg, ncl_line_t *lines);
/* REACTIVE, creator only, before any node attaches: start the lock in mode
 * (0 = Lamport, 1 = tournament: mode word 1 and Lamport's y = INVALID, the
 * state a switch leaves). */
void ncl_base_reactive_set_mode(ncl_line_t *lines, int P, int mode);

/* LAMPORT/TOURNAMENT/HYBRID/AK/PTHREAD only. Acquire as participant i; returns the
 * NCL_PATH_* taken. Release with the value ncl_base_lock returned. */
int ncl_base_lock(struct ncl_base *b, int i, struct ncl_base_stats *st);
void ncl_base_unlock(struct ncl_base *b, int i, int path, struct ncl_base_stats *st);

/* ---- primitives shared by HYBRID and FASTSRV ----------------------------- */

enum { NCL_SIDE_FAST = 0, NCL_SIDE_SLOW = 1 };

/* present[i] = 1, then run the splitter. Returns 1 on "stop" (at most one
 * winner), 0 if deflected. present[i] stays set either way; the caller
 * clears it at release. */
int ncl_splitter_enter(struct ncl_base *b, int i, struct ncl_base_stats *st);
/* door = OPEN (the design's unconditional fast-winner reset; kept for reference). */
void ncl_splitter_open(struct ncl_base *b);
/* Fast-path winner i at release: reopen the door only if no other node is
 * present (SCAN, default). Otherwise leave it CLOSED; the last slow-path
 * releaser reopens it via ncl_splitter_reset_if_quiet. This rule is needed
 * for SAFETY, not only fairness: the reset check is check-then-act (door read,
 * present[] scan, door store), and a reopen during it lets a new winner in
 * before its stale door = OPEN admits a second one. While a slow releaser
 * runs that check its own present[] is set, so the strict scan never reopens
 * then (docs/hybrid.md, "Reset race"). */
void ncl_splitter_open_unless_others(struct ncl_base *b, int i, struct ncl_base_stats *st);
/* How the fast winner's release decides (global, set before nodes start):
 * SCAN (default) = every other present[], up to N-1 loads: the only safe
 * choice for HYBRID/FASTSRV. HINT = probe one remembered slot and reopen on a
 * miss: UNSAFE (reset race above), reachable only through the stress test's
 * --debug-unsafe-reopen, to show the race is detected. ALWAYS = unconditional
 * reopen: for AK only (AK as published; rejected for HYBRID/FASTSRV). For AK,
 * SCAN means our fairness variant (reopen only if the top lock's SLOW flag is
 * clear; every slow-path releaser runs the reset, statements 29-37). */
enum { NCL_FASTREL_SCAN = 0, NCL_FASTREL_HINT = 1, NCL_FASTREL_ALWAYS = 2 };
extern int ncl_fast_release;
/* DEBUG: spin this many TSC cycles after the reset check's door read and again
 * after its present[] scan, before the door = OPEN store (widens the window of the stale-door race; stress
 * --reset-delay). 0 = off. */
extern uint64_t ncl_debug_reset_delay;
/* Reopen the door iff it is CLOSED and every present[j] (j != skip) is 0.
 * skip = -1 checks everyone. First an unfenced pre-check of one likely-set
 * present[] slot: if it is set, give up immediately (skipping a reset is
 * always safe). Otherwise run the full fenced check, which re-reads every
 * slot. */
void ncl_splitter_reset_if_quiet(struct ncl_base *b, int skip, struct ncl_base_stats *st);
void ncl_present_clear(struct ncl_base *b, int i);

/* AKSRV pieces (src/server.c). try_fast: statements 1-11 (returns 1 holding the
 * top FAST side) or deflected (0; remembers SLOW 1/2 node-locally).
 * fast_release: 13-20 (fair rule unless -F always). slow_release: the
 * grantee's statement 32, before its REL. server_reset: 29-31, 33-37 by the
 * home server, holding the SLOW side. */
int ncl_aksrv_try_fast(struct ncl_base *b, int i, struct ncl_base_stats *st);
void ncl_aksrv_fast_release(struct ncl_base *b, int i, struct ncl_base_stats *st);
void ncl_aksrv_slow_release(struct ncl_base *b, int i);
void ncl_aksrv_server_reset(struct ncl_base *b, struct ncl_base_stats *st);
/* FASTSRV server: the reset check above with the server visible in
 * present[P] for its duration, so a fast winner's full scan never reopens
 * during it (the same protection a slow node's own present[i] gives in
 * HYBRID). */
void ncl_splitter_server_reset(struct ncl_base *b, struct ncl_base_stats *st);

/* Top two-party Peterson lock, split so a polling server can run it. */
void ncl_top_announce(struct ncl_base *b, int side); /* flag[side]=1; turn=side; fence */
int ncl_top_may_enter(struct ncl_base *b, int side); /* !(flag[other] && turn == side) */
void ncl_top_lock(struct ncl_base *b, int side);     /* announce + wait */
void ncl_top_unlock(struct ncl_base *b, int side);   /* flag[side]=0 */
int ncl_top_other_flag(struct ncl_base *b, int side); /* flag[1-side] (one load) */

const char *ncl_alg_name(ncl_alg_t a);
int ncl_alg_parse(const char *s, ncl_alg_t *out);

/* REACTIVE switch thresholds (set before nodes fork), per lock and node, over
 * this node's global acquisitions of the lock: to the tournament when at least
 * ncl_react_up of the last 16 met another node (<= 16); back to Lamport when
 * at most ncl_react_down of the last 64 did (<= 64), after at least
 * ncl_react_dwell acquisitions in tournament mode (>= 64). Default 8:4:64. */
extern unsigned ncl_react_up, ncl_react_down, ncl_react_dwell;
extern int ncl_react_off; /* 1: never switch (modes stay as initialized) */
/* "U:D[:W]" -> ncl_react_up/down/dwell (W default 64), or "off"; 0 or -1 (bad). */
int ncl_react_parse(const char *s);
/* DEBUG bits (ncl_stress --debug-react): 1 = switch to the tournament without
 * taking it, 2 = no `mode` re-read after a tournament acquisition, 4 = release
 * the Lamport lock normally (not INVALID) on a switch to the tournament: each
 * UNSAFE (must FAIL with 8); 8 = switch on every global acquisition (safe,
 * maximal switching for stress tests); 16 = no mode re-check after a Lamport
 * slow-path success (UNSAFE with 8 and ncl_debug_lamport_delay); 32 = load
 * `mode` before (not after) the root's flag and turn (UNSAFE with 8). */
extern int ncl_debug_react;
/* DEBUG (ncl_stress --lamport-delay): cycles Lamport spins between reading
 * y == 0 and storing y (widens the late-y-store window, src/base.c).
 * ncl_debug_mode_delay (--mode-delay): cycles a REACTIVE tournament acquirer
 * spins between acquiring and reading `mode`. Bit 16 of
 * ncl_debug_react skips the slow-path mode re-check that handles it (UNSAFE). */
extern uint64_t ncl_debug_lamport_delay, ncl_debug_mode_delay;

/* Exponential backoff for spin loops (pause iterations, capped). */
extern unsigned ncl_backoff_cap;

struct ncl_backoff { unsigned cur; };
static inline void ncl_backoff_init(struct ncl_backoff *bo) { bo->cur = 1; }
static inline void ncl_backoff_capped(struct ncl_backoff *bo, unsigned cap)
{
    for (unsigned k = 0; k < bo->cur; k++)
        ncl_cpu_relax();
    if (bo->cur < cap)
        bo->cur <<= 1;
}
static inline void ncl_backoff(struct ncl_backoff *bo) { ncl_backoff_capped(bo, ncl_backoff_cap); }

#endif
