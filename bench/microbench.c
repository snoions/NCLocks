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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#include "hist.h"
#include "ncl/lock.h"

enum { ARR_CLOSED, ARR_POISSON, ARR_UNIFORM, ARR_ONOFF };
static const char *const arr_names[] = {"closed", "poisson", "uniform", "onoff"};
enum { H_LOCK, H_SERVICE, H_SOJOURN, H__COUNT };
static const char *const h_names[] = {"lock", "service", "sojourn"};

static int nnodes = 2, nthreads = 1, nlocks = 1, pin = 1, csv = 0;
static int srv_place = NCL_SRV_PIN_NEXT;
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
    double per_thread = rate / (double)(nnodes * nthreads); /* req/s */
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
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET((int)((w->node * nthreads + w->idx) % ncpu), &cs);
        pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    }

    uint64_t start_ns;
    while (!(start_ns = ncl_uc_load(ncl_user_line(0))))
        ncl_cpu_relax();
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

static int node_main(int node, int wfd)
{
    if (ncl_node_attach(node) < 0)
        return 2;
    struct worker *ws = calloc((size_t)nthreads, sizeof *ws);
    for (int i = 0; i < nthreads; i++) {
        ws[i].node = node;
        ws[i].idx = i;
        pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]);
    }
    struct result *tot = calloc(1, sizeof *tot);
    for (int i = 0; i < nthreads; i++) {
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

    const char *p = (const char *)tot;
    size_t left = sizeof *tot;
    while (left) {
        ssize_t k = write(wfd, p, left);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return 3;
        p += k;
        left -= (size_t)k;
    }
    close(wfd);
    free(tot);
    free(ws);
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t k = read(fd, p, len);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        p += k;
        len -= (size_t)k;
    }
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

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -n N        nodes (default 2)            -t T   threads per node (default 1)\n"
            "  -l L        locks (default 1)            -a ALG lamport|tournament|hybrid|server|fastsrv\n"
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
            "              gate/owner cross-node handoff; lamport, tournament, hybrid)\n"
            "  -K CAP      local/handoff: max consecutive local handoffs (default 64)\n"
            "  -G PLACE    handoff: announce inside the gate (default) | before it\n"
            "  -B N        spin-wait backoff cap for base algorithms, pause iterations (1024)\n"
            "  -Y PLACE    server thread placement: next (CPU N*T+n, default) | sibling\n"
            "              (SMT sibling of node n's first worker)\n"
            "  -F MODE     (d)/(e) fast-release reopen check: hint (only the slot last seen\n"
            "              set, default) | scan (all present[])\n",
            argv0);
}

#define CSV_HEADER                                                                         \
    "alg,backend,nodes,threads_per_node,locks,cs_ns,think_ns,footprint,dev_lat_ns,"        \
    "no_submutex,mode,local_cap,spin_backoff,server_backoff,arrival,target_rps,offered_rps,achieved_rps,unserved_frac,"                          \
    "lock_mean,lock_p50,lock_p90,lock_p99,lock_p999,lock_max,"                             \
    "svc_mean,svc_p50,svc_p99,svc_p999,soj_p50,soj_p99,soj_p999,"                          \
    "fast_frac,meta_memtype,meta_load_ns,node_min_max_ratio,server_place,scan_loads_per_acq,lamport_retries_per_acq,fast_release,announce,global_handoff_frac,local_handoff_frac,missed_announce_per_acq,owner_polls_per_acq,gate_entries_per_acq,announce_loads_per_acq"

int main(int argc, char **argv)
{
    struct ncl_sys_cfg cfg = {
        .backend = NCL_BACKEND_CACHED_FLUSH,
        .alg = NCL_ALG_LAMPORT,
        .mode = NCL_MODE_PHASE1,
    };
    int c, arrival_given = 0;
    while ((c = getopt(argc, argv, "n:t:l:a:b:L:s:W:c:T:f:r:A:S:M:K:B:Y:F:G:mCHPh")) != -1) {
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
        case 'B': ncl_backoff_cap = (unsigned)atoi(optarg); break;
        case 'G':
            if (!strcmp(optarg, "inside"))
                cfg.announce_before = 0;
            else if (!strcmp(optarg, "before"))
                cfg.announce_before = 1;
            else {
                fprintf(stderr, "-G: inside|before\n");
                return 2;
            }
            break;
        case 'F':
            if (!strcmp(optarg, "scan"))
                ncl_fast_release = NCL_FASTREL_SCAN;
            else if (!strcmp(optarg, "hint"))
                ncl_fast_release = NCL_FASTREL_HINT;
            else {
                fprintf(stderr, "-F: scan|hint\n");
                return 2;
            }
            break;
        case 'C': csv = 1; break;
        case 'H': printf("%s\n", CSV_HEADER); return 0;
        case 'P': pin = 0; break;
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
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
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

    stride = (footprint + NCL_CACHELINE - 1) & ~(size_t)(NCL_CACHELINE - 1);
    if (!stride)
        stride = NCL_CACHELINE;
    cfg.nnodes = nnodes;
    cfg.threads_per_node = nthreads;
    cfg.nlocks = nlocks;
    cfg.data_size = stride * (size_t)nlocks;
    cfg.user_meta_lines = 1;
    cfg.server_pin = pin ? srv_place : NCL_SRV_PIN_NONE;
    if (ncl_sys_create(&cfg) < 0)
        return 2;
    for (int l = 0; l < nlocks; l++)
        if (footprint && ncl_lock_set_data(l, (ncl_off_t)l * stride, footprint) < 0)
            return 2;
    cs_cycles = ncl_ns_to_cycles(cs_ns);
    think_cycles = ncl_ns_to_cycles(think_ns);
    cyc_per_ns = (double)ncl_ns_to_cycles(1000000) / 1e6;

    pid_t *pids = calloc((size_t)nnodes, sizeof *pids);
    int *rfd = calloc((size_t)nnodes, sizeof *rfd);
    for (int n = 0; n < nnodes; n++) {
        int pfd[2];
        if (pipe(pfd) < 0) {
            perror("pipe");
            return 2;
        }
        pid_t p = fork();
        if (p < 0) {
            perror("fork");
            return 2;
        }
        if (p == 0) {
            close(pfd[0]);
            _exit(node_main(n, pfd[1]));
        }
        close(pfd[1]);
        rfd[n] = pfd[0];
        pids[n] = p;
    }
    ncl_uc_store(ncl_user_line(0), now_ns() + 50000000ull); /* start in 50 ms */
    ncl_fence();

    struct result *tot = calloc(1, sizeof *tot), *nr = malloc(sizeof *nr);
    uint64_t node_done[NCL_MAX_NODES] = {0}; /* per-node completions: fairness */
    int bad = 0;
    for (int n = 0; n < nnodes; n++) {
        if (read_full(rfd[n], nr, sizeof *nr) < 0) {
            fprintf(stderr, "node %d: no result\n", n);
            bad = 1;
            continue;
        }
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
        close(rfd[n]);
    }
    for (int n = 0; n < nnodes; n++) {
        int st;
        waitpid(pids[n], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st))
            bad = 1;
    }
    if (bad) {
        fprintf(stderr, "a node failed\n");
        return 1;
    }

    double window = secs - warmup;
    /* Completions inside the window. Counting only requests *scheduled* in it
     * would undercount when saturated, since warmup backlog drains first. */
    double achieved = (double)tot->completed / window;
    double offered = arrival == ARR_CLOSED ? achieved : (double)tot->offered / window;
    double unserved = tot->offered ? (double)tot->unserved / (double)tot->offered : 0;
    uint64_t nmin = UINT64_MAX, nmax = 0;
    for (int n = 0; n < nnodes; n++) {
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
#define PER(x) (all_paths ? (double)(x) / (double)all_paths : 0.0)
    char arrdesc[64];
    if (arrival == ARR_ONOFF)
        snprintf(arrdesc, sizeof arrdesc, "onoff:%g:%g", onoff_period_us, onoff_frac);
    else
        snprintf(arrdesc, sizeof arrdesc, "%s", arr_names[arrival]);

    if (csv) {
        printf("%s,%s,%d,%d,%d,%u,%u,%zu,%u,%d,%s,%d,%u,%u,%s,%.0f,%.0f,%.0f,%.5f,"
               "%.0f,%lu,%lu,%lu,%lu,%lu,%.0f,%lu,%lu,%lu,%lu,%lu,%lu,%.4f,%s,%.1f,%.3f,%s,%.3f,%.3f,%s,%s,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f\n",
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
               ncl_fast_release == NCL_FASTREL_HINT ? "hint" : "scan",
               cfg.mode != NCL_MODE_HANDOFF ? "-" : cfg.announce_before ? "before" : "inside",
               PER(tot->paths[NCL_PATH_GLOBAL_HANDOFF]), PER(tot->paths[NCL_PATH_LOCAL_HANDOFF]),
               PER(tot->ho.missed_announce), PER(tot->ho.owner_polls), PER(tot->ho.gate_entries),
               PER(tot->ho.announce_loads));
    } else {
        printf("alg=%s backend=%s mode=%s%s nodes=%d threads/node=%d locks=%d cs=%uns "
               "footprint=%zuB lat=%uns secs=%.2f warmup=%.2f\n",
               ncl_alg_name(cfg.alg), ncl_backend_name(cfg.backend), ncl_mode_name(cfg.mode),
               cfg.no_submutex ? "+no_submutex" : "", nnodes, nthreads, nlocks, cs_ns, footprint,
               cfg.dev_lat_ns, secs, warmup);
        printf("mem: %s\n", ncl_mem_describe());
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
        printf("per-node completions (min/max = %.3f):", fair);
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
        const struct ncl_srv_stats *v = &tot->srv;
        if (cfg.alg == NCL_ALG_SERVER || cfg.alg == NCL_ALG_FASTSRV) {
            /* each server round = one uncached load per source queue */
            printf("servers: msgs=%lu grants=%lu queued=%lu errors=%lu  poll_rounds/s=%.0f "
                   "(idle %.0f%%) queue_loads/s=%.0f",
                   (unsigned long)v->msgs, (unsigned long)v->grants, (unsigned long)v->queued,
                   (unsigned long)v->errors, (double)v->rounds / secs,
                   v->rounds ? 100.0 * (double)v->idle_rounds / (double)v->rounds : 0.0,
                   (double)v->rounds * nnodes / secs);
            if (cfg.alg == NCL_ALG_FASTSRV)
                printf("  top_slow_acquires=%lu top_polls=%lu top_yields=%lu",
                       (unsigned long)v->top_acquires, (unsigned long)v->top_wait_polls,
                       (unsigned long)v->top_yields);
            printf("\n");
        }
        if (cfg.mode == NCL_MODE_HANDOFF) {
            const struct ncl_handoff_stats *o = &tot->ho;
            printf("handoff (announce %s): gate_entries=%lu takes=%lu gate_handoffs=%lu "
                   "wait_handoffs=%lu missed_announce=%lu owner_polls/acq=%.2f sent=%lu frees=%lu "
                   "announce_loads/release=%.2f\n",
                   cfg.announce_before ? "before" : "inside", (unsigned long)o->gate_entries,
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
    free(pids);
    free(rfd);
    return 0;
}
