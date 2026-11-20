/*
 * Two-level lock system (design §3): node-local submutex + global algorithm.
 *
 * Lifecycle:
 *   parent:  ncl_sys_create(cfg)          map region, write static config,
 *            ncl_lock_set_data(...)       zero metadata, register data ranges
 *            fork() one process per node
 *   node:    ncl_node_attach(node_id)     build node-local state
 *            ncl_thread_init(&t, ...)     per thread
 *            ncl_acquire(&t, lock) / ncl_release(lock, hints, n)
 *
 * Modes:
 *   NCL_MODE_PHASE1   base algorithm held across the critical section (§3.4).
 *   NCL_MODE_LOCAL    Phase 2a: Phase 1 plus local handoff (cohort passing,
 *                     §3.2). On release, if another local thread is queued on
 *                     the submutex and fewer than local_cap consecutive local
 *                     handoffs have happened, the submutex AND global
 *                     ownership pass to it: no global traffic, no data
 *                     writeback/invalidate. Ownership belongs to the node, so
 *                     any local thread may do the eventual global release.
 *                     Applies to every variant; for SERVER/FASTSRV the node
 *                     keeps the grant and the server hears only the final REL.
 *   NCL_MODE_HANDOFF  Phase 2b: local handoff as in NCL_MODE_LOCAL, plus
 *                     cross-node handoff through a gate + owner field (§3.3).
 *                     The base algorithm (a)/(b)/(d) is only a short gate
 *                     around reading/taking the uncached `owner` word (0 =
 *                     FREE, node+1 = held); it is NOT held across the critical
 *                     section. A releaser scans announce[] round-robin from
 *                     itself and hands off with one 8-byte store to `owner`
 *                     (or stores FREE). Waiters poll `owner`; seeing FREE
 *                     means a missed announcement, so they go back through the
 *                     gate. Not for FASTSRV or the no-submutex ablation; SERVER
 *                     uses holder-to-waiter passing instead (below).
 *                     cfg.announce_place selects the §3.5 Open placement:
 *                     NCL_ANNOUNCE_BEFORE (default: announce, fence, then
 *                     the gate), _INSIDE (spec baseline). (A LAZY variant,
 *                     announcing only after reading a held owner, was removed:
 *                     it collapses when the gate is slow, docs/handoff.md.)
 *                     cfg.pick_policy (NCL_PICK_*): release probes one
 *                     announce[] slot at a per-node round-robin pointer if
 *                     this node's own acquisition was uncontended, else scans
 *                     (ADAPTIVE, default); always scans (SCAN, spec). (Always
 *                     probing was removed: it misses waiters under contention.)
 *                     cfg.handoff_wait (NCL_WAIT_*): waiters poll their own
 *                     announce line, granted by the releaser (OWN, default),
 *                     or the owner word (OWNER).
 *                     SERVER ("chain", design §4c variant, see docs/handoff.md):
 *                     the home server assigns each REQ a grant number and
 *                     publishes the successor of number n-1 in an uncached
 *                     succ[] ring; the holder of k passes the lock by storing
 *                     the grant into its successor's grant slot, and only
 *                     messages the server (REL) when it finds none. A hint
 *                     bit in the grant says whether to read succ[] at all:
 *                     pick_policy ADAPTIVE (default) follows the hint, SCAN
 *                     always reads. cfg.chain_lines = succ[] ring size =
 *                     assignment window (default 4N rounded up to a power of
 *                     two): the server never sees passes, so it assigns at
 *                     most that many grant numbers past the last one it knows
 *                     finished. FASTSRV is rejected.
 *
 * Precise dirty hints under NCL_MODE_LOCAL: hints from every release in a
 * local run are accumulated (node-local) and all written back when the lock
 * leaves the node; overflow, or any release without hints, falls back to a
 * full writeback of the lock's range.
 *
 * Ablation (cfg.no_submutex): no local submutex; the global algorithm is sized
 * for every thread and each thread is its own participant (§6.4). Not
 * supported for SERVER/FASTSRV (design: they would need per-thread queues).
 *
 * SERVER/FASTSRV start a server thread per node in ncl_node_attach. Every node
 * must call ncl_node_detach() when done: it waits until ALL nodes are done
 * (a server must keep serving the others) and then stops the server.
 *
 * Protected data: each lock may own a data range. In Phase 1 every global
 * acquire invalidates it and every global release writes it back (full mode),
 * or writes back only caller-supplied dirty ranges (precise mode, §2 rule 5).
 */
#ifndef NCL_LOCK_H
#define NCL_LOCK_H

#include "ncl/base.h"
#include "ncl/data.h"
#include "ncl/submutex.h"

typedef enum {
    NCL_MODE_PHASE1 = 0,
    NCL_MODE_HANDOFF = 1,
    NCL_MODE_LOCAL = 2,
} ncl_mode_t;

/* Announce placement in MODE_HANDOFF (design §3.5). BEFORE (default):
 * announce, fence, then enter the gate, so a releaser sees every waiter even
 * while the gate is congested. INSIDE: the spec baseline, announce inside the
 * gate before reading owner. */
enum { NCL_ANNOUNCE_BEFORE = 0, NCL_ANNOUNCE_INSIDE = 1 }; /* 0 = default */

enum { NCL_PICK_ADAPTIVE = 0, NCL_PICK_SCAN = 1 }; /* 0 = default */

/* How MODE_HANDOFF waiters learn of a handoff (gate algorithms; docs/handoff.md).
 * OWN (default): the announce word carries a per-node sequence number; the
 * releaser stores owner = j, fences, then copies j's sequence into word 1 of
 * j's announce line (the grant). Waiters poll their own line and re-read owner
 * (a missed announce leaves it FREE) after NCL_OWNER_RECHECK polls, then after
 * twice as many each time, so owner loads per wait grow as log(wait). A waiter
 * that learns of its handoff from owner instead waits for the grant before
 * clearing its announce, so stores to the shared line never overlap.
 * OWNER (design §3.3 as first specified): poll the shared owner word. All N-1
 * waiters poll one line, and on the CXL host every handoff queues behind their
 * polls, so throughput falls as 1/N (docs/cxl_results.md). Ignored outside
 * MODE_HANDOFF and by the server chain. */
enum { NCL_WAIT_OWN = 0, NCL_WAIT_OWNER = 1 }; /* 0 = default */
/* MODE_HANDOFF critical-path options (gate algorithms; cfg.handoff_opts bits,
 * docs/handoff.md "Shorter handoff path"). Not model-checked.
 *  GRANT:   the handoff is the grant store alone. `owner` is FREE or HELD
 *           (never a node), so the releaser stores nothing else and needs no
 *           owner-before-grant fence; only a release that finds no waiter
 *           stores FREE. Needs --wait own.
 *  CLEAR:   a node clears its announce at its global release (before the
 *           handoff fence) instead of before the critical section. While it
 *           holds the lock only it scans, and it skips its own slot.
 *  SKIP:    a waiter reads `owner` right after its announce (-G before only) and
 *           enters the gate only if it is FREE. Valid because the gate guards
 *           only FREE -> taken: releasers never take it, so a "held" read
 *           outside the gate tells as much as one inside, and a releaser that
 *           misses the announce stores FREE after that read (seen by the
 *           waiter's owner re-reads, which then retry through the gate).
 *           At saturation the gate is then entered only to take a FREE lock.
 *           Adaptive: only when this node's last global acquisition of the
 *           lock waited (as -X adaptive); otherwise straight to the gate. */
enum { NCL_HO_GRANT = 1, NCL_HO_CLEAR = 2, NCL_HO_SKIP = 4, NCL_HO_ALL = 7 };
/* cfg.handoff_opts = NCL_HO_DEFAULT: resolved by ncl_sys_create to all options
 * that apply (-M handoff, gate algorithm): grant needs --wait own, skip needs
 * -G before; elsewhere none. 0 = none (also for zero-initialized configs). */
#define NCL_HO_DEFAULT (-1)
/* "none", "all" or a comma list of grant,clear,skip. 0 ok, -1 bad. */
int ncl_ho_opts_parse(const char *s, int *out);
/* "none" or the comma list; buf of at least 24 bytes. */
const char *ncl_ho_opts_name(int opts, char *buf);

#ifndef NCL_OWNER_RECHECK
#define NCL_OWNER_RECHECK 16 /* polls before the first owner re-read */
#endif

#define NCL_SRV_CHAIN_MAX 128 /* max chain window (succ[] lines per lock) */

enum { NCL_SRV_PIN_NONE = 0, NCL_SRV_PIN_NEXT = 1, NCL_SRV_PIN_SIBLING = 2 };

struct ncl_sys_cfg {
    ncl_backend_t backend;
    ncl_alg_t alg;
    ncl_mode_t mode;
    int nnodes;
    int threads_per_node;   /* needed to size participants in ablation */
    int nlocks;
    int no_submutex;        /* ablation: each thread is a global participant */
    int local_cap;          /* LOCAL/HANDOFF: max consecutive local handoffs (0 = 64) */
    int announce_place;     /* HANDOFF: announce placement (§3.5), NCL_ANNOUNCE_*; 0 = before */
    int pick_policy;        /* HANDOFF: pick_waiter policy, NCL_PICK_*; 0 = adaptive */
    int handoff_wait;       /* HANDOFF, gate algorithms: NCL_WAIT_*; 0 = own line */
    int handoff_opts;       /* HANDOFF, gate algorithms: NCL_HO_* bits; 0 = none; NCL_HO_DEFAULT */
    int chain_lines;        /* SERVER HANDOFF: succ[] ring = assignment window, power of
                               two in [2, NCL_SRV_CHAIN_MAX]; 0 = 4N rounded up */
    int meta_split;         /* 1: scanned arrays (b[], present[], announce[], server
                               queues) in a CACHED metadata region, batched reads;
                               single words stay uncached. uc and emu only. 0: all
                               metadata the backend's type (ncl/mem.h) */
    int alg_per_lock;       /* 1: each lock's base algorithm is set with ncl_lock_set_alg
                               (lamport or tournament; default cfg.alg, which must be
                               one of them); the layout fits either */
    int precise_data;       /* 1: precise data maintenance in both directions: acquire
                               with ncl_acquire_ranges (only the named ranges are
                               invalidated) and release with dirty hints (mandatory).
                               0: ncl_acquire invalidates the lock's whole range when
                               the lock arrives at the node. */
    int pad_lines;          /* extra meta-region lines per lock (layout experiments) */
    size_t data_size;       /* bytes of protected-data region */
    size_t user_meta_lines; /* extra uncached lines for applications/tests */
    uint32_t dev_lat_ns;
    int mem_node;           /* NUMA node + 1 for the shared regions (meta, scan, data);
                               0 = default placement (or env NCL_MEM_NODE). uc: the
                               driver's pool must already be on that node (nid=) */
    uintptr_t base_addr;
    /* SERVER/FASTSRV */
    int server_pin;              /* 0: unpinned; NCL_SRV_PIN_NEXT: node n's server on CPU
                                    (N*T + n) % ncpu; NCL_SRV_PIN_SIBLING: on the SMT
                                    sibling of CPU n*T (node n's first worker) */
    unsigned server_backoff_cap; /* idle poll backoff cap, pause iterations (0 = 1024) */
    /* Debug knobs to show the emulator catches protocol bugs. */
    int debug_skip_data_wb;
    int debug_skip_data_inv;
    int debug_skip_meta_wb;  /* split layout: scan-region stores not written back */
    int debug_skip_meta_inv; /* split layout: scan-region loads not invalidated */
    int check_announce;      /* handoff, gate algorithms: at each global acquisition,
                                check that our announce word is 0 (counted in
                                ncl_handoff_stats.stale_announce; ncl_stress sets it) */
    int debug_precise;       /* PRECISE, DEBUG: 1 = no refresh on local handoffs, 2 = no
                                writeback of dirty lines before invalidating them */
    int debug_no_grant_wait; /* handoff, --wait own: clear the announce without waiting
                                for a grant still on its way (unsafe with full-line
                                writeback of the announce line) */
    /* EMU split: write back only the stored word of a cached metadata line (the old
     * model). Default 0: write back the node's whole line, as hardware does. */
    int emu_word_wb;
};


/* Per-thread handle. Lives on the thread's stack or heap. */
struct ncl_thread {
    int node;
    int local_idx;      /* index of this thread within its node */
    int participant;    /* global participant index (ablation only) */
    uint32_t reqid;     /* SERVER/FASTSRV request id counter (grant slot = local_idx) */
    struct ncl_qnode q;
};

/* This node's server thread (SERVER/FASTSRV). */
struct ncl_srv_stats {
    uint64_t msgs, rounds, idle_rounds, grants, queued, errors;
    uint64_t top_acquires;   /* FASTSRV: SLOW side of the top lock taken */
    uint64_t top_wait_polls; /* FASTSRV: polls of the top-lock condition */
    uint64_t top_yields;     /* FASTSRV: SLOW side yielded to a waiting fast winner */
    uint64_t published;      /* SERVER chain: succ[] entries published */
    uint64_t rel_grants;     /* SERVER chain: REL(k) with k+1 assigned: server granted it */
    uint64_t window_full;    /* SERVER chain: REQs held back by a full assignment window */
    struct ncl_base_stats base; /* FASTSRV: splitter resets done by the server */
};

/* MODE_HANDOFF counters (all uint64_t; merged generically). */
struct ncl_handoff_stats {
    uint64_t gate_entries;    /* passes through the gate (incl. retries) */
    uint64_t gate_takes;      /* owner was FREE in the gate: taken */
    uint64_t gate_handoffs;   /* owner == me already when read in the gate */
    uint64_t wait_handoffs;   /* owner == me seen while polling */
    uint64_t missed_announce; /* poll saw FREE: back through the gate (recovery) */
    uint64_t owner_polls;     /* owner loads while waiting */
    uint64_t handoffs_sent;   /* global releases that named a waiter */
    uint64_t frees;           /* global releases that stored FREE */
    uint64_t announce_loads;  /* announce[] loads in pick_waiter */
    uint64_t grant_polls;     /* WAIT_OWN: polls of the own grant word */
    uint64_t grant_waits;     /* WAIT_OWN: handoff seen via owner, then waited for the grant */
    uint64_t gate_skips;      /* SKIP: owner read held before the gate, gate not entered */
    uint64_t stale_announce;  /* check_announce: own announce found set at a new global
                                 acquisition (a late writeback resurrected it) */
};
#define NCL_HANDOFF_STATS_N (sizeof(struct ncl_handoff_stats) / sizeof(uint64_t))

/* Per-node statistics (node-local, summed over threads; approximate). */
struct ncl_node_stats {
    uint64_t acquires;          /* all acquisitions, incl. local handoffs */
    uint64_t global_releases;   /* releases that let the lock leave the node */
    uint64_t cap_releases;      /* ...of which forced by local_cap despite a waiter */
    uint64_t path[NCL_PATH__COUNT];
    struct ncl_base_stats base;
    struct ncl_srv_stats srv;
    struct ncl_handoff_stats ho;
};

/* MODE_HANDOFF: overlap the global release's data writeback with the
 * release path's leading metadata loads (default 1; set before fork). */
extern int ncl_wb_overlap;

int ncl_sys_create(const struct ncl_sys_cfg *cfg);
/* DEBUG (ncl_stress --grant-delay): cycles a 2b releaser spins between its
 * owner store and the grant store (--wait own, no --ho grant), widening the
 * window in which a waiter learns of the handoff from owner first. */
extern uint64_t ncl_debug_grant_delay;
void ncl_sys_destroy(void);

/* Parent, before fork: associate [off, off+len) of the data region with lock. */
int ncl_lock_set_data(int lock, ncl_off_t off, size_t len);
/* Parent, before fork, cfg.alg_per_lock only: lock's base algorithm
 * (NCL_ALG_LAMPORT or NCL_ALG_TOURNAMENT). Returns 0 or -1. */
int ncl_lock_set_alg(int lock, ncl_alg_t alg);
ncl_alg_t ncl_lock_alg(int lock);
/* Parent, before fork, -a reactive only: start lock in mode (0 Lamport, 1
 * tournament). Returns 0 or -1. */
int ncl_lock_set_reactive_mode(int lock, int mode);

int ncl_node_attach(int node_id);
/* Barrier across all nodes, then stop this node's server (if any). */
void ncl_node_detach(void);
void ncl_thread_init(struct ncl_thread *t, int local_idx);

void ncl_acquire(struct ncl_thread *t, int lock);
/* cfg.precise_data: acquire, promising to read or write only inside the given
 * ranges (within the lock's registered range) until the release. On return they
 * are current: invalidated when the lock arrives at the node, and on a local
 * handoff unless already fetched in this node's current run of ownership
 * (coherent within the node). A range the run dirtied is written back before
 * it is invalidated (clflushopt does so in hardware; the emulator needs it).
 * Writes must stay inside the acquired ranges too: a line written without
 * being invalidated first would write stale neighbors back. */
void ncl_acquire_ranges(struct ncl_thread *t, int lock, const struct ncl_range *r, int n);
/* hints == NULL: write back the lock's whole registered range (not allowed with
 * cfg.precise_data). Otherwise write back only the given ranges (caller
 * promises nothing else was modified). */
void ncl_release(int lock, const struct ncl_range *hints, int nhints);

/* Static configuration (read-only after create). */
const struct ncl_sys_cfg *ncl_sys_cfg(void);
int ncl_participants(void);
/* Meta-region lines per lock (the stride between lock blocks / 64 B). */
size_t ncl_lines_per_lock(void);
/* SERVER in MODE_HANDOFF: effective chain window (succ[] lines per lock), else 0. */
int ncl_chain_window(void);

/* Uncached lines reserved for the application (cfg.user_meta_lines). */
ncl_line_t *ncl_user_line(size_t i);

/* This node's stats. Only meaningful after threads have quiesced; server
 * stats only after ncl_node_detach(). */
void ncl_node_stats(struct ncl_node_stats *out);

const char *ncl_mode_name(ncl_mode_t m);
int ncl_mode_parse(const char *s, ncl_mode_t *out);

#endif
