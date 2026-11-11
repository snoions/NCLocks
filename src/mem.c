#define _GNU_SOURCE
#include "ncl/mem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "../kmod/ncl_uc_ioctl.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int ncl_meta_batch = 1;
struct ncl_mem ncl_mem;

/* Default VA for the shared region: high, unlikely to collide. */
#define NCL_DEFAULT_BASE ((uintptr_t)0x600000000000ULL)

static double tsc_per_ns = 1.0;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t cycles(void)
{
#if defined(__x86_64__)
    return __rdtsc();
#else
    return now_ns();
#endif
}

static void calibrate_tsc(void)
{
#if defined(__x86_64__)
    uint64_t t0 = now_ns(), c0 = cycles();
    while (now_ns() - t0 < 20000000ull) /* 20 ms */
        ;
    uint64_t t1 = now_ns(), c1 = cycles();
    tsc_per_ns = (double)(c1 - c0) / (double)(t1 - t0);
#else
    tsc_per_ns = 1.0;
#endif
}

uint64_t ncl_ns_to_cycles(uint64_t ns)
{
    return (uint64_t)((double)ns * tsc_per_ns + 0.5);
}

void ncl_lat_spin_cycles(uint64_t c)
{
    uint64_t start = cycles();
    while (cycles() - start < c)
        ncl_cpu_relax();
}

static size_t round_page(size_t n)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    return (n + pg - 1) / pg * pg;
}

static void *map_fixed(uintptr_t addr, size_t len, int prot, int flags, int fd,
                       off_t off, const char *what)
{
    void *p = mmap((void *)addr, len, prot, flags | MAP_FIXED_NOREPLACE, fd, off);
    if (p == MAP_FAILED) {
        fprintf(stderr,
                "ncl: cannot map %s at fixed address %#lx (+%zu): %s\n"
                "ncl: refusing to fall back to another address (design §1)\n",
                what, (unsigned long)addr, len, strerror(errno));
        return NULL;
    }
    if ((uintptr_t)p != addr) {
        /* Old kernels ignore MAP_FIXED_NOREPLACE and treat it as a hint. */
        fprintf(stderr, "ncl: %s mapped at %p instead of %#lx; aborting\n",
                what, p, (unsigned long)addr);
        munmap(p, len);
        return NULL;
    }
    return p;
}

static const char *cm_name(int cm)
{
    switch (cm) {
    case NCL_UC_CM_WB: return "WB";
    case NCL_UC_CM_WC: return "WC";
    case NCL_UC_CM_UC_MINUS: return "UC-";
    case NCL_UC_CM_UC: return "UC";
    case NCL_UC_CM_WT: return "WT";
    case NCL_UC_CM_WP: return "WP";
    }
    return "unknown";
}

/* Mean latency of plain 8-byte loads from the metadata mapping, spread over
 * distinct lines. Measures the mapping's memory type, not the backend's access
 * routine (no flushes). UC loads go to DRAM every time (~100 ns); WB loads of
 * a small, repeatedly touched set hit in cache (~1 ns). */
static double measure_meta_load_ns(void)
{
    size_t span = ncl_mem.meta_size < (64u << 10) ? ncl_mem.meta_size : (64u << 10);
    size_t nlines = span / NCL_CACHELINE;
    const int reps = 4000;
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < nlines; i++) /* warm up (matters only for WB) */
        sink += *(volatile uint64_t *)(ncl_mem.meta + i * NCL_CACHELINE);
    uint64_t t0 = now_ns();
    for (int r = 0; r < reps; r++)
        sink += *(volatile uint64_t *)(ncl_mem.meta + ((size_t)r % nlines) * NCL_CACHELINE);
    uint64_t t1 = now_ns();
    (void)sink;
    return (double)(t1 - t0) / reps;
}

/* Below this a metadata load is certainly being served from cache. */
#define NCL_UC_MIN_LOAD_NS 20.0

static int map_uc_meta(uintptr_t base)
{
    const char *path = getenv("NCL_UC_DEV") ? getenv("NCL_UC_DEV") : NCL_UC_DEV_PATH;
    int accept_any = getenv("NCL_UC_ACCEPT_ANY_MEMTYPE") != NULL;

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        fprintf(stderr, "ncl: uc backend: cannot open %s: %s\n", path, strerror(e));
        if (e == EBUSY)
            fprintf(stderr, "ncl: another experiment holds the device (one at a time)\n");
        else if (e == ENOENT)
            fprintf(stderr, "ncl: build and load kmod/ncl_uc.ko (see kmod/README.md)\n");
        return -1;
    }
    ncl_mem.uc_fd = fd;

    struct ncl_uc_info info;
    if (ioctl(fd, NCL_UC_IOC_INFO, &info) < 0) {
        perror("ncl: NCL_UC_IOC_INFO");
        return -1;
    }
    if (info.size < ncl_mem.meta_size) {
        fprintf(stderr, "ncl: uc pool %llu B < metadata %zu B; reload with larger size_mb\n",
                (unsigned long long)info.size, ncl_mem.meta_size);
        return -1;
    }
    if (!info.pat_enabled && !accept_any) {
        fprintf(stderr, "ncl: PAT disabled on this kernel; mapping would not be uncached\n");
        return -1;
    }
    uint64_t zlen = ncl_mem.meta_size;
    if (ioctl(fd, NCL_UC_IOC_ZERO, &zlen) < 0) {
        perror("ncl: NCL_UC_IOC_ZERO");
        return -1;
    }

    ncl_mem.meta = map_fixed(base, ncl_mem.meta_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0,
                             "metadata (uc)");
    if (!ncl_mem.meta)
        return -1;

    /* Verify the live PTEs, not what we asked for. */
    int cm = NCL_UC_CM_UNKNOWN;
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t off = 0; off < ncl_mem.meta_size; off += pg) {
        struct ncl_uc_probe pr = {.addr = (uint64_t)(uintptr_t)(ncl_mem.meta + off)};
        if (ioctl(fd, NCL_UC_IOC_PROBE, &pr) < 0) {
            perror("ncl: NCL_UC_IOC_PROBE");
            return -1;
        }
        if (off == 0)
            cm = pr.cache_mode;
        else if (pr.cache_mode != cm) {
            fprintf(stderr, "ncl: metadata pages have mixed memory types (%s vs %s)\n",
                    cm_name(cm), cm_name(pr.cache_mode));
            return -1;
        }
    }
    ncl_mem.meta_memtype = cm_name(cm);
    /* UC/UC- and WB stores are ordered among themselves on x86; anything else
     * (WC, WT/WP, unknown) gets the hardware sfence in ncl_fence_ss. */
    ncl_mem.meta_store_sfence = !(cm == NCL_UC_CM_UC || cm == NCL_UC_CM_UC_MINUS ||
                                  cm == NCL_UC_CM_WB);
    if (cm != NCL_UC_CM_UC && cm != NCL_UC_CM_UC_MINUS && !accept_any) {
        fprintf(stderr, "ncl: metadata PTE memory type is %s, not UC; refusing to run "
                        "(set NCL_UC_ACCEPT_ANY_MEMTYPE=1 for deliberate WC/WB runs)\n",
                cm_name(cm));
        return -1;
    }
    return 0;
}

int ncl_mem_init(const struct ncl_mem_cfg *cfg)
{
    memset(&ncl_mem, 0, sizeof ncl_mem);
    ncl_mem.backend = cfg->backend;
    ncl_mem.meta_size = round_page(cfg->meta_size);
    ncl_mem.data_size = round_page(cfg->data_size ? cfg->data_size : 1);
    ncl_mem.dev_lat_ns = cfg->dev_lat_ns;
    ncl_mem.memfd = -1;
    ncl_mem.uc_fd = -1;
    ncl_mem.meta_memtype = "WB";
    ncl_mem.meta_store_sfence = cfg->backend == NCL_BACKEND_CACHED_FLUSH;

    calibrate_tsc();
    ncl_mem.lat_cycles = cfg->dev_lat_ns ? ncl_ns_to_cycles(cfg->dev_lat_ns) : 0;

    /* memfd layout: [meta (unused by UC backend)] [data] */
    size_t dev_size = ncl_mem.meta_size + ncl_mem.data_size;
    int fd = memfd_create("ncl-device", 0);
    if (fd < 0) {
        perror("ncl: memfd_create");
        return -1;
    }
    if (ftruncate(fd, (off_t)dev_size) < 0) {
        perror("ncl: ftruncate");
        close(fd);
        return -1;
    }
    ncl_mem.memfd = fd;

    uintptr_t base = cfg->base_addr ? cfg->base_addr : NCL_DEFAULT_BASE;
    int prot = PROT_READ | PROT_WRITE;

    if (cfg->backend == NCL_BACKEND_UC) {
        if (map_uc_meta(base) < 0)
            goto fail;
    } else {
        ncl_mem.meta = map_fixed(base, ncl_mem.meta_size, prot, MAP_SHARED, fd, 0, "metadata");
        if (!ncl_mem.meta)
            goto fail;
    }

    uintptr_t data_va = base + ncl_mem.meta_size;
    if (cfg->backend == NCL_BACKEND_EMU) {
        /* Node view = private anonymous memory (the node's "cache"); each
         * forked node gets its own copy-on-write image. */
        ncl_mem.data = map_fixed(data_va, ncl_mem.data_size, prot,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, "data (node view)");
        if (!ncl_mem.data)
            goto fail;
        ncl_mem.data_dev = map_fixed(data_va + ncl_mem.data_size, ncl_mem.data_size, prot,
                                     MAP_SHARED, fd, (off_t)ncl_mem.meta_size,
                                     "data (device view)");
        if (!ncl_mem.data_dev)
            goto fail;
    } else {
        ncl_mem.data = map_fixed(data_va, ncl_mem.data_size, prot, MAP_SHARED, fd,
                                 (off_t)ncl_mem.meta_size, "data");
        if (!ncl_mem.data)
            goto fail;
        ncl_mem.data_dev = ncl_mem.data;
    }

    ncl_mem.meta_load_ns = measure_meta_load_ns();
    if (cfg->backend == NCL_BACKEND_UC && ncl_mem.meta_load_ns < NCL_UC_MIN_LOAD_NS &&
        !getenv("NCL_UC_ACCEPT_ANY_MEMTYPE")) {
        fprintf(stderr, "ncl: metadata loads take %.1f ns (< %.0f ns): the mapping is being "
                        "cached despite PTE type %s; refusing to run\n",
                ncl_mem.meta_load_ns, NCL_UC_MIN_LOAD_NS, ncl_mem.meta_memtype);
        goto fail;
    }
    return 0;

fail:
    ncl_mem_fini();
    return -1;
}

const char *ncl_mem_describe(void)
{
    static char buf[160];
    snprintf(buf, sizeof buf, "backend=%s meta_memtype=%s meta_load=%.1fns",
             ncl_backend_name(ncl_mem.backend), ncl_mem.meta_memtype, ncl_mem.meta_load_ns);
    return buf;
}

void ncl_mem_fini(void)
{
    if (ncl_mem.meta)
        munmap(ncl_mem.meta, ncl_mem.meta_size);
    if (ncl_mem.data)
        munmap(ncl_mem.data, ncl_mem.data_size);
    if (ncl_mem.data_dev && ncl_mem.data_dev != ncl_mem.data)
        munmap(ncl_mem.data_dev, ncl_mem.data_size);
    if (ncl_mem.memfd >= 0)
        close(ncl_mem.memfd);
    if (ncl_mem.uc_fd >= 0)
        close(ncl_mem.uc_fd);
    memset(&ncl_mem, 0, sizeof ncl_mem);
    ncl_mem.memfd = -1;
    ncl_mem.uc_fd = -1;
}

void ncl_mem_node_attach(void)
{
    if (ncl_mem.backend == NCL_BACKEND_EMU)
        ncl_data_invalidate(ncl_mem.data, ncl_mem.data_size);
}

const char *ncl_backend_name(ncl_backend_t b)
{
    switch (b) {
    case NCL_BACKEND_COHERENT: return "coherent";
    case NCL_BACKEND_CACHED_FLUSH: return "cached-flush";
    case NCL_BACKEND_EMU: return "emu";
    case NCL_BACKEND_UC: return "uc";
    }
    return "?";
}

int ncl_backend_parse(const char *s, ncl_backend_t *out)
{
    if (!strcmp(s, "coherent")) *out = NCL_BACKEND_COHERENT;
    else if (!strcmp(s, "cached-flush")) *out = NCL_BACKEND_CACHED_FLUSH;
    else if (!strcmp(s, "emu")) *out = NCL_BACKEND_EMU;
    else if (!strcmp(s, "uc")) *out = NCL_BACKEND_UC;
    else return -1;
    return 0;
}

/* ---- data writeback / invalidate --------------------------------------- */

static void line_range(const void *p, size_t len, uintptr_t *lo, uintptr_t *hi)
{
    *lo = (uintptr_t)p & ~(uintptr_t)(NCL_CACHELINE - 1);
    *hi = ((uintptr_t)p + len + NCL_CACHELINE - 1) & ~(uintptr_t)(NCL_CACHELINE - 1);
}

/* EMU: copy whole lines with 8-byte relaxed atomics. Each word is atomic at
 * the device but the line as a whole is not (design §2 rule 1). */
static void emu_copy_lines(char *dst, const char *src, size_t len)
{
    _Atomic uint64_t *d = (_Atomic uint64_t *)dst;
    const _Atomic uint64_t *s = (const _Atomic uint64_t *)src;
    for (size_t i = 0; i < len / sizeof(uint64_t); i++)
        atomic_store_explicit(&d[i],
                              atomic_load_explicit((_Atomic uint64_t *)&s[i], memory_order_relaxed),
                              memory_order_relaxed);
}

void ncl_data_writeback(const void *p, size_t len)
{
    if (len == 0)
        return;
    uintptr_t lo, hi;
    line_range(p, len, &lo, &hi);

    switch (ncl_mem.backend) {
    case NCL_BACKEND_COHERENT:
        break;
    case NCL_BACKEND_CACHED_FLUSH:
    case NCL_BACKEND_UC: /* data is cached in both */
        for (uintptr_t a = lo; a < hi; a += NCL_CACHELINE) {
            ncl_dev_delay();
            ncl__flushopt((const void *)a);
        }
        break;
    case NCL_BACKEND_EMU: {
        size_t off = lo - (uintptr_t)ncl_mem.data;
        for (uintptr_t a = lo; a < hi; a += NCL_CACHELINE)
            ncl_dev_delay();
        emu_copy_lines(ncl_mem.data_dev + off, (const char *)lo, hi - lo);
        break;
    }
    }
    ncl_fence();
}

void ncl_data_invalidate(const void *p, size_t len)
{
    if (len == 0)
        return;
    uintptr_t lo, hi;
    line_range(p, len, &lo, &hi);

    ncl_fence();
    switch (ncl_mem.backend) {
    case NCL_BACKEND_COHERENT:
        break;
    case NCL_BACKEND_CACHED_FLUSH:
    case NCL_BACKEND_UC: /* data is cached in both */
        /* clflushopt both writes back and invalidates; there is no
         * invalidate-only instruction usable from user space. */
        for (uintptr_t a = lo; a < hi; a += NCL_CACHELINE) {
            ncl_dev_delay();
            ncl__flushopt((const void *)a);
        }
        break;
    case NCL_BACKEND_EMU: {
        size_t off = lo - (uintptr_t)ncl_mem.data;
        for (uintptr_t a = lo; a < hi; a += NCL_CACHELINE)
            ncl_dev_delay();
        emu_copy_lines((char *)lo, ncl_mem.data_dev + off, hi - lo);
        break;
    }
    }
    ncl_fence();
}
