/*
 * ncl_kv: a YCSB-style key-value store on the non-coherent shared data region
 * (design §6.1, §6.3; docs/kv.md).
 *
 * Table: K fixed-size records (key, version, value) striped over L locks;
 * stripe(k) = hash(k) mod L, and each stripe's records sit contiguously in
 * the data region, which is the lock's registered data range. YCSB A/B/C/F
 * never insert, so the key -> record index is static: the parent builds it
 * before the nodes fork and each node keeps its own copy (read-mostly
 * metadata, design §2).
 *
 * Operations: read (copy the value), update (write a new value), and
 * read-modify-write (F). Workloads: A 50/50 read/update, B 95/5, C read only,
 * F 50/50 read/RMW. Keys: uniform or YCSB's scrambled Zipfian (theta 0.99 by
 * default). Affinity -F f: with probability f a request picks a key from its
 * own node's partition (keys whose stripe s has s mod N == node), with the
 * same distribution inside the partition.
 *
 * Data maintenance: precise by default (cfg.precise_data: acquire names the
 * record's lines, release names them as dirty if written); --full uses the
 * conservative mode (the whole stripe invalidated on arrival, written back
 * on departure).
 *
 * Per-lock algorithms (--algmap top:H): the H stripes with the highest
 * expected load (from the key distribution) use the tournament, the rest
 * Lamport. H=0 is all Lamport, H=L all tournament; sweeping H bounds what a
 * per-lock choice (or a reactive lock) can gain.
 *
 * Verification: a record's value words are a function of (key, version), so
 * every read checks for stale or torn data (errors); at the end the parent
 * reads the device copy and checks every record and that the versions sum to
 * the number of updates. Any error fails the run (exit 1).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#include "hist.h"
#include "ncl/cpus.h"
#include "ncl/lock.h"

enum { OP_READ, OP_UPDATE, OP_RMW, OP__N };
enum { H_LOCK, H_OP, H__N };

static int nnodes = 2, nthreads = 1, nlocks = 64, pin = 1, csv = 0, full = 0;
static long nkeys = 100000;
static size_t vbytes = 100, rec_size;
static char workload = 'A';
static int zipf = 1;
static double theta = 0.99, affinity = 0.0;
static double secs = 2.0, warmup = 0.2, rate = 0;
static unsigned cs_ns = 0;
static uint64_t cs_cycles;
static double cyc_per_ns;
static int algmap_top = -1; /* --algmap top:H; -1 = every lock uses -a */

/* Static index, built by the parent and inherited by every node. */
static int32_t *key_stripe;   /* [K] */
static uint64_t *key_off;     /* [K] data-region offset of the record */
static uint64_t *stripe_off, *stripe_len; /* [L] */
static long *part_keys[NCL_MAX_NODES], part_n[NCL_MAX_NODES]; /* keys owned by node n */
static double *stripe_load;   /* [L] expected share of requests (for --algmap) */

struct record {
    uint64_t key;
    uint64_t version;
    uint64_t value[]; /* vwords words */
};
static size_t vwords;

struct result {
    uint64_t ops[OP__N], completed, offered, unserved, errors, updates;
    uint64_t local_handoffs, acquires;
    uint64_t global_acq, tour_acq, switches; /* REACTIVE: mode mix and switches */
    uint64_t slow_mode_reads, lamport_slow;
    struct hist h[H__N];
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
static uint64_t xorshift(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}
static double unif01(uint64_t *s) { return ((double)(xorshift(s) >> 11) + 1.0) / 9007199254740992.0; }

/* Value word i of key k at version v. */
static inline uint64_t vword(uint64_t k, uint64_t v, size_t i)
{
    return splitmix64(k * 0x9e3779b97f4a7c15ull ^ v * 0xc2b2ae3d27d4eb4full ^ (uint64_t)i);
}
static inline struct record *rec(long k) { return (struct record *)ncl_data_ptr(key_off[k]); }
static int check_record(const struct record *r, long k)
{
    if (r->key != (uint64_t)k)
        return 1;
    for (size_t i = 0; i < vwords; i++)
        if (r->value[i] != vword((uint64_t)k, r->version, i))
            return 1;
    return 0;
}

/* ---- key distributions (YCSB) --------------------------------------------- */

struct zipf {
    long n;
    double theta, alpha, zetan, eta, half_pow;
};
static double zeta(long n, double th)
{
    double s = 0;
    for (long i = 1; i <= n; i++)
        s += 1.0 / pow((double)i, th);
    return s;
}
static void zipf_init(struct zipf *z, long n, double th)
{
    z->n = n;
    z->theta = th;
    z->alpha = 1.0 / (1.0 - th);
    z->zetan = zeta(n, th);
    double zeta2 = 1.0 + pow(0.5, th);
    z->eta = (1.0 - pow(2.0 / (double)n, 1.0 - th)) / (1.0 - zeta2 / z->zetan);
    z->half_pow = 1.0 + pow(0.5, th);
}
static long zipf_next(const struct zipf *z, uint64_t *rng)
{
    double u = unif01(rng), uz = u * z->zetan;
    if (uz < 1.0)
        return 0;
    if (uz < z->half_pow)
        return 1;
    long r = (long)((double)z->n * pow(z->eta * u - z->eta + 1.0, z->alpha));
    return r >= z->n ? z->n - 1 : r;
}
/* YCSB's ScrambledZipfian: hash the rank, so popular keys spread out. */
static long scramble(long rank, long n) { return (long)(splitmix64((uint64_t)rank) % (uint64_t)n); }

/* ---- worker ---------------------------------------------------------------- */

struct worker {
    pthread_t tid;
    int node, idx;
    struct result r;
};
static struct zipf zall, zpart[NCL_MAX_NODES];

static long pick_key(int node, uint64_t *rng)
{
    if (affinity > 0 && unif01(rng) <= affinity && part_n[node] > 0) {
        long n = part_n[node];
        long r = zipf ? scramble(zipf_next(&zpart[node], rng), n) : (long)(xorshift(rng) % (uint64_t)n);
        return part_keys[node][r];
    }
    return zipf ? scramble(zipf_next(&zall, rng), nkeys) : (long)(xorshift(rng) % (uint64_t)nkeys);
}
static int pick_op(uint64_t *rng)
{
    double u = unif01(rng);
    switch (workload) {
    case 'A': return u <= 0.5 ? OP_READ : OP_UPDATE;
    case 'B': return u <= 0.95 ? OP_READ : OP_UPDATE;
    case 'C': return OP_READ;
    default: return u <= 0.5 ? OP_READ : OP_RMW; /* F */
    }
}

static void do_op(struct ncl_thread *t, struct result *r, uint64_t *rng, int op, long k,
                  uint64_t *tacq)
{
    int s = key_stripe[k];
    struct record *p = rec(k);
    struct ncl_range rg = {p, rec_size};
    if (full)
        ncl_acquire(t, s);
    else
        ncl_acquire_ranges(t, s, &rg, 1);
    *tacq = __rdtsc();
    uint64_t buf[vwords ? vwords : 1];
    int wrote = 0;
    if (op == OP_READ || op == OP_RMW) {
        memcpy(buf, p->value, vwords * sizeof(uint64_t));
        if (p->key != (uint64_t)k)
            r->errors++;
        else
            for (size_t i = 0; i < vwords; i++)
                if (buf[i] != vword((uint64_t)k, p->version, i)) {
                    r->errors++;
                    break;
                }
    }
    if (op == OP_UPDATE || op == OP_RMW) {
        uint64_t v = p->version + 1;
        for (size_t i = 0; i < vwords; i++)
            p->value[i] = vword((uint64_t)k, v, i);
        p->version = v;
        r->updates++;
        wrote = 1;
    }
    (void)rng;
    if (cs_cycles)
        ncl_lat_spin_cycles(cs_cycles);
    if (full)
        ncl_release(s, NULL, 0);
    else
        ncl_release(s, &rg, wrote);
}

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    struct result *r = &w->r;
    struct ncl_thread t;
    ncl_thread_init(&t, w->idx);
    uint64_t rng = splitmix64(((uint64_t)w->node << 32) | (uint64_t)w->idx) | 1;
    if (pin)
        ncl_pin_self(ncl_cpu_slot(w->node * nthreads + w->idx));
    uint64_t start_ns;
    while (!(start_ns = ncl_uc_load(ncl_user_line(0))))
        ncl_cpu_relax();
    while (now_ns() < start_ns)
        ncl_cpu_relax();
    const uint64_t t0 = __rdtsc();
    const uint64_t twarm = t0 + (uint64_t)(warmup * 1e9 * cyc_per_ns);
    const uint64_t tend = t0 + (uint64_t)(secs * 1e9 * cyc_per_ns);
    const double gap = rate > 0 ? 1e9 * cyc_per_ns / (rate / (double)(nnodes * nthreads)) : 0;
    double v = rate > 0 ? unif01(&rng) * gap : 0;
    for (;;) {
        uint64_t sched, start;
        if (rate > 0) {
            sched = t0 + (uint64_t)v;
            v += -log(unif01(&rng)) * gap;
            if (sched >= tend)
                break;
            while ((start = __rdtsc()) < sched)
                ncl_cpu_relax();
            if (start >= tend) {
                if (sched >= twarm) {
                    r->offered++;
                    r->unserved++;
                }
                continue;
            }
        } else {
            sched = start = __rdtsc();
            if (start >= tend)
                break;
        }
        int op = pick_op(&rng);
        long k = pick_key(w->node, &rng);
        uint64_t acq;
        do_op(&t, r, &rng, op, k, &acq);
        uint64_t done = __rdtsc();
        if (done >= twarm && done < tend)
            r->completed++;
        if (sched >= twarm) {
            r->offered++;
            r->ops[op]++;
            hist_add(&r->h[H_LOCK], (uint64_t)((double)(acq - sched) / cyc_per_ns));
            hist_add(&r->h[H_OP], (uint64_t)((double)(done - sched) / cyc_per_ns));
        }
    }
    return NULL;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t k = write(fd, p, len);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return -1;
        p += k;
        len -= (size_t)k;
    }
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
        struct result *r = &ws[i].r;
        for (int o = 0; o < OP__N; o++)
            tot->ops[o] += r->ops[o];
        tot->completed += r->completed;
        tot->offered += r->offered;
        tot->unserved += r->unserved;
        tot->errors += r->errors;
        tot->updates += r->updates;
        for (int h = 0; h < H__N; h++)
            hist_merge(&tot->h[h], &r->h[h]);
    }
    ncl_node_detach();
    struct ncl_node_stats st;
    ncl_node_stats(&st);
    tot->acquires = st.acquires;
    tot->local_handoffs = st.path[NCL_PATH_LOCAL_HANDOFF];
    tot->tour_acq = st.path[NCL_PATH_TOURNAMENT];
    tot->global_acq = st.path[NCL_PATH_TOURNAMENT] + st.path[NCL_PATH_LAMPORT_FAST] +
                      st.path[NCL_PATH_LAMPORT_SLOW];
    tot->switches = st.base.react_up + st.base.react_down;
    tot->slow_mode_reads = st.base.react_slow_mode_reads;
    tot->lamport_slow = st.path[NCL_PATH_LAMPORT_SLOW];
    int rc = write_full(wfd, tot, sizeof *tot) < 0 ? 3 : 0;
    close(wfd);
    return rc;
}

/* ---- setup ----------------------------------------------------------------- */

static int cmp_desc(const void *a, const void *b)
{
    double x = stripe_load[*(const int *)a], y = stripe_load[*(const int *)b];
    return x < y ? 1 : x > y ? -1 : (*(const int *)a - *(const int *)b);
}

/* Expected share of requests per stripe, from the key distribution. */
static void expected_load(void)
{
    stripe_load = calloc((size_t)nlocks, sizeof *stripe_load);
    double *pk = calloc((size_t)nkeys, sizeof *pk); /* global distribution */
    if (zipf) {
        for (long r = 0; r < nkeys; r++)
            pk[scramble(r, nkeys)] += 1.0 / pow((double)(r + 1), theta) / zall.zetan;
    } else {
        for (long k = 0; k < nkeys; k++)
            pk[k] = 1.0 / (double)nkeys;
    }
    for (long k = 0; k < nkeys; k++)
        stripe_load[key_stripe[k]] += (1.0 - affinity) * pk[k];
    if (affinity > 0) {
        for (int n = 0; n < nnodes; n++) {
            long m = part_n[n];
            for (long r = 0; r < m; r++) {
                double p = zipf ? 1.0 / pow((double)(r + 1), theta) / zpart[n].zetan : 1.0 / (double)m;
                long k = part_keys[n][zipf ? scramble(r, m) : r];
                stripe_load[key_stripe[k]] += affinity * p / (double)nnodes;
            }
        }
    }
    free(pk);
}

static void usage(const char *a0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -n N -t T     nodes, threads per node (2, 1)\n"
            "  -l L          locks = stripes (64)       -k K   records (100000)\n"
            "  -V B          value bytes (100; records are 64-byte aligned)\n"
            "  -w A|B|C|F    YCSB workload (A)          -d uniform|zipf (zipf)\n"
            "  -z THETA      Zipfian skew (0.99)        -F FRAC key-to-node affinity (0)\n"
            "  -a ALG        lock algorithm (tournament); --algmap top:H: the H stripes with\n"
            "                the highest expected load use the tournament, the rest Lamport\n"
            "                (-a reactive: they start in tournament mode; --react off pins them)\n"
            "  -M MODE -K CAP -b BACKEND -U LAYOUT --wb clwb --mem-node N --cpus LIST: as ncl_bench\n"
            "  --react U:D[:W] -a reactive: to the tournament when >= U of the last 16 global\n"
            "                acquisitions met another node, back when <= D of the last 64, after >= W\n"
            "                acquisitions on the tournament (8:4:64); off: never switch\n"
            "  --full        conservative data maintenance (whole stripe) instead of precise\n"
            "  -c NS         extra critical-section time    -s SECS -W SECS (2, 0.2)\n"
            "  -r RATE       open loop, Poisson, aggregate req/s (k/M ok); default closed loop\n"
            "  --react-layout mode-first|xy-first  reactive lock block layout (xy-first)\n"
            "  --pad-lines N extra metadata lines per lock (layout experiments)\n"
            "  --raw-layout  no stride rule (per-lock line counts that are multiples of 8 are\n"
            "                otherwise padded by 2)\n"
            "  -m            no node-local submutex: every thread is a global participant\n"
            "                (Phase 1 only; with -b coherent -a pthread: one plain mutex per stripe)\n"
            "  -P            don't pin    -C one CSV row    -H CSV header\n"
            "  --debug skip-inv|skip-wb|no-local-refresh|no-dirty-wb: DEBUG, must FAIL on emu\n",
            a0);
}

#define CSV_HEADER                                                                             \
    "workload,dist,theta,affinity,keys,value_bytes,locks,alg,algmap_top,tournament_locks,"     \
    "backend,mode,local_cap,meta_layout,data,nodes,threads_per_node,target_rps,achieved_rps,"  \
    "unserved_frac,lock_p50,lock_p99,lock_p999,op_p50,op_p99,op_p999,node_min_max_ratio,"      \
    "local_handoff_frac,hot_stripe_share,errors,tournament_acq_frac,mode_switches,react,"   \
    "no_submutex,react_layout,pad_lines,lamport_slow_frac,slow_mode_reads_per_acq,"          \
    "lines_per_lock,raw_layout"

int main(int argc, char **argv)
{
    struct ncl_sys_cfg cfg = {
        .backend = NCL_BACKEND_CACHED_FLUSH,
        .alg = NCL_ALG_TOURNAMENT,
        .mode = NCL_MODE_PHASE1,
        .handoff_opts = NCL_HO_DEFAULT,
    };
    static struct option lo[] = {{"algmap", required_argument, 0, 1001},
                                 {"full", no_argument, 0, 1002},
                                 {"mem-node", required_argument, 0, 1003},
                                 {"cpus", required_argument, 0, 1004},
                                 {"wb", required_argument, 0, 1005},
                                 {"debug", required_argument, 0, 1006},
                                 {"react", required_argument, 0, 1007},
                                 {"react-layout", required_argument, 0, 1008},
                                 {"pad-lines", required_argument, 0, 1009},
                                 {"raw-layout", no_argument, 0, 1010},
                                 {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "n:t:l:k:V:w:d:z:F:a:M:K:b:U:c:s:W:r:mPCHh", lo, NULL)) != -1) {
        switch (c) {
        case 'n': nnodes = atoi(optarg); break;
        case 't': nthreads = atoi(optarg); break;
        case 'l': nlocks = atoi(optarg); break;
        case 'k': nkeys = atol(optarg); break;
        case 'V': vbytes = (size_t)atol(optarg); break;
        case 'w': workload = optarg[0]; break;
        case 'd': zipf = strcmp(optarg, "uniform") != 0; break;
        case 'z': theta = atof(optarg); break;
        case 'F': affinity = atof(optarg); break;
        case 'a':
            if (ncl_alg_parse(optarg, &cfg.alg) < 0) { usage(argv[0]); return 2; }
            break;
        case 'M':
            if (ncl_mode_parse(optarg, &cfg.mode) < 0) { usage(argv[0]); return 2; }
            break;
        case 'K': cfg.local_cap = atoi(optarg); break;
        case 'b':
            if (ncl_backend_parse(optarg, &cfg.backend) < 0) { usage(argv[0]); return 2; }
            break;
        case 'U': cfg.meta_split = !strcmp(optarg, "split"); break;
        case 'c': cs_ns = (unsigned)atoi(optarg); break;
        case 's': secs = atof(optarg); break;
        case 'W': warmup = atof(optarg); break;
        case 'r': {
            char *e;
            rate = strtod(optarg, &e);
            if (*e == 'k' || *e == 'K') rate *= 1e3;
            if (*e == 'm' || *e == 'M') rate *= 1e6;
            break;
        }
        case 'm': cfg.no_submutex = 1; break;
        case 'P': pin = 0; break;
        case 'C': csv = 1; break;
        case 'H': puts(CSV_HEADER); return 0;
        case 1001:
            if (strncmp(optarg, "top:", 4)) { usage(argv[0]); return 2; }
            algmap_top = atoi(optarg + 4);
            break;
        case 1002: full = 1; break;
        case 1008:
            if (!strcmp(optarg, "xy-first")) ncl_react_layout = 1;
            else if (!strcmp(optarg, "mode-first")) ncl_react_layout = 0;
            else { usage(argv[0]); return 2; }
            break;
        case 1009: cfg.pad_lines = atoi(optarg); break;
        case 1010: cfg.raw_layout = 1; break;
        case 1007:
            if (ncl_react_parse(optarg) < 0) { usage(argv[0]); return 2; }
            break;
        case 1006: /* emulator self-checks: each must FAIL on -b emu */
            if (!strcmp(optarg, "skip-inv")) cfg.debug_skip_data_inv = 1;
            else if (!strcmp(optarg, "skip-wb")) cfg.debug_skip_data_wb = 1;
            else if (!strcmp(optarg, "no-local-refresh")) cfg.debug_precise = 1;
            else if (!strcmp(optarg, "no-dirty-wb")) cfg.debug_precise = 2;
            else { usage(argv[0]); return 2; }
            break;
        case 1003: cfg.mem_node = atoi(optarg) + 1; break;
        case 1004: if (ncl_cpus_set(optarg) < 0) return 2; break;
        case 1005:
            if (!strcmp(optarg, "clwb")) { if (ncl_wb_mode_set(NCL_WB_CLWB) < 0) return 2; }
            else if (strcmp(optarg, "clflushopt")) { usage(argv[0]); return 2; }
            break;
        default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (nnodes < 1 || nnodes > NCL_MAX_NODES || nthreads < 1 || nlocks < 1 || nkeys < 1 ||
        !strchr("ABCF", workload) || theta <= 0 || theta >= 1 || affinity < 0 || affinity > 1 ||
        warmup >= secs || algmap_top > nlocks) {
        usage(argv[0]);
        return 2;
    }
    /* --algmap with -a reactive: the H hottest stripes start in tournament mode
     * (the rest Lamport); with --react off they stay there (isolation runs). */
    int react_map = algmap_top >= 0 && cfg.alg == NCL_ALG_REACTIVE;
    if (algmap_top >= 0 && !react_map) {
        cfg.alg_per_lock = 1;
        cfg.alg = NCL_ALG_LAMPORT;
    }
    vwords = (vbytes + 7) / 8;
    rec_size = (sizeof(struct record) + vwords * 8 + NCL_CACHELINE - 1) & ~(size_t)(NCL_CACHELINE - 1);

    /* Index: stripe of every key, then each stripe's records contiguously. */
    key_stripe = malloc((size_t)nkeys * sizeof *key_stripe);
    key_off = malloc((size_t)nkeys * sizeof *key_off);
    stripe_off = calloc((size_t)nlocks, sizeof *stripe_off);
    stripe_len = calloc((size_t)nlocks, sizeof *stripe_len);
    for (long k = 0; k < nkeys; k++) {
        key_stripe[k] = (int32_t)(splitmix64((uint64_t)k ^ 0xabcdef12345ull) % (uint64_t)nlocks);
        stripe_len[key_stripe[k]] += rec_size;
    }
    uint64_t off = 0;
    for (int s = 0; s < nlocks; s++) {
        stripe_off[s] = off;
        off += stripe_len[s] ? stripe_len[s] : NCL_CACHELINE;
    }
    uint64_t *fill = calloc((size_t)nlocks, sizeof *fill);
    for (long k = 0; k < nkeys; k++) {
        int s = key_stripe[k];
        key_off[k] = stripe_off[s] + fill[s];
        fill[s] += rec_size;
    }
    free(fill);
    for (int n = 0; n < nnodes; n++)
        part_keys[n] = malloc((size_t)nkeys * sizeof(long));
    for (long k = 0; k < nkeys; k++) {
        int n = key_stripe[k] % nnodes;
        part_keys[n][part_n[n]++] = k;
    }
    if (zipf) {
        zipf_init(&zall, nkeys, theta);
        if (affinity > 0)
            for (int n = 0; n < nnodes; n++)
                if (part_n[n] > 1)
                    zipf_init(&zpart[n], part_n[n], theta);
    }
    expected_load();
    int *order = malloc((size_t)nlocks * sizeof *order);
    for (int s = 0; s < nlocks; s++)
        order[s] = s;
    qsort(order, (size_t)nlocks, sizeof *order, cmp_desc);
    double hot_share = stripe_load[order[0]];

    cfg.nnodes = nnodes;
    cfg.threads_per_node = nthreads;
    cfg.nlocks = nlocks;
    cfg.data_size = off;
    cfg.user_meta_lines = 1;
    cfg.precise_data = !full;
    cfg.server_pin = pin ? NCL_SRV_PIN_NEXT : NCL_SRV_PIN_NONE;
    if (cfg.backend == NCL_BACKEND_UC && access(getenv("NCL_UC_DEV") ? getenv("NCL_UC_DEV")
                                                                      : "/dev/ncl_uc", F_OK)) {
        fprintf(stderr, "kv: uc backend requested but driver not loaded; skipping\n");
        return 77; /* ctest SKIP_RETURN_CODE */
    }
    if (ncl_sys_create(&cfg) < 0)
        return 2;
    const size_t lines_per_lock = ncl_lines_per_lock();
    if (pin) {
        int srv = cfg.alg == NCL_ALG_SERVER || ncl_alg_fastsrv_like(cfg.alg);
        ncl_cpus_check(nnodes * nthreads + (srv ? nnodes : 0), nnodes, nthreads, 0);
    }
    int ntour = cfg.alg == NCL_ALG_TOURNAMENT ? nlocks : 0;
    for (int s = 0; s < nlocks; s++) {
        if (ncl_lock_set_data(s, (ncl_off_t)stripe_off[s], stripe_len[s]) < 0)
            return 2;
    }
    if (algmap_top >= 0) {
        ntour = algmap_top;
        for (int i = 0; i < algmap_top; i++)
            if ((react_map ? ncl_lock_set_reactive_mode(order[i], 1)
                           : ncl_lock_set_alg(order[i], NCL_ALG_TOURNAMENT)) < 0)
                return 2;
    }
    /* Load phase: version 0, written back to the device. */
    for (long k = 0; k < nkeys; k++) {
        struct record *p = rec(k);
        p->key = (uint64_t)k;
        p->version = 0;
        for (size_t i = 0; i < vwords; i++)
            p->value[i] = vword((uint64_t)k, 0, i);
    }
    ncl_data_writeback(ncl_mem.data, off);
    cs_cycles = ncl_ns_to_cycles(cs_ns);
    cyc_per_ns = (double)ncl_ns_to_cycles(1000000) / 1e6;

    pid_t *pids = calloc((size_t)nnodes, sizeof *pids);
    int *rfd = calloc((size_t)nnodes, sizeof *rfd);
    for (int n = 0; n < nnodes; n++) {
        int pfd[2];
        if (pipe(pfd) < 0) { perror("pipe"); return 2; }
        pid_t p = fork();
        if (p < 0) { perror("fork"); return 2; }
        if (p == 0) {
            close(pfd[0]);
            _exit(node_main(n, pfd[1]));
        }
        close(pfd[1]);
        rfd[n] = pfd[0];
        pids[n] = p;
    }
    ncl_uc_store(ncl_user_line(0), now_ns() + 50000000ull);
    ncl_fence();

    struct result *tot = calloc(1, sizeof *tot), *nr = malloc(sizeof *nr);
    uint64_t node_done[NCL_MAX_NODES] = {0};
    int bad = 0;
    for (int n = 0; n < nnodes; n++) {
        if (read_full(rfd[n], nr, sizeof *nr) < 0) {
            fprintf(stderr, "node %d: no result\n", n);
            bad = 1;
            continue;
        }
        for (int o = 0; o < OP__N; o++)
            tot->ops[o] += nr->ops[o];
        tot->completed += nr->completed;
        node_done[n] = nr->completed;
        tot->offered += nr->offered;
        tot->unserved += nr->unserved;
        tot->errors += nr->errors;
        tot->updates += nr->updates;
        tot->acquires += nr->acquires;
        tot->local_handoffs += nr->local_handoffs;
        tot->global_acq += nr->global_acq;
        tot->tour_acq += nr->tour_acq;
        tot->switches += nr->switches;
        tot->slow_mode_reads += nr->slow_mode_reads;
        tot->lamport_slow += nr->lamport_slow;
        for (int h = 0; h < H__N; h++)
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

    /* Final check against the device copy. */
    ncl_data_invalidate(ncl_mem.data, off);
    uint64_t vsum = 0, final_bad = 0;
    for (long k = 0; k < nkeys; k++) {
        struct record *p = rec(k);
        final_bad += check_record(p, k);
        vsum += p->version;
    }
    uint64_t errors = tot->errors + final_bad + (vsum != tot->updates);

    double window = secs - warmup;
    double achieved = (double)tot->completed / window;
    double unserved = tot->offered ? (double)tot->unserved / (double)tot->offered : 0;
    uint64_t nmin = UINT64_MAX, nmax = 0;
    for (int n = 0; n < nnodes; n++) {
        if (node_done[n] < nmin) nmin = node_done[n];
        if (node_done[n] > nmax) nmax = node_done[n];
    }
    double fair = nmax ? (double)nmin / (double)nmax : 1.0;
    double lh = tot->acquires ? (double)tot->local_handoffs / (double)tot->acquires : 0;
    double tf = tot->global_acq ? (double)tot->tour_acq / (double)tot->global_acq : 0;
    char react[24] = "-";
    if (cfg.alg == NCL_ALG_REACTIVE && ncl_react_off)
        snprintf(react, sizeof react, "off");
    else if (cfg.alg == NCL_ALG_REACTIVE)
        snprintf(react, sizeof react, "%u:%u:%u", ncl_react_up, ncl_react_down, ncl_react_dwell);
    const struct hist *hl = &tot->h[H_LOCK], *ho = &tot->h[H_OP];
    char modebuf[32];
    snprintf(modebuf, sizeof modebuf, "%s", ncl_mode_name(cfg.mode));
    if (csv) {
        printf("%c,%s,%.2f,%.2f,%ld,%zu,%d,%s,%d,%d,%s,%s,%d,%s,%s,%d,%d,%.0f,%.0f,%.5f,%lu,%lu,"
               "%lu,%lu,%lu,%lu,%.3f,%.4f,%.4f,%lu,%.4f,%lu,%s,%d,%s,%d,%.4f,%.4f,%zu,%d\n",
               workload, zipf ? "zipf" : "uniform", zipf ? theta : 0.0, affinity, nkeys, vbytes,
               nlocks, algmap_top >= 0 && !react_map ? "map" : ncl_alg_name(cfg.alg), algmap_top, ntour,
               ncl_backend_name(cfg.backend), modebuf,
               cfg.mode == NCL_MODE_PHASE1 ? 0 : (cfg.local_cap ? cfg.local_cap : 64),
               cfg.meta_split ? "split" : "uniform", full ? "full" : "precise", nnodes, nthreads,
               rate, achieved, unserved, (unsigned long)hist_pct(hl, 0.5),
               (unsigned long)hist_pct(hl, 0.99), (unsigned long)hist_pct(hl, 0.999),
               (unsigned long)hist_pct(ho, 0.5), (unsigned long)hist_pct(ho, 0.99),
               (unsigned long)hist_pct(ho, 0.999), fair, lh, hot_share, (unsigned long)errors, tf,
               (unsigned long)tot->switches, react, cfg.no_submutex,
               ncl_react_layout ? "xy-first" : "mode-first", cfg.pad_lines,
               tot->global_acq ? (double)tot->lamport_slow / (double)tot->global_acq : 0.0,
               tot->global_acq ? (double)tot->slow_mode_reads / (double)tot->global_acq : 0.0,
               lines_per_lock, cfg.raw_layout);
    } else {
        printf("kv: workload %c, %s, affinity %.2f, %ld keys x %zu B, %d locks, alg %s "
               "(%d tournament), %s, mode %s, data %s, %dx%d\n",
               workload, zipf ? "zipf" : "uniform", affinity, nkeys, vbytes,
               nlocks, algmap_top >= 0 && !react_map ? "map" : ncl_alg_name(cfg.alg), ntour,
               ncl_backend_name(cfg.backend), modebuf, full ? "full" : "precise", nnodes, nthreads);
        printf("mem: %s\n", ncl_mem_describe());
        printf("throughput %.0f ops/s (read %lu, update %lu, rmw %lu), unserved %.4f, fairness "
               "%.3f, local handoffs %.3f, hottest stripe %.4f of requests\n",
               achieved, (unsigned long)tot->ops[OP_READ], (unsigned long)tot->ops[OP_UPDATE],
               (unsigned long)tot->ops[OP_RMW], unserved, fair, lh, hot_share);
        if (cfg.alg == NCL_ALG_REACTIVE)
            printf("reactive %s: %.3f of global acquisitions on the tournament, %lu mode switches\n",
                   react, tf, (unsigned long)tot->switches);
        printf("latency ns   p50 / p99 / p99.9: lock %lu / %lu / %lu, op %lu / %lu / %lu\n",
               (unsigned long)hist_pct(hl, 0.5), (unsigned long)hist_pct(hl, 0.99),
               (unsigned long)hist_pct(hl, 0.999), (unsigned long)hist_pct(ho, 0.5),
               (unsigned long)hist_pct(ho, 0.99), (unsigned long)hist_pct(ho, 0.999));
        printf("check: read errors %lu, bad final records %lu, versions %lu vs updates %lu -> %s\n",
               (unsigned long)tot->errors, (unsigned long)final_bad, (unsigned long)vsum,
               (unsigned long)tot->updates, errors ? "FAIL" : "PASS");
    }
    ncl_sys_destroy();
    return errors ? 1 : 0;
}
