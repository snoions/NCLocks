/*
 * Per-operation cost of metadata access patterns on the uc and cached-flush
 * backends (docs/backends.md): single load, store + fence, a K-line scan read
 * serially vs batched (K clflushopt, one mfence, K loads), and one Dekker step.
 * Single process, one core; usage: ncl_memlat uc|cached-flush [K]
 * The "merged" row (invalidate issued before the fence that orders the store)
 * is shown for its cost only: it is NOT safe on non-coherent memory.
 */
#define _GNU_SOURCE
#include <time.h>
#include "ncl/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>
#define IT 200000
static ncl_line_t *L;
static double ns_per_cycle;
static double tsc_ns(void){ struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a); uint64_t t=__rdtsc(); do clock_gettime(CLOCK_MONOTONIC,&b); while((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec)<2e8); uint64_t t2=__rdtsc(); return ((b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec))/(double)(t2-t);}
#define TIME(name, body) do { uint64_t s=0; for (int it=0; it<1000; it++){body;} uint64_t t0=__rdtsc(); for (int it=0; it<IT; it++){body;} s=__rdtsc()-t0; printf("  %-46s %7.1f ns\n", name, s*ns_per_cycle/IT); } while(0)
int main(int argc, char **argv) {
    ncl_backend_t be;
    if (argc < 2 || ncl_backend_parse(argv[1], &be)) {
        fprintf(stderr, "usage: %s uc|cached-flush [K]\n", argv[0]);
        return 2;
    }
    int K = argc > 2 ? atoi(argv[2]) : 7;
    if (K < 1 || K > 30) return 2;
    struct ncl_mem_cfg c = {.backend = be, .meta_size = 1<<20, .data_size = 1<<20};
    if (ncl_mem_init(&c)) return 1;
    ns_per_cycle = tsc_ns();
    L = (ncl_line_t *)ncl_mem.meta + 64; /* skip header area */
    volatile uint64_t sink = 0;
    printf("%s (%s), K=%d\n", argv[1], ncl_mem_describe(), K);
    TIME("1 load (backend)", sink += ncl_uc_load(&L[0]));
    TIME("store + mfence (backend)", { ncl_uc_store(&L[1], it); ncl_fence(); });
    TIME("K-line scan, serial (backend)", for (int k=0;k<K;k++) sink += ncl_uc_load(&L[2+k*2]));
    if (be == NCL_BACKEND_CACHED_FLUSH) {
        TIME("K-line scan, batched: K flush, 1 mfence, K loads", {
            for (int k=0;k<K;k++) _mm_clflushopt((void*)&L[2+k*2]);
            _mm_mfence();
            for (int k=0;k<K;k++) sink += atomic_load_explicit(&L[2+k*2].v, memory_order_relaxed); });
        TIME("store; flush; flush Y; mfence; load Y (UNSAFE)", {
            atomic_store_explicit(&L[1].v, it, memory_order_relaxed); _mm_clflushopt((void*)&L[1]);
            _mm_clflushopt((void*)&L[0]); _mm_mfence(); sink += atomic_load_explicit(&L[0].v, memory_order_relaxed); });
    }
    TIME("store; mfence; load (Dekker step, backend)", { ncl_uc_store(&L[1], it); ncl_fence(); sink += ncl_uc_load(&L[0]); });
    ncl_mem_fini();
    return 0;
}
