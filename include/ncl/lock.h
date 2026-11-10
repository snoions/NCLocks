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
 *   NCL_MODE_HANDOFF  gate + owner field + cohort passing (§3.2/3.3) — not
 *                     implemented yet; ncl_sys_create rejects it.
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
} ncl_mode_t;

struct ncl_sys_cfg {
    ncl_backend_t backend;
    ncl_alg_t alg;
    ncl_mode_t mode;
    int nnodes;
    int threads_per_node;   /* needed to size participants in ablation */
    int nlocks;
    int no_submutex;        /* ablation: each thread is a global participant */
    size_t data_size;       /* bytes of protected-data region */
    size_t user_meta_lines; /* extra uncached lines for applications/tests */
    uint32_t dev_lat_ns;
    uintptr_t base_addr;
    /* SERVER/FASTSRV */
    int server_pin;              /* pin node n's server to CPU (N*T + n) % ncpu */
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
    struct ncl_base_stats base; /* FASTSRV: splitter resets done by the server */
};

/* Per-node statistics (node-local, summed over threads; approximate). */
struct ncl_node_stats {
    uint64_t acquires;
    uint64_t path[NCL_PATH__COUNT];
    struct ncl_base_stats base;
    struct ncl_srv_stats srv;
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

#endif
