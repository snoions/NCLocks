#define _GNU_SOURCE
#include "ncl/mem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

#include "../kmod/ncl_uc_ioctl.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int ncl_meta_batch = NCL_META_PROBE;
struct ncl_mem ncl_mem;
static int wb_mode = NCL_WB_CLFLUSHOPT; /* survives ncl_mem_init's reset */

int ncl_wb_mode_set(int mode)
{
    if (mode == NCL_WB_CLWB) {
#if defined(__x86_64__)
        unsigned a, b, c, d;
        if (!__get_cpuid_count(7, 0, &a, &b, &c, &d) || !(b & (1u << 24))) {
            fprintf(stderr, "ncl: this CPU has no clwb\n");
            return -1;
        }
#else
        fprintf(stderr, "ncl: clwb is x86-only\n");
        return -1;
#endif
    }
    wb_mode = mode;
    ncl_mem.wb_clwb = mode == NCL_WB_CLWB;
    return 0;
}

const char *ncl_wb_mode_name(void) { return wb_mode == NCL_WB_CLWB ? "clwb" : "clflushopt"; }

/* ---- NUMA placement of the shared regions (no libnuma dependency) ------- */

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif
#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT 1
#endif
#define NCL_NUMA_MAXNODE 1024

/* Bind [p, p+len) to `node` before anything touches it. For a MAP_SHARED
 * memfd mapping this sets the shared (inode) policy, so it holds for every
 * process that maps the file. */
static int bind_region(void *p, size_t len, int node, const char *what)
{
    unsigned long mask[NCL_NUMA_MAXNODE / (8 * sizeof(unsigned long))] = {0};
    if (node < 0 || node >= NCL_NUMA_MAXNODE)
        return -1;
    mask[node / (8 * sizeof(unsigned long))] = 1ul << (node % (8 * sizeof(unsigned long)));
    if (syscall(SYS_mbind, p, len, MPOL_BIND, mask, (unsigned long)NCL_NUMA_MAXNODE,
                MPOL_MF_STRICT) < 0) {
        fprintf(stderr, "ncl: mbind %s to NUMA node %d: %s\n", what, node, strerror(errno));
        return -1;
    }
    return 0;
}

/* Fault every page of [p, p+len) in (writing the zero it already holds) and
 * check that each one landed on `node` (node < 0: just report the first
 * page's node). Returns the node of the first page, or -2 on a mismatch. */
static int touch_and_check(char *p, size_t len, int node, const char *what)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), n = len / pg;
    for (size_t i = 0; i < n; i++)
        *(volatile char *)(p + i * pg) = *(volatile char *)(p + i * pg);
    enum { CH = 512 };
    void *pages[CH];
    int status[CH];
    int first = -1;
    for (size_t i = 0; i < n; i += CH) {
        size_t m = n - i < CH ? n - i : CH;
        for (size_t k = 0; k < m; k++)
            pages[k] = p + (i + k) * pg;
        if (syscall(SYS_move_pages, 0, (unsigned long)m, pages, NULL, status, 0) < 0) {
            if (node >= 0) {
                fprintf(stderr, "ncl: move_pages(%s): %s\n", what, strerror(errno));
                return -2;
            }
            return -1; /* reporting only */
        }
        for (size_t k = 0; k < m; k++) {
            if (i + k == 0)
                first = status[k];
            if (node >= 0 && status[k] != node) {
                fprintf(stderr, "ncl: %s page %zu is on NUMA node %d, not %d; refusing to run\n",
                        what, i + k, status[k], node);
                return -2;
            }
        }
    }
    return first;
}

/* Bind (if node >= 0) and verify one shared region. */
static int place_region(char *p, size_t len, int node, const char *what)
{
    if (node >= 0 && bind_region(p, len, node, what) < 0)
        return -1;
    int got = touch_and_check(p, len, node, what);
    if (got == -2)
        return -1;
    if (node < 0 && ncl_mem.mem_node == -1)
        ncl_mem.mem_node = -(got + 2); /* unbound: remember where it landed (<= -2) */
    return 0;
}

/* Default VA for the shared region: high, unlikely to collide. */
#define NCL_DEFAULT_BASE ((uintptr_t)0x600000000000ULL)

static double tsc_per_ns = 1.0;
static void emu_copy_lines(char *dst, const char *src, size_t len);

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

static int map_uc_meta(uintptr_t base, size_t len)
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
        /* Driver built before the nid field: same layout minus nid/pad. Its
         * pool node is unknown (-1), so --mem-node refuses uc with it. */
        struct { __u64 size; __s32 memtype, pat_enabled; } old;
        if (errno != ENOTTY ||
            ioctl(fd, _IOR('N', 1, old), &old) < 0) {
            perror("ncl: NCL_UC_IOC_INFO");
            return -1;
        }
        info.size = old.size;
        info.memtype = old.memtype;
        info.pat_enabled = old.pat_enabled;
        info.nid = -1;
    }
    ncl_mem.uc_pool_node = info.nid;
    if (info.size < len) {
        fprintf(stderr, "ncl: uc pool %llu B < metadata %zu B; reload with larger size_mb\n",
                (unsigned long long)info.size, len);
        return -1;
    }
    if (!info.pat_enabled && !accept_any) {
        fprintf(stderr, "ncl: PAT disabled on this kernel; mapping would not be uncached\n");
        return -1;
    }
    uint64_t zlen = len;
    if (ioctl(fd, NCL_UC_IOC_ZERO, &zlen) < 0) {
        perror("ncl: NCL_UC_IOC_ZERO");
        return -1;
    }

    ncl_mem.meta = map_fixed(base, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0,
                             "metadata (uc)");
    if (!ncl_mem.meta)
        return -1;

    /* Verify the live PTEs, not what we asked for. */
    int cm = NCL_UC_CM_UNKNOWN;
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t off = 0; off < len; off += pg) {
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
    ncl_mem.scan_size = round_page(cfg->scan_size ? cfg->scan_size : 1);
    ncl_mem.split = cfg->meta_split;
    ncl_mem.data_size = round_page(cfg->data_size ? cfg->data_size : 1);
    ncl_mem.dev_lat_ns = cfg->dev_lat_ns;
    ncl_mem.memfd = -1;
    ncl_mem.uc_fd = -1;
    ncl_mem.meta_memtype = "WB";
    ncl_mem.meta_store_sfence = cfg->backend == NCL_BACKEND_CACHED_FLUSH;
    ncl_mem.wb_clwb = wb_mode == NCL_WB_CLWB;
    ncl_mem.mem_node = -1;
    ncl_mem.uc_pool_node = -1;
    int node = cfg->mem_node - 1;
    if (!cfg->mem_node && getenv("NCL_MEM_NODE"))
        node = atoi(getenv("NCL_MEM_NODE"));
    if (cfg->meta_split && cfg->backend != NCL_BACKEND_UC && cfg->backend != NCL_BACKEND_EMU) {
        fprintf(stderr, "ncl: split metadata needs the uc or emu backend (%s metadata is "
                        "uniform by definition)\n", ncl_backend_name(cfg->backend));
        return -1;
    }

    calibrate_tsc();
    ncl_mem.lat_cycles = cfg->dev_lat_ns ? ncl_ns_to_cycles(cfg->dev_lat_ns) : 0;

    /* memfd layout: [meta (unused by UC backend)] [scan (unused by uniform UC)] [data] */
    size_t meta_all = ncl_mem.meta_size + ncl_mem.scan_size;
    size_t dev_size = meta_all + ncl_mem.data_size;
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

    /* Uniform: meta and scan are one mapping of the backend's metadata type.
     * Split: meta alone, then scan mapped cached (see mem.h). */
    size_t meta_map = ncl_mem.split ? ncl_mem.meta_size : meta_all;
    if (cfg->backend == NCL_BACKEND_UC) {
        if (map_uc_meta(base, meta_map) < 0)
            goto fail;
    } else {
        ncl_mem.meta = map_fixed(base, meta_map, prot, MAP_SHARED, fd, 0, "metadata");
        if (!ncl_mem.meta || place_region(ncl_mem.meta, meta_map, node, "metadata") < 0)
            goto fail;
    }
    uintptr_t scan_va = base + ncl_mem.meta_size;
    uintptr_t data_va = scan_va + ncl_mem.scan_size;
    if (!ncl_mem.split) {
        ncl_mem.scan = ncl_mem.scan_dev = ncl_mem.meta + ncl_mem.meta_size;
    } else if (cfg->backend == NCL_BACKEND_EMU) {
        ncl_mem.scan = map_fixed(scan_va, ncl_mem.scan_size, prot, MAP_PRIVATE | MAP_ANONYMOUS,
                                 -1, 0, "scan metadata (node view)");
        if (!ncl_mem.scan)
            goto fail;
        ncl_mem.scan_dev = map_fixed(data_va + 2 * ncl_mem.data_size, ncl_mem.scan_size, prot,
                                     MAP_SHARED, fd, (off_t)ncl_mem.meta_size,
                                     "scan metadata (device view)");
        if (!ncl_mem.scan_dev ||
            place_region(ncl_mem.scan_dev, ncl_mem.scan_size, node, "scan metadata") < 0)
            goto fail;
    } else {
        ncl_mem.scan = ncl_mem.scan_dev = map_fixed(scan_va, ncl_mem.scan_size, prot, MAP_SHARED,
                                                    fd, (off_t)ncl_mem.meta_size,
                                                    "scan metadata (cached)");
        if (!ncl_mem.scan || place_region(ncl_mem.scan, ncl_mem.scan_size, node, "scan metadata") < 0)
            goto fail;
    }
    if (ncl_mem.split) {
        ncl_mem.cm_lo = (uintptr_t)ncl_mem.scan;
        ncl_mem.cm_len = ncl_mem.scan_size;
        ncl_mem.meta_store_sfence = 1; /* clflushopt is weakly ordered with later stores */
    }

    if (cfg->backend == NCL_BACKEND_EMU) {
        /* Node view = private anonymous memory (the node's "cache"); each
         * forked node gets its own copy-on-write image. */
        ncl_mem.data = map_fixed(data_va, ncl_mem.data_size, prot,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, "data (node view)");
        if (!ncl_mem.data)
            goto fail;
        ncl_mem.data_dev = map_fixed(data_va + ncl_mem.data_size, ncl_mem.data_size, prot,
                                     MAP_SHARED, fd, (off_t)meta_all, "data (device view)");
        if (!ncl_mem.data_dev || place_region(ncl_mem.data_dev, ncl_mem.data_size, node, "data") < 0)
            goto fail;
    } else {
        ncl_mem.data = map_fixed(data_va, ncl_mem.data_size, prot, MAP_SHARED, fd,
                                 (off_t)meta_all, "data");
        if (!ncl_mem.data || place_region(ncl_mem.data, ncl_mem.data_size, node, "data") < 0)
            goto fail;
        ncl_mem.data_dev = ncl_mem.data;
    }

    if (node >= 0)
        ncl_mem.mem_node = node;
    if (cfg->backend == NCL_BACKEND_UC && node >= 0 && ncl_mem.uc_pool_node != node) {
        fprintf(stderr, "ncl: uc pool pages are on NUMA node %d, not %d: reload the driver "
                        "with nid=%d (kmod/README.md); refusing to run\n",
                ncl_mem.uc_pool_node, node, node);
        goto fail;
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

/* NUMA node of the shared regions: bound node, else where the first page
 * landed (-1 = unknown). */
int ncl_mem_node(void)
{
    return ncl_mem.mem_node >= 0 ? ncl_mem.mem_node : ncl_mem.mem_node <= -2 ? -(ncl_mem.mem_node + 2) : -1;
}

const char *ncl_mem_describe(void)
{
    static char buf[256];
    char pool[32] = "";
    if (ncl_mem.backend == NCL_BACKEND_UC)
        snprintf(pool, sizeof pool, " uc_pool_node=%d", ncl_mem.uc_pool_node);
    snprintf(buf, sizeof buf, "backend=%s meta_memtype=%s meta_load=%.1fns scan=%s mem_node=%d%s%s wb=%s",
             ncl_backend_name(ncl_mem.backend), ncl_mem.meta_memtype, ncl_mem.meta_load_ns,
             !ncl_mem.split ? "uniform"
             : ncl_mem.backend == NCL_BACKEND_EMU ? "split(emu-cached)" : "split(WB+clflush)",
             ncl_mem_node(), ncl_mem.mem_node >= 0 ? "(bound)" : "(default)", pool,
             ncl_wb_mode_name());
    return buf;
}

void ncl_mem_fini(void)
{
    if (ncl_mem.meta)
        munmap(ncl_mem.meta, ncl_mem.split ? ncl_mem.meta_size : ncl_mem.meta_size + ncl_mem.scan_size);
    if (ncl_mem.split && ncl_mem.scan)
        munmap(ncl_mem.scan, ncl_mem.scan_size);
    if (ncl_mem.split && ncl_mem.scan_dev && ncl_mem.scan_dev != ncl_mem.scan)
        munmap(ncl_mem.scan_dev, ncl_mem.scan_size);
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
    if (ncl_mem.backend == NCL_BACKEND_EMU && ncl_mem.split)
        emu_copy_lines(ncl_mem.scan, ncl_mem.scan_dev, ncl_mem.scan_size);
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
    ncl_data_writeback_nofence(p, len);
    ncl_fence();
}

void ncl_data_writeback_nofence(const void *p, size_t len)
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
            ncl__wb((const void *)a);
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
