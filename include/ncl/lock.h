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
 *                     gate. Not for SERVER/FASTSRV (they hand off through the
 *                     server queue) or the no-submutex ablation.
 *                     cfg.announce_place selects the §3.5 Open placement:
 *                     NCL_ANNOUNCE_LAZY (default: inside the gate, only
 *                     after reading a held owner), _INSIDE (spec baseline),
 *                     _BEFORE.
 *                     cfg.pick_policy (NCL_PICK_*): release probes one
 *                     announce[] slot at a per-node round-robin pointer if
 *                     this node's own acquisition was uncontended, else scans
 *                     (ADAPTIVE, default); always scans (SCAN, spec); always
 *                     probes (PROBE; collapses at moderate load).
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
#include "ncl/submutex.h"

typedef enum {
    NCL_MODE_PHASE1 = 0,
    NCL_MODE_HANDOFF = 1,
    NCL_MODE_LOCAL = 2,
} ncl_mode_t;

/* Announce placement in MODE_HANDOFF (design §3.5 Open). LAZY: inside the gate,
 * and only after reading a held owner (an uncontended take never announces). */
enum { NCL_ANNOUNCE_LAZY = 0, NCL_ANNOUNCE_INSIDE = 1, NCL_ANNOUNCE_BEFORE = 2 }; /* 0 = default */

enum { NCL_PICK_ADAPTIVE = 0, NCL_PICK_SCAN = 1, NCL_PICK_PROBE = 2 }; /* 0 = default */

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
    int announce_place;     /* HANDOFF: announce placement (§3.5), NCL_ANNOUNCE_*; 0 = lazy */
    int pick_policy;        /* HANDOFF: pick_waiter policy, NCL_PICK_*; 0 = adaptive */
    size_t data_size;       /* bytes of protected-data region */
    size_t user_meta_lines; /* extra uncached lines for applications/tests */
    uint32_t dev_lat_ns;
    uintptr_t base_addr;
    /* SERVER/FASTSRV */
    int server_pin;              /* 0: unpinned; NCL_SRV_PIN_NEXT: node n's server on CPU
                                    (N*T + n) % ncpu; NCL_SRV_PIN_SIBLING: on the SMT
                                    sibling of CPU n*T (node n's first worker) */
    unsigned server_backoff_cap; /* idle poll backoff cap, pause iterations (0 = 1024) */
    /* Debug knobs to show the emulator catches protocol bugs. */
    int debug_skip_data_wb;
    int debug_skip_data_inv;
};

struct ncl_range {
    const void *p;
    size_t len;
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

int ncl_sys_create(const struct ncl_sys_cfg *cfg);
void ncl_sys_destroy(void);

/* Parent, before fork: associate [off, off+len) of the data region with lock. */
int ncl_lock_set_data(int lock, ncl_off_t off, size_t len);

int ncl_node_attach(int node_id);
/* Barrier across all nodes, then stop this node's server (if any). */
void ncl_node_detach(void);
void ncl_thread_init(struct ncl_thread *t, int local_idx);

void ncl_acquire(struct ncl_thread *t, int lock);
/* hints == NULL: write back the lock's whole registered range. Otherwise write
 * back only the given ranges (caller promises nothing else was modified). */
void ncl_release(int lock, const struct ncl_range *hints, int nhints);

/* Static configuration (read-only after create). */
const struct ncl_sys_cfg *ncl_sys_cfg(void);
int ncl_participants(void);

/* Uncached lines reserved for the application (cfg.user_meta_lines). */
ncl_line_t *ncl_user_line(size_t i);

/* This node's stats. Only meaningful after threads have quiesced; server
 * stats only after ncl_node_detach(). */
void ncl_node_stats(struct ncl_node_stats *out);

const char *ncl_mode_name(ncl_mode_t m);
int ncl_mode_parse(const char *s, ncl_mode_t *out);

#endif
