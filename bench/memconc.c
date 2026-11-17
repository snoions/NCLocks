/*
 * ncl_memconc: aggregate throughput of one memory-access pattern issued by T
 * threads at once, each on its own lines (docs/cxl_results.md, "UC ceiling").
 * Question: does the device serve uncached 8-byte accesses from many cores
 * concurrently, or does the aggregate flatten? And is it the uncached memory
 * type or the partial (8-byte) write that limits it?
 *
 * Usage: ncl_memconc -b uc|cached-flush|coherent [-T 1,2,4,...] [-o op,op,...]
 *                    [-s SECS] [-W WARMUP] [--mem-node N] [--wb clwb] [-C] [-H]
 * One row per (op, T). Threads are pinned to the process's CPUs in order
 * (numactl --cpunodebind works; more threads than CPUs is refused).
 *
 * Ops (metadata ops go through the backend: UC on uc, flush around the
 * access on cached-flush; data ops always use the cached data region):
 *   load     one metadata load of the thread's own line
 *   store    one metadata store (8 bytes) to the own line, no fence
 *   storef   store + full fence
 *   dekker   store own line, full fence, load the next thread's line (the
 *            Peterson/Lamport step)
 *   shload   every thread loads the same metadata line (a polled hot spot)
 *   wb64     cached data: write the own line, writeback (clflushopt or
 *            --wb clwb), sfence: a full-line write to the device
 *   nt8      cached data: one 8-byte non-temporal store (movnti), sfence: a
 *            partial write from write-back memory, no UC involved
 *   nt64     cached data: 8 movnti filling the own line, sfence: a full line
 *            through write combining
 * nt8 vs nt64 vs store separates "partial write" from "uncached".
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>
#include "ncl/cpus.h"
#include "ncl/mem.h"

enum { OP_LOAD, OP_STORE, OP_STOREF, OP_DEKKER, OP_SHLOAD, OP_WB64, OP_NT8, OP_NT64, OP_N };
static const char *op_names[OP_N] = {"load", "store", "storef", "dekker",
                                     "shload", "wb64", "nt8", "nt64"};

#define MAXT 256
#define META_STRIDE 2 /* lines between threads' metadata lines (no adjacent-line pairs) */
#define DATA_STRIDE 4 /* lines between threads' data lines */

static int op, nthreads;
static ncl_line_t *mlines; /* thread t: mlines[t * META_STRIDE] */
static ncl_line_t *shared_line;
static char *dlines;       /* thread t: dlines + t * DATA_STRIDE * 64 */
static _Atomic int phase;  /* 0 warmup, 1 measuring, 2 stop */
static pthread_barrier_t bar;
static uint64_t counted[MAXT];
static volatile uint64_t sink_all;

static void *worker(void *arg)
{
    int t = (int)(intptr_t)arg;
    ncl_pin_self(ncl_cpu_slot(t));
    ncl_line_t *my = &mlines[t * META_STRIDE];
    ncl_line_t *peer = &mlines[((t + 1) % (nthreads > 1 ? nthreads : 2)) * META_STRIDE];
    char *d = dlines + (size_t)t * DATA_STRIDE * 64;
    uint64_t n = 0, start = 0, sink = 0;
    int started = 0;
    pthread_barrier_wait(&bar);
    for (;;) {
        for (int k = 0; k < 64; k++) {
            uint64_t v = n + (uint64_t)k + 1;
            switch (op) {
            case OP_LOAD: sink += ncl_uc_load(my); break;
            case OP_STORE: ncl_uc_store(my, v); break;
            case OP_STOREF: ncl_uc_store(my, v); ncl_fence(); break;
            case OP_DEKKER: ncl_uc_store(my, v); ncl_fence(); sink += ncl_uc_load(peer); break;
            case OP_SHLOAD: sink += ncl_uc_load(shared_line); break;
            case OP_WB64:
                *(volatile uint64_t *)d = v;
                ncl_data_writeback(d, 64); /* clflushopt|clwb + sfence */
                break;
            case OP_NT8:
                _mm_stream_si64((long long *)d, (long long)v);
                _mm_sfence();
                break;
            case OP_NT64:
                for (int w = 0; w < 8; w++)
                    _mm_stream_si64((long long *)d + w, (long long)v);
                _mm_sfence();
                break;
            }
        }
        n += 64;
        int p = atomic_load_explicit(&phase, memory_order_relaxed);
        if (p == 1 && !started) {
            start = n;
            started = 1;
        } else if (p == 2) {
            counted[t] = started ? n - start : 0;
            break;
        }
    }
    sink_all += sink;
    return NULL;
}

static void sleep_s(double s)
{
    struct timespec ts = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
    while (nanosleep(&ts, &ts))
        ;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static const char *HEADER =
    "backend,meta_memtype,mem_node,uc_pool_node,wb,op,threads,secs,mops,"
    "ns_per_op_per_thread,thread_min_max_ratio,cpus";

int main(int argc, char **argv)
{
    struct ncl_mem_cfg mc = {.backend = NCL_BACKEND_UC, .meta_size = 1 << 20,
                             .scan_size = 1 << 16, .data_size = 1 << 20};
    const char *tlist = "1,2,4,8,16", *olist = "load,store,storef,dekker,shload,wb64,nt8,nt64";
    double secs = 1.0, warm = 0.2;
    int csv = 0;
    static struct option lo[] = {{"mem-node", required_argument, 0, 1001},
                                 {"cpus", required_argument, 0, 1002},
                                 {"wb", required_argument, 0, 1003},
                                 {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "b:T:o:s:W:CH", lo, NULL)) != -1) {
        switch (c) {
        case 'b':
            if (ncl_backend_parse(optarg, &mc.backend) ||
                (mc.backend != NCL_BACKEND_UC && mc.backend != NCL_BACKEND_CACHED_FLUSH &&
                 mc.backend != NCL_BACKEND_COHERENT)) {
                fprintf(stderr, "-b: uc | cached-flush | coherent\n");
                return 2;
            }
            break;
        case 'T': tlist = optarg; break;
        case 'o': olist = optarg; break;
        case 's': secs = atof(optarg); break;
        case 'W': warm = atof(optarg); break;
        case 'C': csv = 1; break;
        case 'H': puts(HEADER); return 0;
        case 1001: mc.mem_node = atoi(optarg) + 1; break;
        case 1002: if (ncl_cpus_set(optarg) < 0) return 2; break;
        case 1003:
            if (!strcmp(optarg, "clwb")) { if (ncl_wb_mode_set(NCL_WB_CLWB) < 0) return 2; }
            else if (strcmp(optarg, "clflushopt")) { fprintf(stderr, "--wb: clflushopt|clwb\n"); return 2; }
            break;
        default:
            fprintf(stderr, "usage: %s -b uc|cached-flush|coherent [-T 1,2,4] [-o ops] [-s S] "
                            "[-W S] [--mem-node N] [--cpus LIST] [--wb clwb] [-C] [-H]\n"
                            "ops: load store storef dekker shload wb64 nt8 nt64\n", argv[0]);
            return 2;
        }
    }
    int ops[OP_N], nops = 0;
    for (char *s = strdup(olist), *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
        int k = 0;
        while (k < OP_N && strcmp(tok, op_names[k]))
            k++;
        if (k == OP_N) { fprintf(stderr, "unknown op %s\n", tok); return 2; }
        ops[nops++] = k;
    }
    int ts[64], nts = 0;
    for (char *s = strdup(tlist), *tok = strtok(s, ","); tok && nts < 64; tok = strtok(NULL, ","))
        ts[nts++] = atoi(tok);

    ncl_cpus_init();
    for (int i = 0; i < nts; i++)
        if (ts[i] < 1 || ts[i] > MAXT || ts[i] > ncl_cpus_count()) {
            fprintf(stderr, "ncl_memconc: %d threads, but %d CPUs (max %d): refused, threads "
                            "would share CPUs\n", ts[i], ncl_cpus_count(), MAXT);
            return 2;
        }
    if (ncl_mem_init(&mc))
        return 1;
    mlines = (ncl_line_t *)ncl_mem.meta + 64;
    shared_line = mlines + MAXT * META_STRIDE + 8;
    dlines = ncl_mem.data;
    if (!csv) {
        printf("mem: %s\ncpus: %s\n", ncl_mem_describe(), ncl_cpus_describe());
        printf("%-7s %4s %10s %14s %8s\n", "op", "T", "Mops/s", "ns/op/thread", "min/max");
    }
    char cpus_csv[512];
    snprintf(cpus_csv, sizeof cpus_csv, "%s", ncl_cpus_describe());
    for (char *p = cpus_csv; *p; p++)
        if (*p == ',')
            *p = ';';

    for (int oi = 0; oi < nops; oi++) {
        for (int ti = 0; ti < nts; ti++) {
            op = ops[oi];
            nthreads = ts[ti];
            atomic_store(&phase, 0);
            pthread_barrier_init(&bar, NULL, (unsigned)nthreads + 1);
            pthread_t th[MAXT];
            for (int t = 0; t < nthreads; t++)
                pthread_create(&th[t], NULL, worker, (void *)(intptr_t)t);
            pthread_barrier_wait(&bar);
            sleep_s(warm);
            double t0 = now_s();
            atomic_store(&phase, 1);
            sleep_s(secs);
            atomic_store(&phase, 2);
            double el = now_s() - t0;
            uint64_t tot = 0, mn = UINT64_MAX, mx = 0;
            for (int t = 0; t < nthreads; t++) {
                pthread_join(th[t], NULL);
                tot += counted[t];
                if (counted[t] < mn) mn = counted[t];
                if (counted[t] > mx) mx = counted[t];
            }
            pthread_barrier_destroy(&bar);
            double mops = tot / el / 1e6;
            double nspt = tot ? nthreads * el * 1e9 / tot : 0;
            double mm = mx ? (double)mn / mx : 0;
            if (csv)
                printf("%s,%s,%d,%d,%s,%s,%d,%.3f,%.3f,%.1f,%.3f,%s\n",
                       ncl_backend_name(mc.backend), ncl_mem.meta_memtype, ncl_mem_node(),
                       mc.backend == NCL_BACKEND_UC ? ncl_mem.uc_pool_node : -1,
                       ncl_wb_mode_name(), op_names[op], nthreads, el, mops, nspt, mm, cpus_csv);
            else
                printf("%-7s %4d %10.2f %14.1f %8.3f\n", op_names[op], nthreads, mops, nspt, mm);
            fflush(stdout);
        }
    }
    ncl_mem_fini();
    return 0;
}
