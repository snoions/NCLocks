#define _GNU_SOURCE
#include "harness.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct harness harness = {.role = HR_NONE, .node = -1};

static int nnodes;
static size_t line0;     /* first harness user line: go, ready[N], done[N] */
static ncl_off_t data0;  /* first result slot in the data region */
static size_t rsize, rstride;
static _Atomic uint64_t start_ns; /* this process's start time, 0 until harness_start */

uint64_t harness_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static ncl_line_t *go_line(void) { return ncl_user_line(line0); }
static ncl_line_t *ready_line(int n) { return ncl_user_line(line0 + 1 + (size_t)n); }
static ncl_line_t *done_line(int n) { return ncl_user_line(line0 + 1 + (size_t)nnodes + (size_t)n); }
static void *slot(int n) { return ncl_data_ptr(data0 + (ncl_off_t)n * rstride); }

/* Wait for another process: spin briefly, then sleep (nodes may outnumber CPUs). */
static void wait_nonzero(ncl_line_t *l)
{
    for (unsigned i = 0; !ncl_uc_load(l); i++) {
        if (i < 1000)
            ncl_cpu_relax();
        else
            usleep(50);
    }
}

int harness_opt(int c, const char *arg)
{
    switch (c) {
    case HARNESS_OPT_SHM: harness.shm = arg; return 1;
    case HARNESS_OPT_NODE: {
        char *end;
        long v = strtol(arg, &end, 10);
        if (*end || v < 0 || v >= NCL_MAX_NODES) {
            fprintf(stderr, "--node: 0..%d\n", NCL_MAX_NODES - 1);
            return -1;
        }
        harness.node = (int)v;
        harness.role = HR_NODE;
        return 1;
    }
    case HARNESS_OPT_INIT: harness.role = HR_INIT; return 1;
    case HARNESS_OPT_QUERY: harness.role = HR_QUERY; return 1;
    }
    return 0;
}

/* FNV-1a over the arguments, skipping the harness's own. */
static uint64_t fingerprint(int argc, char **argv)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--init") || !strcmp(a, "--query") || !strncmp(a, "--shm=", 6) ||
            !strncmp(a, "--node=", 7))
            continue;
        if (!strcmp(a, "--shm") || !strcmp(a, "--node")) {
            i++;
            continue;
        }
        for (const char *p = a; ; p++) { /* includes the terminating 0 */
            h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
            if (!*p)
                break;
        }
    }
    return h;
}

int harness_check(int argc, char **argv, const char *prog)
{
    if (harness.role == HR_NONE) {
        fprintf(stderr,
                "%s: nodes are separate processes; launch with scripts/ncl_run.sh %s [options]\n",
                prog, prog);
        return -1;
    }
    if (harness.role != HR_QUERY && !harness.shm) {
        fprintf(stderr, "%s: --init and --node need --shm PATH\n", prog);
        return -1;
    }
    harness.tag = fingerprint(argc, argv);
    return 0;
}

int harness_query(int n, ncl_backend_t backend, int timeout_s)
{
    printf("nodes=%d timeout=%d uc=%d\n", n, timeout_s, backend == NCL_BACKEND_UC);
    return 0;
}

int harness_open(struct ncl_sys_cfg *cfg, size_t result_size)
{
    nnodes = cfg->nnodes;
    if (harness.role == HR_NODE && harness.node >= nnodes) {
        fprintf(stderr, "--node %d: only %d nodes\n", harness.node, nnodes);
        return -1;
    }
    line0 = cfg->user_meta_lines;
    cfg->user_meta_lines += 1 + 2 * (size_t)nnodes;
    rsize = result_size;
    rstride = (result_size + NCL_CACHELINE - 1) & ~(size_t)(NCL_CACHELINE - 1);
    data0 = (ncl_off_t)((cfg->data_size + NCL_CACHELINE - 1) & ~(size_t)(NCL_CACHELINE - 1));
    cfg->data_size = (size_t)data0 + rstride * (size_t)nnodes;
    cfg->app_tag = harness.tag;
    if (harness.role == HR_INIT)
        return ncl_sys_create(cfg, harness.shm);
    return ncl_sys_attach(cfg, harness.shm);
}

uint64_t harness_start(void)
{
    ncl_uc_store(ready_line(harness.node), 1);
    ncl_fence();
    if (harness.node == 0) {
        for (int n = 0; n < nnodes; n++)
            wait_nonzero(ready_line(n));
        ncl_uc_store(go_line(), 1);
        ncl_fence();
    }
    wait_nonzero(go_line());
    uint64_t t = harness_now_ns() + 1000000ull; /* workers are already waiting */
    atomic_store(&start_ns, t);
    return t;
}

uint64_t harness_wait_start(void)
{
    uint64_t t;
    while (!(t = atomic_load(&start_ns)))
        ncl_cpu_relax();
    return t;
}

void harness_put_result(const void *r)
{
    void *s = slot(harness.node);
    memcpy(s, r, rsize);
    ncl_data_writeback(s, rsize);
    ncl_uc_store(done_line(harness.node), 1);
    ncl_fence();
}

void harness_get_result(int n, void *r)
{
    wait_nonzero(done_line(n));
    void *s = slot(n);
    ncl_data_invalidate(s, rsize);
    memcpy(r, s, rsize);
}
