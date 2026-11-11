/*
 * Memory-access layer (design §1, §2).
 *
 * Every access to shared state goes through this header so the backend can be
 * swapped without touching the lock algorithms:
 *
 *   NCL_BACKEND_COHERENT  Plain coherent shared memory, no cache maintenance.
 *                         The "coherent NUMA" baseline (design §6.5).
 *   NCL_BACKEND_UC        REAL uncached hot metadata: mapped from the
 *                         kmod/ncl_uc driver (PAT UC-, verified at init via
 *                         the live PTE and load timing). No flushes on
 *                         metadata. Data is cached and written back /
 *                         invalidated with clflushopt.
 *   NCL_BACKEND_CACHED_FLUSH  NOT uncached. Hot metadata is ordinary cached
 *                         memory with clflushopt after every store and
 *                         clflushopt+mfence before every load — the "cached
 *                         + explicit writeback/invalidate" alternative, used
 *                         as a stand-in when the driver is unavailable. On a
 *                         coherent host it cannot expose missing flushes.
 *   NCL_BACKEND_EMU       Software non-coherence emulator. Hot metadata lives
 *                         on the "device" (shared memfd, physically WB) and
 *                         is accessed with single 8-byte relaxed atomics, so
 *                         it BEHAVES like uncached memory (no node-private
 *                         copy) but has cached timing. The data
 *                         region seen by each node is a PRIVATE copy (the
 *                         node's "cache"); writeback copies whole lines to the
 *                         device, invalidate copies whole lines back. A missing
 *                         writeback or invalidate therefore yields stale data,
 *                         which the stress test detects.
 *
 * Region layout (same virtual address in every node, design §1 option 1):
 *
 *   base                     meta   (shared, "uncached" hot metadata + config)
 *   base+meta_size           data   (node view; what applications dereference)
 *   base+meta_size+data_size data device view (EMU only)
 *
 * Shared structures still store offsets (ncl_off_t) rather than pointers so a
 * per-node-address backend (design §1 option 2) can be added later.
 *
 * Rules enforced by construction:
 *   - Hot-metadata accesses are exactly one aligned 8-byte load or store.
 *   - Each hot-metadata variable occupies its own cache line (ncl_line_t)
 *     until sub-line device writes are verified (design §2 rule 3, Open).
 *   - Fences are explicit (ncl_fence / ncl_fence_ss) even where x86 makes them
 *     redundant (design §2 rule 4).
 */
#ifndef NCL_MEM_H
#define NCL_MEM_H

#include <stdatomic.h>
#include <stdbool.h>
#include "ncl/config.h"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

typedef enum {
    NCL_BACKEND_COHERENT = 0,
    NCL_BACKEND_CACHED_FLUSH = 1,
    NCL_BACKEND_EMU = 2,
    NCL_BACKEND_UC = 3,
} ncl_backend_t;

typedef uint64_t ncl_off_t;

/* One hot-metadata variable: an 8-byte word alone on its cache line. */
typedef struct {
    _Atomic uint64_t v;
    char pad[NCL_CACHELINE - sizeof(uint64_t)];
} NCL_ALIGNED(NCL_CACHELINE) ncl_line_t;

_Static_assert(sizeof(ncl_line_t) == NCL_CACHELINE, "ncl_line_t size");

struct ncl_mem_cfg {
    ncl_backend_t backend;
    size_t meta_size;      /* bytes, rounded up to page size */
    size_t data_size;      /* bytes, rounded up to page size */
    uintptr_t base_addr;   /* fixed VA for the region; 0 = default */
    uint32_t dev_lat_ns;   /* injected latency per device access (0 = none) */
};

/* Per-process view of the region. Filled by ncl_mem_init, inherited by fork. */
struct ncl_mem {
    ncl_backend_t backend;
    char *meta;            /* hot metadata + static config */
    char *data;            /* node view of protected data */
    char *data_dev;        /* device view of data (== data unless EMU) */
    size_t meta_size;
    size_t data_size;
    uint32_t dev_lat_ns;
    uint64_t lat_cycles;   /* dev_lat_ns converted to TSC cycles */
    int memfd;
    int uc_fd;             /* /dev/ncl_uc (UC backend), else -1 */
    const char *meta_memtype; /* memory type of hot metadata, as verified */
    int meta_store_sfence; /* ncl_fence_ss must emit sfence: metadata stores are
                              weakly ordered (cached-flush: clflushopt after each
                              store; uc: verified PTE type is WC or unknown) */
    double meta_load_ns;   /* measured mean latency of a metadata load */
};

extern struct ncl_mem ncl_mem;

/* Map the region at a fixed address. Fails loudly (returns -1 with message on
 * stderr) if the address range is already in use — never silently relocates. */
int ncl_mem_init(const struct ncl_mem_cfg *cfg);
void ncl_mem_fini(void);

/* One-line description of the metadata mapping (backend, memtype, latency). */
const char *ncl_mem_describe(void);

/* Called in each node process after fork: drops any inherited node-view data
 * (EMU) by re-reading the device copy. */
void ncl_mem_node_attach(void);

const char *ncl_backend_name(ncl_backend_t b);
int ncl_backend_parse(const char *s, ncl_backend_t *out);

/* ---- latency injection -------------------------------------------------- */

void ncl_lat_spin_cycles(uint64_t cycles);
uint64_t ncl_ns_to_cycles(uint64_t ns);

static inline void ncl_dev_delay(void)
{
    if (NCL_UNLIKELY(ncl_mem.lat_cycles))
        ncl_lat_spin_cycles(ncl_mem.lat_cycles);
}

/* ---- fences ------------------------------------------------------------- */

/* Full fence: orders all earlier loads/stores/flushes before later ones.
 * Used between a store and a later load whose order matters (Dekker pattern),
 * and wherever cached and uncached accesses mix. */
static inline void ncl_fence(void)
{
#if defined(__x86_64__)
    _mm_mfence();
#endif
    atomic_thread_fence(memory_order_seq_cst);
}

/* Store-store fence: earlier stores become visible before later stores.
 * x86 already keeps ordinary stores in order (SDM 8.2.2), including WB and
 * UC/UC- stores mixed together, so the hardware sfence is needed only when
 * metadata stores are weakly ordered: after clflushopt (cached-flush backend)
 * or on a WC mapping (uc backend run with NCL_UC_ACCEPT_ANY_MEMTYPE). The
 * C11 release fence is always kept: it is the explicit fence the algorithms
 * and the data-race argument rely on, and the compiler barrier. */
static inline void ncl_fence_ss(void)
{
#if defined(__x86_64__)
    if (ncl_mem.meta_store_sfence)
        _mm_sfence();
#endif
    atomic_thread_fence(memory_order_release);
}

/* ---- hot metadata (uncached) ------------------------------------------- */

#if defined(__x86_64__)
#ifndef __CLFLUSHOPT__
#error "build with -mclflushopt (CMake adds it on x86-64)"
#endif
static inline void ncl__flushopt(const void *p) { _mm_clflushopt((void *)p); }
#else
static inline void ncl__flushopt(const void *p) { (void)p; }
#endif

static inline uint64_t ncl_uc_load(ncl_line_t *l)
{
    ncl_dev_delay();
    if (ncl_mem.backend == NCL_BACKEND_CACHED_FLUSH) {
        /* Cached mapping: invalidate before reading what another node wrote. */
        ncl__flushopt(l);
#if defined(__x86_64__)
        _mm_mfence();
#endif
    }
    return atomic_load_explicit(&l->v, memory_order_relaxed);
}

static inline void ncl_uc_store(ncl_line_t *l, uint64_t v)
{
    ncl_dev_delay();
    atomic_store_explicit(&l->v, v, memory_order_relaxed);
    if (ncl_mem.backend == NCL_BACKEND_CACHED_FLUSH)
        ncl__flushopt(l); /* write back so other nodes can see it */
}

/* ---- batched metadata reads -------------------------------------------- */

/* Batch invalidates for multi-line reads (cached-flush only): clflushopt every
 * line, ONE mfence, then all loads, which miss in parallel (about 2 round trips
 * instead of n). On uc (UC loads execute one at a time), emu and coherent the
 * loads stay serial, with early exit where the caller allows it, so behavior
 * there is unchanged. ncl_meta_batch = 0 forces serial everywhere (A/B).
 *
 * Ordering: the batch starts after earlier fences, so it is ordered after
 * earlier stores the same way a single ncl_uc_load is. Loads WITHIN a batch
 * are unordered: on non-coherent memory x86 load-load ordering does not hold
 * (it relies on snoops). A caller that needs one read before another must use
 * separate calls with ncl_fence() between them. Injected device latency
 * (dev_lat_ns) is charged once per batch: the accesses overlap. */
/* 0 = serial, 1 = batch, 2 = probe (scans only: load the first candidate
 * alone and batch the rest only if it is 0; under contention the first,
 * hinted, slot usually decides). Default 2 (probe). */
enum { NCL_META_SERIAL = 0, NCL_META_BATCH = 1, NCL_META_PROBE = 2 };
extern int ncl_meta_batch;

static inline int ncl_meta_batching(void)
{
    return ncl_mem.backend == NCL_BACKEND_CACHED_FLUSH && ncl_meta_batch;
}

/* out[k] = *ls[k] for k < n (all n loads, any backend). */
static inline void ncl_uc_load_many(ncl_line_t *const *ls, int n, uint64_t *out)
{
    if (ncl_meta_batching()) {
        ncl_dev_delay();
        for (int k = 0; k < n; k++)
            ncl__flushopt(ls[k]);
#if defined(__x86_64__)
        _mm_mfence();
#endif
        for (int k = 0; k < n; k++)
            out[k] = atomic_load_explicit(&ls[k]->v, memory_order_relaxed);
    } else {
        for (int k = 0; k < n; k++)
            out[k] = ncl_uc_load(ls[k]);
    }
}

/* First j = (start + k) % n, k < count, j != skip, with arr[j] != 0; -1 if none.
 * Serial backends stop at the first hit; batched reads all candidates.
 * *nloads += metadata loads issued. */
static inline int ncl_uc_scan_first(ncl_line_t *arr, int n, int start, int count, int skip,
                                    uint64_t *nloads)
{
    if (ncl_meta_batching()) {
        ncl_line_t *ls[NCL_MAX_PARTICIPANTS];
        int idx[NCL_MAX_PARTICIPANTS];
        uint64_t v[NCL_MAX_PARTICIPANTS];
        int m = 0, k0 = 0;
        if (ncl_meta_batch == NCL_META_PROBE) {
            for (; k0 < count; k0++) {
                int j = (start + k0) % n;
                if (j == skip)
                    continue;
                ++*nloads;
                if (ncl_uc_load(&arr[j]))
                    return j;
                k0++;
                break;
            }
        }
        for (int k = k0; k < count; k++) {
            int j = (start + k) % n;
            if (j != skip) {
                idx[m] = j;
                ls[m++] = &arr[j];
            }
        }
        if (m == 0)
            return -1;
        ncl_uc_load_many(ls, m, v);
        *nloads += (uint64_t)m;
        for (int k = 0; k < m; k++)
            if (v[k])
                return idx[k];
        return -1;
    }
    for (int k = 0; k < count; k++) {
        int j = (start + k) % n;
        if (j == skip)
            continue;
        ++*nloads;
        if (ncl_uc_load(&arr[j]))
            return j;
    }
    return -1;
}

/* ---- protected data (cached) ------------------------------------------- */

/* Make this node's writes to [p, p+len) visible on the device. Whole cache
 * lines are written (a writeback can clobber a neighbor on the same line —
 * callers must not share lines between independently locked data). */
void ncl_data_writeback(const void *p, size_t len);
/* Same, but only ISSUES the writeback: no trailing fence. The caller must
 * ncl_fence() before any store that lets another node read the data (that
 * fence waits for the clflushopts). Lets independent loads overlap the
 * flushes (docs/backends.md). */
void ncl_data_writeback_nofence(const void *p, size_t len);

/* Discard this node's cached copy of [p, p+len) so later reads fetch the
 * device contents. Must not be called while holding unwritten dirty data in
 * the range (EMU discards it, matching an invalidate-without-writeback). */
void ncl_data_invalidate(const void *p, size_t len);

/* Offset <-> pointer helpers (data region, node view). */
static inline void *ncl_data_ptr(ncl_off_t off) { return ncl_mem.data + off; }
static inline ncl_off_t ncl_data_off(const void *p)
{
    return (ncl_off_t)((const char *)p - ncl_mem.data);
}
static inline void *ncl_meta_ptr(ncl_off_t off) { return ncl_mem.meta + off; }

#endif
