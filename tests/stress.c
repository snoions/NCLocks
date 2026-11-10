/*
 * Runtime stress test (design §5).
 *
 * Forks one process per emulated node; each runs T threads that repeatedly
 * acquire a random lock. Every lock guards {counter, checksum, words[W]} in
 * the cached data region. Checks:
 *   - mutual exclusion: an uncached occupant word per lock must be 0 on entry
 *     and still ours on exit;
 *   - data visibility: the checksum must match the data on entry (catches a
 *     stale/torn view, e.g. a missing invalidate);
 *   - no lost updates: sum of counters == total acquisitions (catches a
 *     missing writeback, or a whole-snapshot stale read).
 * Random delays are injected inside and outside critical sections.
 *
 * With --backend emu, --skip-wb / --skip-inv must make this test FAIL;
 * that is the check that the emulator actually models non-coherence.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ncl/lock.h"

struct lockdata {
    uint64_t counter;
    uint64_t checksum;
    uint64_t words[];
};

static int nnodes = 4, nthreads = 2, nlocks = 4, nwords = 16, pin = 0, precise = 0;
static long iters = 100000;
static unsigned max_delay_ns = 200;
static size_t stride;
static uint64_t delay_cycles;

/* User-line layout: [0] start flag, [1..nlocks] occupant, then per node
 * RES_PER_NODE result lines. */
enum { RES_ACQ, RES_VIOL, RES_CKSUM, RES_PER_NODE };
static ncl_line_t *start_line(void) { return ncl_user_line(0); }
static ncl_line_t *occ_line(int l) { return ncl_user_line(1 + (size_t)l); }
static ncl_line_t *res_line(int n, int k)
{
    return ncl_user_line(1 + (size_t)nlocks + (size_t)n * RES_PER_NODE + (size_t)k);
}

static struct lockdata *ld(int l) { return ncl_data_ptr((ncl_off_t)l * stride); }

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static uint64_t checksum(const struct lockdata *d)
{
    uint64_t h = mix(0x1234, d->counter);
    for (int i = 0; i < nwords; i++)
        h = mix(h, d->words[i]);
    return h;
}

static uint64_t xorshift(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static void rand_delay(uint64_t *rng)
{
    if (delay_cycles)
        ncl_lat_spin_cycles(xorshift(rng) % delay_cycles);
}

struct worker {
    pthread_t tid;
    int node, idx;
    uint64_t acq, viol, cksum_err;
};

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    struct ncl_thread t;
    ncl_thread_init(&t, w->idx);
    /* splitmix64-scrambled seed: raw seeds differing in a few bits give
     * correlated xorshift streams. */
    uint64_t rng = (((uint64_t)w->node << 32) | (uint64_t)w->idx) + 0x9e3779b97f4a7c15ull;
    rng = (rng ^ (rng >> 30)) * 0xbf58476d1ce4e5b9ull;
    rng = (rng ^ (rng >> 27)) * 0x94d049bb133111ebull;
    rng = (rng ^ (rng >> 31)) | 1;
    uint64_t tag = (uint64_t)w->node * (uint64_t)nthreads + (uint64_t)w->idx + 1;

    if (pin) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET((int)((w->node * nthreads + w->idx) % ncpu), &cs);
        pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    }

    while (!ncl_uc_load(start_line()))
        ncl_cpu_relax();

    for (long it = 0; it < iters; it++) {
        int l = (int)(xorshift(&rng) % (uint64_t)nlocks);
        ncl_acquire(&t, l);

        ncl_line_t *occ = occ_line(l);
        if (ncl_uc_load(occ) != 0)
            w->viol++;
        ncl_uc_store(occ, tag);
        ncl_fence();

        struct lockdata *d = ld(l);
        if (d->checksum != checksum(d))
            w->cksum_err++;
        int k = (int)(xorshift(&rng) % (uint64_t)nwords);
        d->words[k] += xorshift(&rng) | 1;
        rand_delay(&rng);
        d->counter++;
        d->checksum = checksum(d);

        ncl_fence();
        if (ncl_uc_load(occ) != tag)
            w->viol++;
        ncl_uc_store(occ, 0);

        if (precise) {
            struct ncl_range r[2] = {
                {d, 2 * sizeof(uint64_t)},
                {&d->words[k], sizeof(uint64_t)},
            };
            ncl_release(l, r, 2);
        } else {
            ncl_release(l, NULL, 0);
        }
        w->acq++;
        rand_delay(&rng);
    }
    return NULL;
}

static int node_main(int node)
{
    if (ncl_node_attach(node) < 0)
        return 2;
    struct worker *ws = calloc((size_t)nthreads, sizeof *ws);
    for (int i = 0; i < nthreads; i++) {
        ws[i].node = node;
        ws[i].idx = i;
        pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]);
    }
    uint64_t acq = 0, viol = 0, ck = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(ws[i].tid, NULL);
        acq += ws[i].acq;
        viol += ws[i].viol;
        ck += ws[i].cksum_err;
    }
    ncl_node_detach(); /* all nodes done; servers stopped */
    struct ncl_node_stats st;
    ncl_node_stats(&st);
    viol += st.srv.errors; /* server-detected protocol violations */
    ncl_uc_store(res_line(node, RES_ACQ), acq);
    ncl_uc_store(res_line(node, RES_VIOL), viol);
    ncl_uc_store(res_line(node, RES_CKSUM), ck);
    ncl_fence();

    printf("node %2d: acq=%lu viol=%lu cksum_err=%lu", node, (unsigned long)acq,
           (unsigned long)viol, (unsigned long)ck);
    for (int p = 0; p < NCL_PATH__COUNT; p++)
        if (st.path[p])
            printf(" %s=%lu", ncl_path_names[p], (unsigned long)st.path[p]);
    if (st.base.lamport_retries)
        printf(" lamport_retries=%lu", (unsigned long)st.base.lamport_retries);
    struct ncl_base_stats sb = st.base;
    ncl_base_stats_add(&sb, &st.srv.base);
    if (sb.split_fail_closed || sb.split_fail_lost)
        printf(" split_closed=%lu split_lost=%lu", (unsigned long)sb.split_fail_closed,
               (unsigned long)sb.split_fail_lost);
    if (sb.split_resets || sb.split_reset_skips)
        printf(" door_resets=%lu reset_skips=%lu", (unsigned long)sb.split_resets,
               (unsigned long)sb.split_reset_skips);
    if (st.global_releases != st.acquires)
        printf(" global_releases=%lu cap_releases=%lu", (unsigned long)st.global_releases,
               (unsigned long)st.cap_releases);
    if (st.srv.msgs)
        printf(" srv_msgs=%lu srv_errors=%lu top_acq=%lu", (unsigned long)st.srv.msgs,
               (unsigned long)st.srv.errors, (unsigned long)st.srv.top_acquires);
    printf("\n");
    fflush(stdout);
    free(ws);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -n N          nodes (default 4)\n"
            "  -t T          threads per node (default 2)\n"
            "  -l L          locks (default 4)\n"
            "  -i I          iterations per thread (default 100000)\n"
            "  -w W          data words per lock (default 16)\n"
            "  -d NS         max random delay in/out of CS, ns (default 200)\n"
            "  -a ALG        lamport|tournament|hybrid|server|fastsrv (default lamport)\n"
            "  -b BACKEND    coherent|cached-flush|uc|emu (default emu)\n"
            "  -L NS         injected device latency, ns (default 0)\n"
            "  -m            ablation: no local submutex\n"
            "  -M MODE       phase1|local (default phase1)\n"
            "  -K CAP        local mode: max consecutive local handoffs (default 64)\n"
            "  -F MODE       (d)/(e) fast-release check: hint (default) | scan\n"
            "  -H            precise dirty-range hints on release\n"
            "  -p            pin threads to CPUs\n"
            "  --skip-wb     DEBUG: skip data writeback (emu must fail)\n"
            "  --skip-inv    DEBUG: skip data invalidate (emu must fail)\n",
            argv0);
}

int main(int argc, char **argv)
{
    struct ncl_sys_cfg cfg = {
        .backend = NCL_BACKEND_EMU,
        .alg = NCL_ALG_LAMPORT,
        .mode = NCL_MODE_PHASE1,
    };
    static struct option lopts[] = {
        {"skip-wb", no_argument, 0, 1},
        {"skip-inv", no_argument, 0, 2},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "n:t:l:i:w:d:a:b:L:M:K:F:mHph", lopts, NULL)) != -1) {
        switch (c) {
        case 'n': nnodes = atoi(optarg); break;
        case 't': nthreads = atoi(optarg); break;
        case 'l': nlocks = atoi(optarg); break;
        case 'i': iters = atol(optarg); break;
        case 'w': nwords = atoi(optarg); break;
        case 'd': max_delay_ns = (unsigned)atoi(optarg); break;
        case 'a':
            if (ncl_alg_parse(optarg, &cfg.alg) < 0) { usage(argv[0]); return 2; }
            break;
        case 'b':
            if (ncl_backend_parse(optarg, &cfg.backend) < 0) { usage(argv[0]); return 2; }
            break;
        case 'L': cfg.dev_lat_ns = (uint32_t)atoi(optarg); break;
        case 'm': cfg.no_submutex = 1; break;
        case 'M':
            if (ncl_mode_parse(optarg, &cfg.mode) < 0) { usage(argv[0]); return 2; }
            break;
        case 'K': cfg.local_cap = atoi(optarg); break;
        case 'F':
            if (!strcmp(optarg, "hint"))
                ncl_fast_release = NCL_FASTREL_HINT;
            else if (!strcmp(optarg, "scan"))
                ncl_fast_release = NCL_FASTREL_SCAN;
            else {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'H': precise = 1; break;
        case 'p': pin = 1; cfg.server_pin = 1; break;
        case 1: cfg.debug_skip_data_wb = 1; break;
        case 2: cfg.debug_skip_data_inv = 1; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (nwords < 1 || nlocks < 1 || nthreads < 1) {
        usage(argv[0]);
        return 2;
    }

    stride = (sizeof(struct lockdata) + (size_t)nwords * sizeof(uint64_t) + NCL_CACHELINE - 1) &
             ~(size_t)(NCL_CACHELINE - 1);
    cfg.nnodes = nnodes;
    cfg.threads_per_node = nthreads;
    cfg.nlocks = nlocks;
    cfg.data_size = stride * (size_t)nlocks;
    cfg.user_meta_lines = 1 + (size_t)nlocks + (size_t)nnodes * RES_PER_NODE;

    if (cfg.backend == NCL_BACKEND_UC && access(getenv("NCL_UC_DEV") ? getenv("NCL_UC_DEV")
                                                                      : "/dev/ncl_uc", F_OK)) {
        fprintf(stderr, "stress: uc backend requested but driver not loaded; skipping\n");
        return 77; /* ctest SKIP_RETURN_CODE */
    }
    if (ncl_sys_create(&cfg) < 0)
        return 2;
    printf("mem: %s\n", ncl_mem_describe());
    delay_cycles = ncl_ns_to_cycles(max_delay_ns);

    for (int l = 0; l < nlocks; l++) {
        if (ncl_lock_set_data(l, (ncl_off_t)l * stride, stride) < 0)
            return 2;
        struct lockdata *d = ld(l);
        d->checksum = checksum(d);
        ncl_data_writeback(d, stride);
    }

    printf("stress: alg=%s backend=%s mode=%s%s nodes=%d threads/node=%d participants=%d "
           "locks=%d iters/thread=%ld words=%d delay<=%uns lat=%uns%s%s%s\n",
           ncl_alg_name(cfg.alg), ncl_backend_name(cfg.backend), ncl_mode_name(cfg.mode),
           cfg.no_submutex ? "+no_submutex" : "", nnodes, nthreads, ncl_participants(),
           nlocks, iters, nwords, max_delay_ns, cfg.dev_lat_ns, precise ? " precise-hints" : "",
           cfg.debug_skip_data_wb ? " SKIP-WB" : "", cfg.debug_skip_data_inv ? " SKIP-INV" : "");
    fflush(stdout);

    pid_t *pids = calloc((size_t)nnodes, sizeof *pids);
    for (int n = 0; n < nnodes; n++) {
        pid_t p = fork();
        if (p < 0) {
            perror("fork");
            return 2;
        }
        if (p == 0)
            _exit(node_main(n));
        pids[n] = p;
    }
    ncl_uc_store(start_line(), 1);
    ncl_fence();

    int bad_exit = 0;
    for (int n = 0; n < nnodes; n++) {
        int status;
        waitpid(pids[n], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "node %d exited abnormally (status %#x)\n", n, status);
            bad_exit = 1;
        }
    }

    uint64_t acq = 0, viol = 0, ck = 0, sum = 0, final_ck_bad = 0;
    for (int n = 0; n < nnodes; n++) {
        acq += ncl_uc_load(res_line(n, RES_ACQ));
        viol += ncl_uc_load(res_line(n, RES_VIOL));
        ck += ncl_uc_load(res_line(n, RES_CKSUM));
    }
    for (int l = 0; l < nlocks; l++) {
        struct lockdata *d = ld(l);
        ncl_data_invalidate(d, stride);
        sum += d->counter;
        if (d->checksum != checksum(d))
            final_ck_bad++;
    }
    uint64_t expect = (uint64_t)nnodes * (uint64_t)nthreads * (uint64_t)iters;
    int ok = !bad_exit && viol == 0 && ck == 0 && final_ck_bad == 0 && sum == expect &&
             acq == expect;
    printf("total: expected=%lu acquired=%lu counter_sum=%lu mutex_violations=%lu "
           "checksum_errors=%lu final_bad_checksums=%lu -> %s\n",
           (unsigned long)expect, (unsigned long)acq, (unsigned long)sum, (unsigned long)viol,
           (unsigned long)ck, (unsigned long)final_ck_bad, ok ? "PASS" : "FAIL");
    ncl_sys_destroy();
    free(pids);
    return ok ? 0 : 1;
}
