/*
 * Multi-process launch for ncl_stress, ncl_bench and ncl_kv.
 *
 * Each node is a separate process, started by scripts/ncl_run.sh with the same
 * command line plus one role:
 *   --query               print "nodes=N timeout=S uc=0|1" (the launcher's plan) and exit
 *   --shm PATH --init     create the shared region ("device") in file PATH, set it up
 *                         (locks, data), write it back, exit
 *   --shm PATH --node I   map PATH, run node I, report
 * The processes share nothing but that region, as nodes on separate hosts would
 * share only the CXL device:
 *   - The creator stores a fingerprint of the command line; every node checks it.
 *   - Start: each node marks its ready line; node 0 waits for all of them and
 *     stores go; each node starts its own clock when it sees go (no shared clock).
 *   - Results: after ncl_node_detach each node writes a result blob into its own
 *     line-aligned slot of the data region (written back), then marks its done
 *     line; node 0 gathers the blobs (invalidated first) and prints the report.
 * The harness reserves its lines after the application's user lines and its result
 * slots after the application's data (harness_open).
 */
#ifndef NCL_HARNESS_H
#define NCL_HARNESS_H

#include <getopt.h>
#include <stdint.h>

#include "ncl/lock.h"

enum { HR_NONE, HR_QUERY, HR_INIT, HR_NODE };

struct harness {
    int role;
    int node;        /* HR_NODE: this process's node id */
    const char *shm; /* shared file */
    uint64_t tag;    /* fingerprint of the command line minus the harness options */
};
extern struct harness harness;

#define HARNESS_OPT_SHM 0x7f01
#define HARNESS_OPT_NODE 0x7f02
#define HARNESS_OPT_INIT 0x7f03
#define HARNESS_OPT_QUERY 0x7f04
#define HARNESS_LONGOPTS                                                                      \
    {"shm", required_argument, 0, HARNESS_OPT_SHM},                                           \
        {"node", required_argument, 0, HARNESS_OPT_NODE},                                     \
        {"init", no_argument, 0, HARNESS_OPT_INIT}, {"query", no_argument, 0, HARNESS_OPT_QUERY}

#define HARNESS_USAGE                                                                         \
    "  Launch with scripts/ncl_run.sh %s [options]: one process per node. Roles (added by\n"  \
    "  the launcher): --query | --shm PATH --init | --shm PATH --node I\n"

/* getopt hook: 1 = handled, 0 = not a harness option, -1 = bad argument. */
int harness_opt(int c, const char *arg);
/* After option parsing: a role is required (else a message pointing to
 * scripts/ncl_run.sh); computes the command-line fingerprint. 0 or -1. */
int harness_check(int argc, char **argv, const char *prog);
/* HR_QUERY: print the launcher's plan. Returns 0. */
int harness_query(int nnodes, ncl_backend_t backend, int timeout_s);
/* Reserve the harness's user lines and result slots in cfg (after the
 * application's), then create (HR_INIT) or attach to (HR_NODE) the region.
 * result_size: bytes of each node's result blob. 0 or -1. */
int harness_open(struct ncl_sys_cfg *cfg, size_t result_size);
/* HR_NODE: the start barrier (call after ncl_node_attach and after starting
 * the worker threads). Returns the start time, CLOCK_MONOTONIC ns. */
uint64_t harness_start(void);
/* Worker threads: wait until harness_start ran in this process; returns the
 * start time (spin until now >= it to start together). */
uint64_t harness_wait_start(void);
/* After ncl_node_detach: publish this node's result blob. */
void harness_put_result(const void *r);
/* Node 0: wait for `node`'s result blob and copy it into r. */
void harness_get_result(int node, void *r);

uint64_t harness_now_ns(void);

#endif
