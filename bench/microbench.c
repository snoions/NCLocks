/*
 * Lock microbenchmark (design §6.2), closed- or open-loop.
 *
 * Every request: pick a lock -> acquire -> touch `footprint` bytes of its data
 * and spin `cs` ns -> release.
 *
 * Closed loop (default, no -r): each thread issues its next request right
 * after the previous one (plus optional think time -T). The lock's speed sets
 * the request rate. Use it for throughput and scaling.
 *
 * Open loop (-r RATE): every thread gets a precomputed arrival schedule whose
 * aggregate mean is RATE requests/s, independent of how fast the lock is.
 * A thread that falls behind issues requests back to back; the backlog shows
 * up as latency because all latencies are measured from the SCHEDULED time
 * (no coordinated omission). Each thread is one worker with at most one
 * request in flight. Arrival processes:
 *   poisson                 exponential gaps
 *   uniform                 fixed gaps, random phase per thread
 *   onoff:PERIOD_US:FRAC    Poisson during the first FRAC of every PERIOD_US,
 *                           silent otherwise; bursts are aligned across all
 *                           nodes; the mean rate is still RATE
 *
 * Reported latencies (ns, log-linear buckets, <=6.25% overestimate):
 *   lock     scheduled -> acquired (the design's lock latency; open loop)
 *   service  attempt start -> acquired (what a closed loop measures)
 *   sojourn  scheduled -> released
 * In closed loop, scheduled == attempt start, so lock == service.
 *
 * Requests scheduled in the first -W seconds run but are not recorded. A run
 * stops at its end time even if behind; arrivals scheduled before the end but
 * never started are reported as `unserved` and mean the offered load was not
 * sustainable (their latency is not in the histograms).
 *
 * Nodes are separate processes (bench/harness.h): run it with
 * scripts/ncl_run.sh ncl_bench [options]. Node 0 prints the report.
 *
 * Not yet implemented: skew/phased access across nodes, node-local vs shared
 * access ratio.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#include "harness.h"
#include "hist.h"
#include "ncl/lock.h"
#include "ncl/cpus.h"

enum { ARR_CLOSED, ARR_POISSON, ARR_UNIFORM, ARR_ONOFF };
static const char *const arr_names[] = {"closed", "poisson", "uniform", "onoff"};
enum { H_LOCK, H_SERVICE, H_SOJOURN, H__COUNT };
static const char *const h_names[] = {"lock", "service", "sojourn"};

static int nnodes = 2, nthreads = 1, nlocks = 1, pin = 1, csv = 0;
static int srv_place = NCL_SRV_PIN_NEXT;
/* --active: nodes that run worker threads (bit n = node n); the others attach and
 * take part in the start barrier and detach, but never acquire. 0 = all. */
static uint64_t active_mask;
static int nactive;
static char active_desc[256] = "all";
static int node_active(int n) { return (active_mask >> n) & 1; }
static double secs = 2.0, warmup = 0.2, rate = 0;
static int arrival = ARR_CLOSED;
static double onoff_period_us = 1000, onoff_frac = 0.1;
static unsigned cs_ns = 0, think_ns = 0;
static size_t footprint = 64, stride;
static uint64_t cs_cycles, think_cycles;
static double cyc_per_ns;

struct result {
    uint64_t ops;      /* recorded requests (scheduled in measured window) */
    uint64_t completed; /* requests finishing inside the measured window,
                           whenever scheduled: the throughput numerator */
    uint64_t offered;  /* arrivals scheduled in the measured window */
    uint64_t unserved; /* of those, never started before the end */
    uint64_t paths[NCL_PATH__COUNT];
    struct ncl_base_stats base; /* node-side + server-side splitter counters */
    struct ncl_srv_stats srv;
    struct ncl_handoff_stats ho;
    struct hist h[H__COUNT];
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t xorshift(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

/* Seeds that differ in a few bits give nearly identical first xorshift
 * outputs (e.g. identical arrival phases on every thread), so every seed is
 * first scrambled with splitmix64. */
static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return (x ^ (x >> 31)) | 1;
}

static double unif01(uint64_t *s) /* (0, 1] */
{
    return ((double)(xorshift(s) >> 11) + 1.0) * (1.0 / 9007199254740992.0);
}

/* ---- arrival schedule ---------------------------------------------------
 * Time is in TSC cycles relative to the common start t0. For onoff, gaps are
 * drawn in "on-time" (time spent inside bursts) and mapped to real time. */
struct sched {
    uint64_t rng;
    double mean_gap; /* cycles, in on-time for onoff */
    double v;        /* current position in (on-)time */
    double period, on;
};

static void sched_init(struct sched *s, uint64_t seed)
{
    s->rng = splitmix64(seed);
    double per_thread = rate / (double)(nactive * nthreads); /* req/s */
    double gap = 1e9 * cyc_per_ns / per_thread;              /* cycles */
    if (arrival == ARR_ONOFF) {
        s->period = onoff_period_us * 1e3 * cyc_per_ns;
        s->on = s->period * onoff_frac;
        gap *= onoff_frac; /* higher rate inside bursts keeps the mean */
    }
    s->mean_gap = gap;
    s->v = unif01(&s->rng) * gap; /* random phase */
}

static uint64_t sched_next(struct sched *s)
{
    double gap = arrival == ARR_UNIFORM ? s->mean_gap : -log(unif01(&s->rng)) * s->mean_gap;
    double t = s->v;
    s->v += gap;
    if (arrival == ARR_ONOFF)
        t = floor(t / s->on) * s->period + fmod(t, s->on);
    return (uint64_t)t;
}

/* ---- worker -------------------------------------------------------------- */

struct worker {
    pthread_t tid;
    int node, idx;
    struct result r;
};

static void do_request(struct ncl_thread *t, uint64_t *rng, uint64_t *tacq)
{
    int l = nlocks > 1 ? (int)(xorshift(rng) % (uint64_t)nlocks) : 0;
    ncl_acquire(t, l);
    *tacq = __rdtsc();
    volatile uint64_t *d = ncl_data_ptr((ncl_off_t)l * stride);
    for (size_t k = 0; k < footprint / sizeof(uint64_t); k += NCL_CACHELINE / sizeof(uint64_t))
        d[k]++;
    if (cs_cycles)
        ncl_lat_spin_cycles(cs_cycles);
    ncl_release(l, NULL, 0);
}

static void record(struct result *r, uint64_t sched, uint64_t start, uint64_t acq, uint64_t done)
{
    r->ops++;
    hist_add(&r->h[H_LOCK], (uint64_t)((double)(acq - sched) / cyc_per_ns));
    hist_add(&r->h[H_SERVICE], (uint64_t)((double)(acq - start) / cyc_per_ns));
    hist_add(&r->h[H_SOJOURN], (uint64_t)((double)(done - sched) / cyc_per_ns));
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    struct result *r = &w->r;
    struct ncl_thread t;
    ncl_thread_init(&t, w->idx);
    uint64_t seed = ((uint64_t)w->node << 32) | (uint64_t)w->idx;
    uint64_t rng = splitmix64(seed);

    if (pin) {
        ncl_pin_self(ncl_cpu_slot(w->node * nthreads + w->idx));
    }

    uint64_t start_ns = harness_wait_start();
    while (now_ns() < start_ns)
        ncl_cpu_relax();
    const uint64_t t0 = __rdtsc(); /* TSC is invariant and shared on one host */
    const uint64_t twarm = t0 + (uint64_t)(warmup * 1e9 * cyc_per_ns);
    const uint64_t tend = t0 + (uint64_t)(secs * 1e9 * cyc_per_ns);

    if (arrival == ARR_CLOSED) {
        uint64_t now = t0;
        while (now < tend) {
            uint64_t start = __rdtsc(), acq;
            do_request(&t, &rng, &acq);
            uint64_t done = __rdtsc();
            if (done >= twarm && done < tend)
                r->completed++;
            if (start >= twarm) {
                record(r, start, start, acq, done);
                r->offered++;
            }
            if (think_cycles)
                ncl_lat_spin_cycles(think_cycles);
            now = __rdtsc();
        }
    } else {
        struct sched s;
        sched_init(&s, seed ^ 0x5bd1e9955bd1e995ull);
        uint64_t sched = t0 + sched_next(&s);
        while (sched < tend) {
            uint64_t now = __rdtsc();
            while (now < sched) { /* idle until the next arrival */
                ncl_cpu_relax();
                now = __rdtsc();
            }
            if (now >= tend)
                break; /* behind schedule at the end: rest is unserved */
            uint64_t acq;
            do_request(&t, &rng, &acq);
            uint64_t done = __rdtsc();
            if (done >= twarm && done < tend)
                r->completed++;
            if (sched >= twarm) {
                record(r, sched, now, acq, done);
                r->offered++;
            }
            sched = t0 + sched_next(&s);
        }
        for (; sched < tend; sched = t0 + sched_next(&s))
            if (sched >= twarm) {
                r->offered++;
                r->unserved++;
            }
    }
    return NULL;
}

static int node_main(int node)
{
    if (ncl_node_attach(node) < 0)
        return 2;
    const int nt = node_active(node) ? nthreads : 0;
    struct worker *ws = calloc((size_t)nthreads, sizeof *ws);
    for (int i = 0; i < nt; i++) {
        ws[i].node = node;
        ws[i].idx = i;
        pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]);
    }
    harness_start();
    struct result *tot = calloc(1, sizeof *tot);
    for (int i = 0; i < nt; i++) {
        pthread_join(ws[i].tid, NULL);
        tot->ops += ws[i].r.ops;
        tot->completed += ws[i].r.completed;
        tot->offered += ws[i].r.offered;
        tot->unserved += ws[i].r.unserved;
        for (int h = 0; h < H__COUNT; h++)
            hist_merge(&tot->h[h], &ws[i].r.h[h]);
    }
    ncl_node_detach(); /* all nodes done; servers stopped */
    struct ncl_node_stats st;
    ncl_node_stats(&st);
    memcpy(tot->paths, st.path, sizeof tot->paths);
    tot->base = st.base;
    ncl_base_stats_add(&tot->base, &st.srv.base);
    tot->srv = st.srv;
    tot->ho = st.ho;
    harness_put_result(tot);
    free(tot);
    free(ws);
    return 0;
}

static double parse_rate(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    if (*end == 'k' || *end == 'K') v *= 1e3;
    else if (*end == 'm' || *end == 'M') v *= 1e6;
    return v;
}

static int parse_arrival(const char *s)
{
    if (!strcmp(s, "poisson")) arrival = ARR_POISSON;
    else if (!strcmp(s, "uniform")) arrival = ARR_UNIFORM;
    else if (!strncmp(s, "onoff", 5)) {
        arrival = ARR_ONOFF;
        if (s[5] == ':' && sscanf(s + 6, "%lf:%lf", &onoff_period_us, &onoff_frac) != 2)
            return -1;
        if (onoff_period_us <= 0 || onoff_frac <= 0 || onoff_frac > 1)
            return -1;
    } else
        return -1;
    return 0;
}

/* "0,8" or "0-3,8": node ids < 64. Sets active_mask; 0 on success. */
static int parse_active(const char *s)
{
    const char *p = s;
    uint64_t m = 0;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            return -1;
        if (*end == '-') {
            p = end + 1;
            b = strtol(p, &end, 10);
            if (end == p)
                return -1;
        }
        if (a < 0 || b < a || b >= NCL_MAX_NODES)
            return -1;
        for (long i = a; i <= b; i++)
            m |= 1ull << i;
        p = end;
        if (*p == ',')
            p++;
        else if (*p)
            return -1;
    }
    active_mask = m;
    return m ? 0 : -1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -n N        nodes (default 2)            -t T   threads per node (default 1)\n"
            "  -l L        locks (default 1)            -a ALG lamport|tournament|hybrid|server|fastsrv|ak|aksrv|reactive|bakery|pthread\n"
            "  -b BACKEND  coherent|cached-flush|uc|emu (default cached-flush)\n"
            "  -L NS       injected device latency      -m     no submutex (ablation)\n"
            "  -s SECS     duration (default 2)         -W SECS warmup, not recorded (0.2)\n"
            "  -c NS       critical-section length      -f B   data footprint (default 64)\n"
            "  -T NS       think time (closed loop only)\n"
            "  -r RATE     OPEN LOOP: aggregate offered load, req/s (k/M suffix ok)\n"
            "  -A PROC     arrivals: poisson (default) | uniform | onoff:PERIOD_US:FRAC\n"
            "  -C          print one CSV row (see -H for the header)\n"
            "  -H          print the CSV header and exit\n"
            "  -P          don't pin threads\n"
            "  -S N        server idle poll backoff cap, pause iterations (server/fastsrv)\n"
            "  -M MODE     phase1 (default) | local (2a cohort passing) | handoff (2b: local +\n"
            "              gate/owner cross-node handoff; lamport, tournament, hybrid; server:\n"
            "              holder passes to the server-published successor)\n"
            "  -K CAP      local/handoff: max consecutive local handoffs (default 64)\n"
            "  -G PLACE    handoff: announce before (announce, fence, gate; default) | inside\n"
            "              (spec baseline)\n"
            "  -X PICK     handoff release: adaptive (probe one slot if this node's acquisition\n"
            "              was uncontended, else scan; default) | scan (spec).\n"
            "              server: adaptive = read succ[] only if the grant's hint bit is set;\n"
            "              scan = always read\n"
            "  --wait W    handoff, gate algorithms: waiters poll their own announce line, granted\n"
            "              by the releaser (own, default) or the shared owner word (owner)\n"
            "  --ho LIST   handoff, gate algorithms: shorter handoff path, all (default: the options\n"
            "              that apply; grant needs --wait own, skip -G before) | none |\n"
            "              comma list of grant (owner FREE/HELD, the grant alone hands off; needs\n"
            "              --wait own), clear (announce cleared at release), skip (read owner\n"
            "              before the gate, enter it only if FREE; -G before); docs/handoff.md\n"
            "  -R LINES    server handoff: chain window = succ[] lines per lock (power of two;\n"
            "              default 4N rounded up)\n"
            "  -O on|off   handoff: overlap the release's data writeback with its loads (on)\n"
            "  -Q MODE     multi-line metadata reads (scans, server poll): batch (cached-flush:\n"
            "              flush all, one mfence, parallel loads) | probe (scans: first candidate\n"
            "              alone, batch the rest on a miss; default) | serial\n"
            "  -U LAYOUT   metadata layout: uniform (default) | split (uc, emu: scanned arrays\n"
            "              b[], present[], announce[], server queues in a cached region with\n"
            "              batched reads; single words stay uncached)\n"
            "  -B N        spin-wait backoff cap for base algorithms, pause iterations (1024)\n"
            "  -Y PLACE    server thread placement: next (CPU N*T+n, default) | sibling\n"
            "              (SMT sibling of node n's first worker)\n"
            "  -F MODE     ak: fair (default; reopen only if no slow waiter at the top\n"
            "              lock) | always (Anderson-Kim as published; unfair)\n"
            "  --mem-node N bind the shared regions (metadata, scan, data) to NUMA node N\n"
            "                and verify every page (uc: driver must be loaded with nid=N)\n"
            "  --cpus LIST   CPUs for pinned threads, e.g. 0-59,120-179 (default: this\n"
            "                process's affinity, e.g. from numactl --cpunodebind)\n"
            "  --wb MODE     writeback instruction: clflushopt (default) | clwb\n"
            "  --bakery-arity K  -a bakery: tree of K-way nodes (0 = one flat node, default)\n"
            "  --bakery-poll pred|all  -a bakery wait: poll the predecessor (default) or re-read\n"
            "                every blocker on each poll (ablation)\n"
            "  --bakery-wait classic|strict  -a bakery: a blocker once seen not choosing is\n"
            "                waited on by its ticket only (classic, default), or waited on\n"
            "                while choosing on every read (strict)\n"
            "  --retain      tournament: prefix retention (design.md §4g g1; phase1/local): keep\n"
            "                the levels below the lowest one with a waiter; a per-node\n"
            "                sweeper drops them on a waiter or lease expiry\n"
            "  --retain-check both|acquire|release|none  where kept levels are checked for\n"
            "                waiters besides the sweeper (acquire)\n"
            "  --retain-poll US  sweeper poll period (10; 0 = yield)\n"
            "  --retain-lease US  drop kept levels this long after they were kept (1000)\n"
            "  --retain-max N  at most N locks with kept levels per node (0 = no limit)\n"
            "  --retain-probe P  learned keep height: a waiter on level w caps the kept\n"
            "                levels at w-1; probe one level higher after P releases, doubling\n"
            "                per lowering (16; 0 = always keep depth-1 levels)\n"
            "  --hot K       -a tournament --retain: hot-slot subtree with K slots (design.md\n"
            "                §4g g2): nodes whose g1 cap shows contenders meeting low claim a\n"
            "                slot and acquire through a K-leaf subtree under a new root R\n"
            "  --hot-window US  admission: last 4 gaps between acquisitions within US (100)\n"
            "  --hot-lease US  free a slot unused for US (250; piggybacked on releases)\n"
            "  --hot-admit cap|always  admission test: g1's cap (default) or none\n"
            "  --hot-probe   -a tournament: read one more line (hot_mode, never set) in the\n"
            "                root's entry check: the cost of g2's root bypass (design.md §4g)\n"
            "  --active LIST only these nodes run workers, e.g. 0,8 or 0-3 (default all); the\n"
            "                others take part in the lock structure but never acquire.\n"
            "                Fairness and open-loop rates count active nodes only\n"
            "  --react U:D[:W] -a reactive: to the tournament when >= U of the last 16 global\n"
            "                acquisitions met another node, back when <= D of the last 64, after >= W\n"
            "                acquisitions on the tournament (8:4:64)\n"
            HARNESS_USAGE,
            argv0, argv0);
}

#define CSV_HEADER                                                                         \
    "alg,backend,nodes,threads_per_node,locks,cs_ns,think_ns,footprint,dev_lat_ns,"        \
    "no_submutex,mode,local_cap,spin_backoff,server_backoff,arrival,target_rps,offered_rps,achieved_rps,unserved_frac,"                          \
    "lock_mean,lock_p50,lock_p90,lock_p99,lock_p999,lock_max,"                             \
    "svc_mean,svc_p50,svc_p99,svc_p999,soj_p50,soj_p99,soj_p999,"                          \
    "fast_frac,meta_memtype,meta_load_ns,node_min_max_ratio,server_place,scan_loads_per_acq,lamport_retries_per_acq,fast_release,announce,pick,global_handoff_frac,local_handoff_frac,missed_announce_per_acq,owner_polls_per_acq,gate_entries_per_acq,announce_loads_per_acq,chain_window,meta_batch,wb_overlap,meta_layout,mem_node,uc_pool_node,wb,cpus,wait,grant_polls_per_acq,ho_opts,gate_skips_per_acq,bakery_arity,bakery_poll,bakery_wait,active,retain,ret_hits_per_acq,ret_skipped_per_acq,ret_drops_per_acq,ret_sweep_drops_per_acq,ret_policy_skips_per_acq,ret_cap_lowers_per_acq,ret_cap_raises_per_acq,hot_probe,hot,hot_path_frac,hot_r_frac,hot_claims_per_acq,hot_frees_per_acq"

int main(int argc, char **argv)
{
    struct ncl_sys_cfg cfg = {
        .backend = NCL_BACKEND_CACHED_FLUSH,
        .alg = NCL_ALG_LAMPORT,
        .mode = NCL_MODE_PHASE1,
        .handoff_opts = NCL_HO_DEFAULT,
    };
    int c, arrival_given = 0;
    static struct option lopts[] = {
        {"mem-node", required_argument, 0, 1001},
        {"cpus", required_argument, 0, 1002},
        {"wb", required_argument, 0, 1003},
        {"react", required_argument, 0, 1006},
        {"bakery-poll", required_argument, 0, 1007},
        {"bakery-arity", required_argument, 0, 1008},
        {"bakery-wait", required_argument, 0, 1009},
        {"active", required_argument, 0, 1010},
        {"retain", no_argument, 0, 1011},
        {"retain-check", required_argument, 0, 1012},
        {"retain-poll", required_argument, 0, 1013},
        {"retain-lease", required_argument, 0, 1014},
        {"retain-max", required_argument, 0, 1015},
        {"retain-probe", required_argument, 0, 1016},
        {"hot-probe", no_argument, 0, 1017},
        {"hot", required_argument, 0, 1018},
        {"hot-window", required_argument, 0, 1019},
        {"hot-lease", required_argument, 0, 1020},
        {"hot-admit", required_argument, 0, 1021},
        {"wait", required_argument, 0, 1004},
        {"ho", required_argument, 0, 1005},
        {"help", no_argument, 0, 'h'},
        HARNESS_LONGOPTS,
        {0, 0, 0, 0},
    };
    int wait_set = 0;
    while ((c = getopt_long(argc, argv, "n:t:l:a:b:L:s:W:c:T:f:r:A:S:M:K:B:Y:F:G:X:R:Q:O:U:mCHPh", lopts,
                            NULL)) != -1) {
        switch (c) {
        case 'n': nnodes = atoi(optarg); break;
        case 't': nthreads = atoi(optarg); break;
        case 'l': nlocks = atoi(optarg); break;
        case 'a': if (ncl_alg_parse(optarg, &cfg.alg) < 0) { usage(argv[0]); return 2; } break;
        case 'b': if (ncl_backend_parse(optarg, &cfg.backend) < 0) { usage(argv[0]); return 2; } break;
        case 'L': cfg.dev_lat_ns = (uint32_t)atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        case 'W': warmup = atof(optarg); break;
        case 'c': cs_ns = (unsigned)atoi(optarg); break;
        case 'T': think_ns = (unsigned)atoi(optarg); break;
        case 'f': footprint = (size_t)atol(optarg); break;
        case 'r': rate = parse_rate(optarg); break;
        case 'A':
            if (parse_arrival(optarg) < 0) { usage(argv[0]); return 2; }
            arrival_given = 1;
            break;
        case 'm': cfg.no_submutex = 1; break;
        case 'M': if (ncl_mode_parse(optarg, &cfg.mode) < 0) { usage(argv[0]); return 2; } break;
        case 'K': cfg.local_cap = atoi(optarg); break;
        case 'R': cfg.chain_lines = atoi(optarg); break;
        case 'O': ncl_wb_overlap = strcmp(optarg, "off") != 0; break;
        case 'Q':
            if (!strcmp(optarg, "batch"))
                ncl_meta_batch = NCL_META_BATCH;
            else if (!strcmp(optarg, "serial"))
                ncl_meta_batch = NCL_META_SERIAL;
            else if (!strcmp(optarg, "probe"))
                ncl_meta_batch = NCL_META_PROBE;
            else {
                fprintf(stderr, "-Q: batch|probe|serial\n");
                return 2;
            }
            break;
        case 'U':
            if (!strcmp(optarg, "split"))
                cfg.meta_split = 1;
            else if (strcmp(optarg, "uniform")) {
                fprintf(stderr, "-U: uniform|split\n");
                return 2;
            }
            break;
        case 'B': ncl_backoff_cap = (unsigned)atoi(optarg); break;
        case 'G':
            if (!strcmp(optarg, "inside"))
                cfg.announce_place = NCL_ANNOUNCE_INSIDE;
            else if (!strcmp(optarg, "before"))
                cfg.announce_place = NCL_ANNOUNCE_BEFORE;
            else {
                fprintf(stderr, "-G: before|inside\n");
                return 2;
            }
            break;
        case 'X':
            if (!strcmp(optarg, "scan"))
                cfg.pick_policy = NCL_PICK_SCAN;
            else if (!strcmp(optarg, "adaptive"))
                cfg.pick_policy = NCL_PICK_ADAPTIVE;
            else {
                fprintf(stderr, "-X: adaptive|scan\n");
                return 2;
            }
            break;
        case 'F':
            if (!strcmp(optarg, "fair"))
                ncl_fast_release = NCL_FASTREL_SCAN;
            else if (!strcmp(optarg, "always"))
                ncl_fast_release = NCL_FASTREL_ALWAYS;
            else {
                fprintf(stderr, "-F: fair|always (ak only)\n");
                return 2;
            }
            break;
        case 'C': csv = 1; break;
        case 'H': printf("%s\n", CSV_HEADER); return 0;
        case 'P': pin = 0; break;
        case 1001: cfg.mem_node = atoi(optarg) + 1; break;
        case 1002: if (ncl_cpus_set(optarg) < 0) return 2; break;
        case 1003:
            if (!strcmp(optarg, "clwb")) { if (ncl_wb_mode_set(NCL_WB_CLWB) < 0) return 2; }
            else if (strcmp(optarg, "clflushopt")) { usage(argv[0]); return 2; }
            break;
        case 1008: ncl_bakery_arity = atoi(optarg); break;
        case 1009:
            if (!strcmp(optarg, "classic")) ncl_bakery_wait_classic = 1;
            else if (!strcmp(optarg, "strict")) ncl_bakery_wait_classic = 0;
            else { fprintf(stderr, "--bakery-wait: strict|classic\n"); return 2; }
            break;

        case 1011: cfg.retain = 1; break;
        case 1012: if (ncl_retain_opt("check", optarg) < 0) return 2; break;
        case 1013: if (ncl_retain_opt("poll", optarg) < 0) return 2; break;
        case 1014: if (ncl_retain_opt("lease", optarg) < 0) return 2; break;
        case 1015: if (ncl_retain_opt("max", optarg) < 0) return 2; break;
        case 1016: if (ncl_retain_opt("probe", optarg) < 0) return 2; break;
        case 1017: ncl_tour_hot_probe = 1; break;
        case 1018: if (ncl_hot_opt(&cfg, "slots", optarg) < 0) return 2; break;
        case 1019: if (ncl_hot_opt(&cfg, "window", optarg) < 0) return 2; break;
        case 1020: if (ncl_hot_opt(&cfg, "lease", optarg) < 0) return 2; break;
        case 1021: if (ncl_hot_opt(&cfg, "admit", optarg) < 0) return 2; break;
        case 1010:
            if (parse_active(optarg) < 0) {
                fprintf(stderr, "--active: node list, e.g. 0,8 or 0-3\n");
                return 2;
            }
            snprintf(active_desc, sizeof active_desc, "%s", optarg);
            for (char *q = active_desc; *q; q++) /* CSV-safe */
                if (*q == ',')
                    *q = ';';
            break;
        case 1007:
            if (!strcmp(optarg, "all")) ncl_bakery_poll_all = 1;
            else if (!strcmp(optarg, "pred")) ncl_bakery_poll_all = 0;
            else { fprintf(stderr, "--bakery-poll: pred|all\n"); return 2; }
            break;
        case 1006:
            if (ncl_react_parse(optarg) < 0) { usage(argv[0]); return 2; }
            break;
        case 1004:
            if (!strcmp(optarg, "own")) cfg.handoff_wait = NCL_WAIT_OWN;
            else if (!strcmp(optarg, "owner")) cfg.handoff_wait = NCL_WAIT_OWNER;
            else { fprintf(stderr, "--wait: owner|own\n"); return 2; }
            wait_set = 1;
            break;
        case 1005:
            if (ncl_ho_opts_parse(optarg, &cfg.handoff_opts) < 0) {
                fprintf(stderr, "--ho: none|all|grant,clear,skip\n");
                return 2;
            }
            break;
        case 'S': cfg.server_backoff_cap = (unsigned)atoi(optarg); break;
        case 'Y':
            if (!strcmp(optarg, "next"))
                srv_place = NCL_SRV_PIN_NEXT;
            else if (!strcmp(optarg, "sibling"))
                srv_place = NCL_SRV_PIN_SIBLING;
            else {
                fprintf(stderr, "-Y: next|sibling\n");
                return 2;
            }
            break;
        default: {
            int h = harness_opt(c, optarg);
            if (h > 0)
                break;
            if (h == 0)
                usage(argv[0]);
            return c == 'h' ? 0 : 2;
        }
        }
    }
    if (harness_check(argc, argv, "ncl_bench") < 0)
        return 2;
    if (wait_set && (cfg.mode != NCL_MODE_HANDOFF || cfg.alg == NCL_ALG_SERVER)) {
        fprintf(stderr, "--wait applies to -M handoff with a gate algorithm (lamport, tournament, "
                        "hybrid, ak); the server chain already waits on its own slot\n");
        return 2;
    }
    if (rate > 0 && !arrival_given)
        arrival = ARR_POISSON;
    if (arrival != ARR_CLOSED && rate <= 0) {
        fprintf(stderr, "-A needs an offered load (-r RATE)\n");
        return 2;
    }
    if (arrival != ARR_CLOSED && think_ns) {
        fprintf(stderr, "-T (think time) only applies to closed loop; use -r for load\n");
        return 2;
    }
    if (warmup >= secs) {
        fprintf(stderr, "warmup (-W) must be shorter than the run (-s)\n");
        return 2;
    }

    if (nnodes < 1 || nnodes > NCL_MAX_NODES) {
        fprintf(stderr, "-n: 1..%d\n", NCL_MAX_NODES);
        return 2;
    }
    if (ncl_tour_hot_probe && (cfg.alg != NCL_ALG_TOURNAMENT || cfg.retain || cfg.alg_per_lock)) {
        fprintf(stderr, "--hot-probe: plain -a tournament only (no --retain)\n");
        return 2;
    }
    if (!active_mask)
        active_mask = nnodes == 64 ? ~0ull : (1ull << nnodes) - 1;
    if (active_mask >> (nnodes - 1) >> 1) {
        fprintf(stderr, "--active: node ids must be < -n (%d)\n", nnodes);
        return 2;
    }
    nactive = __builtin_popcountll(active_mask);

    stride = (footprint + NCL_CACHELINE - 1) & ~(size_t)(NCL_CACHELINE - 1);
    if (!stride)
        stride = NCL_CACHELINE;
    cfg.nnodes = nnodes;
    cfg.threads_per_node = nthreads;
    cfg.nlocks = nlocks;
    cfg.data_size = stride * (size_t)nlocks;
    cfg.server_pin = pin ? srv_place : NCL_SRV_PIN_NONE;
    if (harness.role == HR_QUERY)
        return harness_query(nnodes, cfg.backend, 0);
    if (harness_open(&cfg, sizeof(struct result)) < 0)
        return 2;
    if (harness.role == HR_INIT) {
        if (pin) {
            int srv = cfg.alg == NCL_ALG_SERVER || ncl_alg_fastsrv_like(cfg.alg);
            ncl_cpus_check(nnodes * nthreads + (srv && srv_place == NCL_SRV_PIN_NEXT ? nnodes : 0),
                           nnodes, nthreads, srv && srv_place == NCL_SRV_PIN_SIBLING, active_mask);
        }
        for (int l = 0; l < nlocks; l++)
            if (footprint && ncl_lock_set_data(l, (ncl_off_t)l * stride, footprint) < 0)
                return 2;
        ncl_sys_publish();
        ncl_sys_destroy();
        return 0;
    }
    cs_cycles = ncl_ns_to_cycles(cs_ns);
    think_cycles = ncl_ns_to_cycles(think_ns);
    cyc_per_ns = (double)ncl_ns_to_cycles(1000000) / 1e6;

    int rc = node_main(harness.node);
    if (rc || harness.node != 0) {
        ncl_sys_destroy();
        return rc;
    }

    /* Node 0: gather every node's result and report. */
    struct result *tot = calloc(1, sizeof *tot), *nr = malloc(sizeof *nr);
    uint64_t node_done[NCL_MAX_NODES] = {0}; /* per-node completions: fairness */
    for (int n = 0; n < nnodes; n++) {
        harness_get_result(n, nr);
        tot->ops += nr->ops;
        tot->completed += nr->completed;
        node_done[n] = nr->completed;
        tot->offered += nr->offered;
        tot->unserved += nr->unserved;
        for (int p = 0; p < NCL_PATH__COUNT; p++)
            tot->paths[p] += nr->paths[p];
        ncl_base_stats_add(&tot->base, &nr->base);
        for (size_t k = 0; k < NCL_HANDOFF_STATS_N; k++)
            ((uint64_t *)&tot->ho)[k] += ((const uint64_t *)&nr->ho)[k];
        {
            uint64_t *d = (uint64_t *)&tot->srv;
            const uint64_t *s = (const uint64_t *)&nr->srv;
            for (size_t k = 0; k < sizeof tot->srv / sizeof(uint64_t); k++)
                d[k] += s[k];
        }
        for (int h = 0; h < H__COUNT; h++)
            hist_merge(&tot->h[h], &nr->h[h]);
    }

    double window = secs - warmup;
    /* Completions inside the window. Counting only requests *scheduled* in it
     * would undercount when saturated, since warmup backlog drains first. */
    double achieved = (double)tot->completed / window;
    double offered = arrival == ARR_CLOSED ? achieved : (double)tot->offered / window;
    double unserved = tot->offered ? (double)tot->unserved / (double)tot->offered : 0;
    uint64_t nmin = UINT64_MAX, nmax = 0;
    for (int n = 0; n < nnodes; n++) {
        if (!node_active(n))
            continue;
        if (node_done[n] < nmin) nmin = node_done[n];
        if (node_done[n] > nmax) nmax = node_done[n];
    }
    /* 1.0 = perfectly even across nodes; 0 = some node starved */
    double fair = nmax ? (double)nmin / (double)nmax : 1.0;
    uint64_t all_paths = 0, fast = 0;
    for (int p = 0; p < NCL_PATH__COUNT; p++)
        all_paths += tot->paths[p];
    fast = tot->paths[NCL_PATH_LAMPORT_FAST] + tot->paths[NCL_PATH_HYBRID_FAST] +
           tot->paths[NCL_PATH_FASTSRV_FAST];
    const struct hist *hl = &tot->h[H_LOCK], *hs = &tot->h[H_SERVICE], *hj = &tot->h[H_SOJOURN];
#define ANN_NAME(a) ((a) == NCL_ANNOUNCE_INSIDE ? "inside" : "before")
#define PICK_NAME(x) ((x) == NCL_PICK_SCAN ? "scan" : "adaptive")
#define WAIT_NAME(w) ((w) == NCL_WAIT_OWN ? "own" : "owner")
#define PER(x) (all_paths ? (double)(x) / (double)all_paths : 0.0)
    char arrdesc[64];
    if (arrival == ARR_ONOFF)
        snprintf(arrdesc, sizeof arrdesc, "onoff:%g:%g", onoff_period_us, onoff_frac);
    else
        snprintf(arrdesc, sizeof arrdesc, "%s", arr_names[arrival]);

    char ho_buf[32];
    const char *ho_desc = cfg.mode != NCL_MODE_HANDOFF || cfg.alg == NCL_ALG_SERVER
                              ? "-" : ncl_ho_opts_name(ncl_sys_cfg()->handoff_opts, ho_buf);
    char cpus_csv[512]; /* the CPU list with ';' for ',' (CSV-safe) */
    snprintf(cpus_csv, sizeof cpus_csv, "%s", ncl_cpus_describe());
    for (char *q = cpus_csv; *q; q++)
        if (*q == ',')
            *q = ';';
    char ret_buf[64], hot_buf[64];
    char bak_arity[16] = "-"; /* BAKERY: 0 = flat */
    if (cfg.alg == NCL_ALG_BAKERY)
        snprintf(bak_arity, sizeof bak_arity, "%d", ncl_bakery_arity);
    if (csv) {
        printf("%s,%s,%d,%d,%d,%u,%u,%zu,%u,%d,%s,%d,%u,%u,%s,%.0f,%.0f,%.0f,%.5f,"
               "%.0f,%lu,%lu,%lu,%lu,%lu,%.0f,%lu,%lu,%lu,%lu,%lu,%lu,%.4f,%s,%.1f,%.3f,%s,%.3f,%.3f,%s,%s,%s,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%d,%s,%s,%s,%d,%d,%s,%s,%s,%.3f,%s,%.3f,%s,%s,%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s,%.4f,%.4f,%.5f,%.5f\n",
               ncl_alg_name(cfg.alg), ncl_backend_name(cfg.backend), nnodes, nthreads, nlocks,
               cs_ns, think_ns, footprint, cfg.dev_lat_ns, cfg.no_submutex, ncl_mode_name(cfg.mode),
               cfg.mode != NCL_MODE_PHASE1 ? (cfg.local_cap > 0 ? cfg.local_cap : 64) : 0, ncl_backoff_cap,
               cfg.server_backoff_cap ? cfg.server_backoff_cap : 1024, arrdesc, rate, offered,
               achieved, unserved, hist_mean(hl), (unsigned long)hist_pct(hl, 0.5),
               (unsigned long)hist_pct(hl, 0.9), (unsigned long)hist_pct(hl, 0.99),
               (unsigned long)hist_pct(hl, 0.999), (unsigned long)hl->max, hist_mean(hs),
               (unsigned long)hist_pct(hs, 0.5), (unsigned long)hist_pct(hs, 0.99),
               (unsigned long)hist_pct(hs, 0.999), (unsigned long)hist_pct(hj, 0.5),
               (unsigned long)hist_pct(hj, 0.99), (unsigned long)hist_pct(hj, 0.999),
               all_paths ? (double)fast / (double)all_paths : 0.0, ncl_mem.meta_memtype,
               ncl_mem.meta_load_ns, fair,
               !pin ? "none" : srv_place == NCL_SRV_PIN_SIBLING ? "sibling" : "next",
               all_paths ? (double)tot->base.split_scan_loads / (double)all_paths : 0.0,
               all_paths ? (double)tot->base.lamport_retries / (double)all_paths : 0.0,
               ncl_fast_release == NCL_FASTREL_HINT ? "hint" : ncl_fast_release == NCL_FASTREL_ALWAYS ? "always" : "scan",
               cfg.mode != NCL_MODE_HANDOFF || cfg.alg == NCL_ALG_SERVER ? "-" : ANN_NAME(cfg.announce_place),
               cfg.mode != NCL_MODE_HANDOFF ? "-" : PICK_NAME(cfg.pick_policy),
               PER(tot->paths[NCL_PATH_GLOBAL_HANDOFF]), PER(tot->paths[NCL_PATH_LOCAL_HANDOFF]),
               PER(tot->ho.missed_announce), PER(tot->ho.owner_polls), PER(tot->ho.gate_entries),
               PER(tot->ho.announce_loads),
               ncl_chain_window(), !ncl_meta_batching() ? "serial" : ncl_meta_batch == NCL_META_PROBE ? "probe" : "batch",
               cfg.mode != NCL_MODE_HANDOFF ? "-" : ncl_wb_overlap ? "on" : "off",
               cfg.meta_split ? "split" : "uniform", ncl_mem_node(),
               cfg.backend == NCL_BACKEND_UC ? ncl_mem.uc_pool_node : -1, ncl_wb_mode_name(),
               pin ? cpus_csv : "none",
               cfg.mode != NCL_MODE_HANDOFF || cfg.alg == NCL_ALG_SERVER ? "-" : WAIT_NAME(cfg.handoff_wait),
               PER(tot->ho.grant_polls), ho_desc, PER(tot->ho.gate_skips), bak_arity,
               cfg.alg != NCL_ALG_BAKERY ? "-" : ncl_bakery_poll_all ? "all" : "pred",
               cfg.alg != NCL_ALG_BAKERY ? "-" : ncl_bakery_wait_classic ? "classic" : "strict",
               active_desc, ncl_retain_describe(ret_buf, sizeof ret_buf),
               PER(tot->base.ret_hits), PER(tot->base.ret_levels_skipped),
               PER(tot->base.ret_acq_drops + tot->base.ret_rel_drops + tot->base.ret_sweep_drops +
                   tot->base.ret_lease_drops),
               PER(tot->base.ret_sweep_drops + tot->base.ret_lease_drops),
               PER(tot->base.ret_policy_skips), PER(tot->base.ret_cap_lowers),
               PER(tot->base.ret_cap_raises), ncl_tour_hot_probe,
               ncl_hot_describe(hot_buf, sizeof hot_buf), PER(tot->base.hot_acqs),
               PER(tot->base.hot_r_acqs), PER(tot->base.hot_claims),
               PER(tot->base.hot_free_fair + tot->base.hot_free_lease));
    } else {
        printf("alg=%s backend=%s mode=%s%s nodes=%d threads/node=%d locks=%d cs=%uns "
               "footprint=%zuB lat=%uns secs=%.2f warmup=%.2f\n",
               ncl_alg_name(cfg.alg), ncl_backend_name(cfg.backend), ncl_mode_name(cfg.mode),
               cfg.no_submutex ? "+no_submutex" : "", nnodes, nthreads, nlocks, cs_ns, footprint,
               cfg.dev_lat_ns, secs, warmup);
        if (nactive < nnodes)
            printf("active nodes: %s (%d of %d)\n", active_desc, nactive, nnodes);
        printf("mem: %s\n", ncl_mem_describe());
        printf("cpus: %s\n", pin ? ncl_cpus_describe() : "unpinned");
        if (arrival == ARR_CLOSED)
            printf("loop=closed think=%uns throughput=%.0f req/s\n", think_ns, achieved);
        else
            printf("loop=open arrival=%s offered=%.0f req/s achieved=%.0f req/s "
                   "unserved=%.2f%%%s\n",
                   arrdesc, offered, achieved, 100 * unserved,
                   unserved > 0.001 ? "  ** SATURATED: offered load not sustained **" : "");
        printf("latency ns   %8s %8s %8s %8s %8s %10s\n", "mean", "p50", "p90", "p99", "p99.9",
               "max");
        for (int h = 0; h < H__COUNT; h++) {
            const struct hist *x = &tot->h[h];
            printf("  %-9s  %8.0f %8lu %8lu %8lu %8lu %10lu\n", h_names[h], hist_mean(x),
                   (unsigned long)hist_pct(x, 0.5), (unsigned long)hist_pct(x, 0.9),
                   (unsigned long)hist_pct(x, 0.99), (unsigned long)hist_pct(x, 0.999),
                   (unsigned long)x->max);
        }
        printf("per-node completions (min/max = %.3f%s):", fair,
               nactive < nnodes ? " over active nodes" : "");
        for (int n = 0; n < nnodes; n++)
            printf(" %lu", (unsigned long)node_done[n]);
        printf("\n");
        printf("paths:");
        for (int p = 0; p < NCL_PATH__COUNT; p++)
            if (tot->paths[p])
                printf(" %s=%.1f%%", ncl_path_names[p],
                       100.0 * (double)tot->paths[p] / (double)all_paths);
        printf("\n");
        const struct ncl_base_stats *b = &tot->base;
        if (cfg.retain && all_paths) {
            double A = (double)all_paths;
            printf("retain %s: hits=%.1f%% levels_skipped/acq=%.2f drops/acq: acquire=%.3f "
                   "release=%.3f sweeper=%.3f lease=%.3f cap_skips=%.3f policy_skips=%.3f cap_lowers=%.4f cap_raises=%.4f sweeper_busy=%lu polls=%lu\n",
                   ncl_retain_describe(ret_buf, sizeof ret_buf), 100 * (double)b->ret_hits / A,
                   (double)b->ret_levels_skipped / A, (double)b->ret_acq_drops / A,
                   (double)b->ret_rel_drops / A, (double)b->ret_sweep_drops / A,
                   (double)b->ret_lease_drops / A, (double)b->ret_cap_skips / A,
                   (double)b->ret_policy_skips / A, (double)b->ret_cap_lowers / A,
                   (double)b->ret_cap_raises / A,
                   (unsigned long)b->ret_sweep_busy, (unsigned long)b->ret_sweep_polls);
        }
        if (cfg.hot_slots && all_paths) {
            double A = (double)all_paths;
            printf("hot %s: hot=%.1f%% cold_via_R=%.1f%% R_waits=%.3f claims/acq=%.5f full=%lu "
                   "frees: fair=%lu lease=%lu hot_mode sets=%lu clears=%lu (checks %lu)\n",
                   ncl_hot_describe(hot_buf, sizeof hot_buf), 100 * (double)b->hot_acqs / A,
                   100 * (double)b->hot_r_acqs / A, (double)b->hot_r_waits / A,
                   (double)b->hot_claims / A, (unsigned long)b->hot_full,
                   (unsigned long)b->hot_free_fair, (unsigned long)b->hot_free_lease,
                   (unsigned long)b->hot_mode_sets, (unsigned long)b->hot_mode_clears,
                   (unsigned long)b->hot_clear_checks);
        }
        if ((cfg.alg == NCL_ALG_AK || cfg.alg == NCL_ALG_AKSRV) && all_paths) {
            double A = (double)all_paths;
            printf("ak: slow1(Y closed)=%.1f%% slow2(deflected)=%.1f%%  reopens=%lu "
                   "reopen_blocked=%lu loads/check=%.2f\n",
                   100 * (double)b->split_fail_closed / A, 100 * (double)b->split_fail_lost / A,
                   (unsigned long)b->split_resets, (unsigned long)b->split_reset_skips,
                   b->split_resets + b->split_reset_skips
                       ? (double)b->split_scan_loads / (double)(b->split_resets + b->split_reset_skips)
                       : 0.0);
        }
        if ((cfg.alg == NCL_ALG_HYBRID || cfg.alg == NCL_ALG_FASTSRV) && all_paths) {
            double A = (double)all_paths;
            printf("splitter: deflected door_closed=%.1f%% lost_race=%.1f%%  door_resets=%lu "
                   "reset_skips=%lu (precheck %lu) fast_kept_closed=%lu present_loads/check=%.2f\n",
                   100 * (double)b->split_fail_closed / A, 100 * (double)b->split_fail_lost / A,
                   (unsigned long)b->split_resets, (unsigned long)b->split_reset_skips,
                   (unsigned long)b->split_precheck_skips, (unsigned long)b->split_fast_keep_closed,
                   b->split_resets + b->split_reset_skips
                       ? (double)b->split_scan_loads / (double)(b->split_resets + b->split_reset_skips)
                       : 0.0);
        }
        if (cfg.alg == NCL_ALG_BAKERY && all_paths) {
            uint64_t w = tot->paths[NCL_PATH_BAKERY_WAIT];
            printf("bakery: waited=%.1f%%  pred_polls/wait=%.2f rescans/wait=%.2f "
                   "choosing_seen/wait=%.3f classic_drops/wait=%.3f\n",
                   100 * (double)w / (double)all_paths,
                   w ? (double)b->bak_pred_polls / (double)w : 0.0,
                   w ? (double)b->bak_rescans / (double)w : 0.0,
                   w ? (double)b->bak_choosing_seen / (double)w : 0.0,
                   w ? (double)b->bak_classic_drops / (double)w : 0.0);
        }
        const struct ncl_srv_stats *v = &tot->srv;
        if (cfg.alg == NCL_ALG_SERVER || ncl_alg_fastsrv_like(cfg.alg)) {
            /* each server round = one uncached load per source queue */
            printf("servers: msgs=%lu grants=%lu queued=%lu errors=%lu  poll_rounds/s=%.0f "
                   "(idle %.0f%%) queue_loads/s=%.0f",
                   (unsigned long)v->msgs, (unsigned long)v->grants, (unsigned long)v->queued,
                   (unsigned long)v->errors, (double)v->rounds / secs,
                   v->rounds ? 100.0 * (double)v->idle_rounds / (double)v->rounds : 0.0,
                   (double)v->rounds * nnodes / secs);
            if (cfg.mode == NCL_MODE_HANDOFF)
                printf("  succ_published=%lu rel_grants=%lu window_full=%lu",
                       (unsigned long)v->published, (unsigned long)v->rel_grants,
                       (unsigned long)v->window_full);
            if (ncl_alg_fastsrv_like(cfg.alg))
                printf("  top_slow_acquires=%lu top_polls=%lu top_yields=%lu",
                       (unsigned long)v->top_acquires, (unsigned long)v->top_wait_polls,
                       (unsigned long)v->top_yields);
            printf("\n");
        }
        if (cfg.mode == NCL_MODE_HANDOFF && cfg.alg == NCL_ALG_SERVER) {
            const struct ncl_handoff_stats *o = &tot->ho;
            printf("chain (%s): passes_sent=%lu rels=%lu succ_loads=%lu succ_misses=%lu "
                   "passes_received=%lu\n",
                   cfg.pick_policy == NCL_PICK_SCAN ? "always read" : "hint",
                   (unsigned long)o->handoffs_sent, (unsigned long)o->frees,
                   (unsigned long)o->announce_loads, (unsigned long)o->missed_announce,
                   (unsigned long)o->wait_handoffs);
        } else if (cfg.mode == NCL_MODE_HANDOFF) {
            const struct ncl_handoff_stats *o = &tot->ho;
            printf("handoff wait=%s opts=%s: grant_polls/acq=%.2f grant_waits=%lu "
                   "gate_skips/acq=%.3f\n",
                   WAIT_NAME(cfg.handoff_wait), ho_desc, PER(o->grant_polls),
                   (unsigned long)o->grant_waits, PER(o->gate_skips));
            printf("handoff (announce %s, pick %s): gate_entries=%lu takes=%lu gate_handoffs=%lu "
                   "wait_handoffs=%lu missed_announce=%lu owner_polls/acq=%.2f sent=%lu frees=%lu "
                   "announce_loads/release=%.2f\n",
                   ANN_NAME(cfg.announce_place), PICK_NAME(cfg.pick_policy),
                   (unsigned long)o->gate_entries,
                   (unsigned long)o->gate_takes, (unsigned long)o->gate_handoffs,
                   (unsigned long)o->wait_handoffs, (unsigned long)o->missed_announce,
                   PER(o->owner_polls), (unsigned long)o->handoffs_sent, (unsigned long)o->frees,
                   o->handoffs_sent + o->frees
                       ? (double)o->announce_loads / (double)(o->handoffs_sent + o->frees)
                       : 0.0);
        }
    }
    ncl_sys_destroy();
    free(tot);
    free(nr);
    return 0;
}
