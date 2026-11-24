#include "ncl/lock.h"
#include "ncl/cpus.h"
#include "server.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCL_MAGIC 0x4e434c4f434b5331ull /* "NCLOCKS1" */

/* Static configuration, at meta offset 0. Read-mostly: written by the creator
 * before any node starts, never modified afterwards. */
struct ncl_hdr {
    uint64_t magic;
    struct ncl_sys_cfg cfg;
    int32_t participants;
    uint64_t lines_per_lock;  /* meta-region lines per lock */
    uint64_t scan_per_lock;   /* scan-region lines per lock (b[], present[], announce[]) */
    uint64_t base_lines;      /* ...of which the base algorithm's (meta) */
    uint64_t base_scan;       /* ...of which the base algorithm's (scan) */
    ncl_off_t locks_off;  /* meta offset of first lock block */
    ncl_off_t slocks_off; /* scan offset of first lock's scan block */
    ncl_off_t user_off;   /* meta offset of user lines */
    ncl_off_t descs_off;  /* meta offset of struct ncl_lock_desc[nlocks] */
    ncl_off_t done_off;   /* meta offset of per-node "done" lines (detach barrier) */
    struct ncl_srv_layout srv; /* SERVER/FASTSRV queues and grant slots */
};

struct ncl_lock_desc {
    ncl_off_t data_off;
    uint64_t data_len;
    int32_t alg;  /* base algorithm (cfg.alg unless set with ncl_lock_set_alg) */
    int32_t pad;
};

/* Node-local per-lock state (ordinary coherent memory, design §2 table).
 * Fields other than `sub` are written only by the current holder, so the
 * state needed at release lives with the lock, not in the thread. */

struct ncl_local_lock {
    struct ncl_submutex sub;
    struct ncl_base base;
    int path;            /* global path taken when the lock arrived at this node */
    int holder;          /* participant index used for the global acquisition */
    int global_held;     /* LOCAL/HANDOFF: node owns the global lock (passed locally) */
    int handoffs;        /* consecutive local handoffs in the current run */
    ncl_line_t *owner;    /* HANDOFF: uncached owner word (0 = FREE, node+1) */
    ncl_line_t *announce; /* HANDOFF: announce[P], one line per node (scan region) */
    int pw_next;          /* HANDOFF, pick_policy: next announce[] slot to probe */
    int acq_contended;    /* HANDOFF: this global acquisition waited or got a handoff */
    uint64_t ann_seq;     /* HANDOFF, WAIT_OWN: last announce sequence number (word 0) */
    uint64_t ann_val;     /* HANDOFF_OPTS CLEAR: our announce value, still set while held */
    int ann_set;          /* HANDOFF_OPTS CLEAR: announce to clear at the global release */
    int need_grant;       /* HANDOFF_OPTS CLEAR: handoff seen via owner, grant not yet seen */
    uint64_t srv_grant;   /* SERVER in HANDOFF: grant word (seq + hint) for the release */
    struct ncl_data_ctx dc; /* protected data (data layer, design §2.1) */
    struct ncl_node_stats st;
} NCL_ALIGNED(NCL_CACHELINE);

int ncl_wb_overlap = 1;              /* see ncl_release */
static struct ncl_hdr *hdr;          /* in shared meta */
static struct ncl_sys_cfg cfg;       /* node-local copy */
static struct ncl_local_lock *local; /* node-local array [nlocks] */
static int my_node = -1;
static int is_server(void) { return cfg.alg == NCL_ALG_SERVER || ncl_alg_fastsrv_like(cfg.alg); }
/* Modes with local cohort passing (Phase 2a and 2b). */
static int cohort(void) { return cfg.mode == NCL_MODE_LOCAL || cfg.mode == NCL_MODE_HANDOFF; }

/* MODE_HANDOFF lines appended to each lock's base-algorithm blocks: meta
 * (owner, or the chain's succ[] ring, read one entry at a time) and scan
 * (announce[]). */
static size_t handoff_lines(const struct ncl_sys_cfg *c, int P)
{
    (void)P;
    if (c->mode != NCL_MODE_HANDOFF)
        return 0;
    if (c->alg == NCL_ALG_SERVER)
        return ncl_srv_chain_lines(c); /* succ[] ring */
    return 1; /* owner */
}
static size_t handoff_scan_lines(const struct ncl_sys_cfg *c, int P)
{
    if (c->mode != NCL_MODE_HANDOFF || c->alg == NCL_ALG_SERVER)
        return 0;
    return (size_t)P; /* announce[] */
}
#define OWNER_FREE 0ull
/* HANDOFF_OPTS GRANT: owner value while held (never equal to a node + 1). */
#define OWNER_HELD ((uint64_t)NCL_MAX_PARTICIPANTS + 1)

static size_t round_line(size_t n) { return (n + NCL_CACHELINE - 1) & ~(size_t)(NCL_CACHELINE - 1); }

const char *ncl_mode_name(ncl_mode_t m)
{
    switch (m) {
    case NCL_MODE_PHASE1: return "phase1";
    case NCL_MODE_LOCAL: return "local";
    case NCL_MODE_HANDOFF: return "handoff";
    }
    return "?";
}

static const struct { int bit; const char *name; } ho_names[] = {
    {NCL_HO_GRANT, "grant"}, {NCL_HO_CLEAR, "clear"}, {NCL_HO_SKIP, "skip"}};

int ncl_ho_opts_parse(const char *s, int *out)
{
    if (!strcmp(s, "none")) { *out = 0; return 0; }
    if (!strcmp(s, "all")) { *out = NCL_HO_ALL; return 0; }
    int v = 0;
    while (*s) {
        size_t n = strcspn(s, ",+");
        int k = 0, nk = (int)(sizeof ho_names / sizeof ho_names[0]);
        for (; k < nk; k++)
            if (strlen(ho_names[k].name) == n && !strncmp(s, ho_names[k].name, n))
                break;
        if (k == nk)
            return -1;
        v |= ho_names[k].bit;
        s += n;
        if (*s == ',' || *s == '+')
            s++;
    }
    *out = v;
    return v ? 0 : -1;
}

const char *ncl_ho_opts_name(int opts, char *buf)
{
    buf[0] = 0;
    for (size_t k = 0; k < sizeof ho_names / sizeof ho_names[0]; k++)
        if (opts & ho_names[k].bit) {
            if (buf[0])
                strcat(buf, "+");
            strcat(buf, ho_names[k].name);
        }
    return buf[0] ? buf : "none";
}

int ncl_mode_parse(const char *s, ncl_mode_t *out)
{
    if (!strcmp(s, "phase1")) *out = NCL_MODE_PHASE1;
    else if (!strcmp(s, "local")) *out = NCL_MODE_LOCAL;
    else if (!strcmp(s, "handoff")) *out = NCL_MODE_HANDOFF;
    else return -1;
    return 0;
}

static int sys_open(const struct ncl_sys_cfg *c, const char *path, int attach)
{
    struct ncl_sys_cfg resolved = *c;
    if (resolved.handoff_opts == NCL_HO_DEFAULT) {
        /* Default: every --ho option that applies (docs/handoff.md). */
        int o = 0;
        if (c->mode == NCL_MODE_HANDOFF && c->alg != NCL_ALG_SERVER) {
            o = NCL_HO_CLEAR;
            if (c->handoff_wait == NCL_WAIT_OWN)
                o |= NCL_HO_GRANT;
            if (c->announce_place == NCL_ANNOUNCE_BEFORE)
                o |= NCL_HO_SKIP;
        }
        resolved.handoff_opts = o;
    }
    c = &resolved;
    if ((c->mode == NCL_MODE_LOCAL || c->mode == NCL_MODE_HANDOFF) && c->no_submutex) {
        fprintf(stderr, "ncl: mode %s needs the submutex (no ablation)\n", ncl_mode_name(c->mode));
        return -1;
    }
    if (c->mode == NCL_MODE_HANDOFF && ncl_alg_fastsrv_like(c->alg)) {
        fprintf(stderr, "ncl: mode handoff applies to lamport, tournament, hybrid (gate + "
                        "owner) and server (holder-to-waiter chain); not fastsrv (use -M local)\n");
        return -1;
    }
    if (c->handoff_wait != NCL_WAIT_OWN && c->handoff_wait != NCL_WAIT_OWNER) {
        fprintf(stderr, "ncl: bad handoff_wait %d\n", c->handoff_wait);
        return -1;
    }
    if (c->handoff_opts & ~NCL_HO_ALL) {
        fprintf(stderr, "ncl: bad handoff_opts %#x\n", c->handoff_opts);
        return -1;
    }
    if (c->handoff_opts && (c->mode != NCL_MODE_HANDOFF || c->alg == NCL_ALG_SERVER)) {
        fprintf(stderr, "ncl: handoff options (grant, clear, skip) apply to -M handoff with "
                        "a gate algorithm\n");
        return -1;
    }
    if ((c->handoff_opts & NCL_HO_GRANT) && c->handoff_wait != NCL_WAIT_OWN) {
        fprintf(stderr, "ncl: handoff option grant needs --wait own (the grant is the handoff)\n");
        return -1;
    }
    if ((c->handoff_opts & NCL_HO_SKIP) && c->announce_place != NCL_ANNOUNCE_BEFORE) {
        fprintf(stderr, "ncl: handoff option skip needs -G before (announce, then read owner)\n");
        return -1;
    }
    if (c->chain_lines && (c->chain_lines < 2 || c->chain_lines > NCL_SRV_CHAIN_MAX ||
                           (c->chain_lines & (c->chain_lines - 1)))) {
        fprintf(stderr, "ncl: chain window must be a power of two in [2, %d]\n", NCL_SRV_CHAIN_MAX);
        return -1;
    }
    if (c->announce_place != NCL_ANNOUNCE_BEFORE && c->announce_place != NCL_ANNOUNCE_INSIDE) {
        fprintf(stderr, "ncl: bad announce_place %d\n", c->announce_place);
        return -1;
    }
    if (c->pick_policy != NCL_PICK_ADAPTIVE && c->pick_policy != NCL_PICK_SCAN) {
        fprintf(stderr, "ncl: bad pick_policy %d\n", c->pick_policy);
        return -1;
    }
    if (c->emu_word_wb && !(c->backend == NCL_BACKEND_EMU && c->meta_split)) {
        fprintf(stderr, "ncl: --emu-word-wb applies to -b emu -U split only\n");
        return -1;
    }
    if ((c->debug_skip_meta_wb || c->debug_skip_meta_inv) && !c->meta_split) {
        fprintf(stderr, "ncl: --skip-meta-wb/--skip-meta-inv apply to the split layout only\n");
        return -1;
    }
    if (ncl_fast_release == NCL_FASTREL_ALWAYS &&
        (c->alg == NCL_ALG_HYBRID || c->alg == NCL_ALG_FASTSRV)) {
        fprintf(stderr, "ncl: -F always (unconditional reopen) is unsafe for %s (splitter reset "
                        "race, docs/hybrid.md); it applies to ak only\n", ncl_alg_name(c->alg));
        return -1;
    }
    if (c->alg_per_lock && c->alg != NCL_ALG_LAMPORT && c->alg != NCL_ALG_TOURNAMENT) {
        fprintf(stderr, "ncl: per-lock algorithms: the default must be lamport or tournament\n");
        return -1;
    }
    if (c->nnodes < 1 || c->nnodes > NCL_MAX_NODES || c->nlocks < 1) {
        fprintf(stderr, "ncl: bad nnodes=%d / nlocks=%d\n", c->nnodes, c->nlocks);
        return -1;
    }
    if (c->alg == NCL_ALG_BAKERY && (ncl_bakery_arity < 0 || ncl_bakery_arity == 1)) {
        fprintf(stderr, "ncl: --bakery-arity must be 0 (flat) or >= 2\n");
        return -1;
    }
    if (c->alg == NCL_ALG_PTHREAD && c->backend != NCL_BACKEND_COHERENT) {
        /* A futex mutex needs coherent cross-node atomics: the reference point
         * "if coherent atomics existed", not a lock for non-coherent memory.
         * On uc it would fall back to x86 bus locks, which says nothing about
         * CXL. */
        fprintf(stderr, "ncl: pthread is a coherent-memory baseline: use -b coherent\n");
        return -1;
    }
    if (c->alg == NCL_ALG_PTHREAD && c->mode != NCL_MODE_PHASE1) {
        /* Cohort passing would unlock the mutex from a thread that did not lock
         * it (undefined for pthread mutexes); the gate + owner handoff needs no
         * base lock. */
        fprintf(stderr, "ncl: pthread runs in phase1 mode only\n");
        return -1;
    }
    int server = c->alg == NCL_ALG_SERVER || ncl_alg_fastsrv_like(c->alg);
    if (server && c->no_submutex) {
        fprintf(stderr, "ncl: %s needs the local submutex (no ablation)\n", ncl_alg_name(c->alg));
        return -1;
    }
    int P = c->no_submutex ? c->nnodes * c->threads_per_node : c->nnodes;
    if (P < 1 || P > NCL_MAX_PARTICIPANTS) {
        fprintf(stderr, "ncl: %d participants exceeds NCL_MAX_PARTICIPANTS=%d\n", P,
                NCL_MAX_PARTICIPANTS);
        return -1;
    }

    size_t base_lines = ncl_base_nlines(c->alg, P), base_scan = ncl_base_nscan(c->alg, P);
    if (c->alg_per_lock) { /* room for either algorithm */
        size_t a = ncl_base_nlines(NCL_ALG_LAMPORT, P), b = ncl_base_nlines(NCL_ALG_TOURNAMENT, P);
        size_t sa = ncl_base_nscan(NCL_ALG_LAMPORT, P), sb = ncl_base_nscan(NCL_ALG_TOURNAMENT, P);
        base_lines = a > b ? a : b;
        base_scan = sa > sb ? sa : sb;
    }
    if (c->pad_lines < 0 || c->pad_lines > 1024) {
        fprintf(stderr, "ncl: pad_lines must be in [0, 1024]\n");
        return -1;
    }
    size_t lines_per_lock = base_lines + handoff_lines(c, P) + (size_t)c->pad_lines;
    size_t scan_per_lock = base_scan + handoff_scan_lines(c, P);
    if (!c->raw_layout) {
        /* Stride rule: avoid per-lock strides that are multiples of 512 B (8 lines),
         * keeping even counts even. */
        if (lines_per_lock % 8 == 0)
            lines_per_lock += 2;
        if (scan_per_lock && scan_per_lock % 8 == 0)
            scan_per_lock += 2;
    }
    size_t hdr_sz = round_line(sizeof(struct ncl_hdr));
    size_t descs_sz = round_line(sizeof(struct ncl_lock_desc) * (size_t)c->nlocks);
    size_t locks_sz = lines_per_lock * NCL_CACHELINE * (size_t)c->nlocks;
    size_t user_sz = c->user_meta_lines * NCL_CACHELINE;
    size_t done_sz = (size_t)c->nnodes * NCL_CACHELINE;
    ncl_off_t srv_off = hdr_sz + descs_sz + locks_sz + user_sz + done_sz;
    struct ncl_srv_layout srv;
    memset(&srv, 0, sizeof srv);
    size_t srv_sz = 0;
    size_t slocks_sz = scan_per_lock * NCL_CACHELINE * (size_t)c->nlocks;
    size_t srv_scan_sz = 0;
    if (server && !(srv_sz = ncl_srv_layout(c, &srv, srv_off, slocks_sz, &srv_scan_sz)))
        return -1;

    struct ncl_mem_cfg mc = {
        .backend = c->backend,
        .meta_size = srv_off + srv_sz,
        .scan_size = slocks_sz + srv_scan_sz,
        .meta_split = c->meta_split,
        .data_size = c->data_size,
        .base_addr = c->base_addr,
        .dev_lat_ns = c->dev_lat_ns,
        .mem_node = c->mem_node,
        .path = path,
        .attach = attach,
    };
    ncl_cpus_init(); /* before threads pin (default = this process's affinity) */
    if (ncl_mem_init(&mc) < 0)
        return -1;

    if (attach) {
        /* Same command line, same layout: check it against the creator's. */
        ncl_mem_node_attach(); /* drop stale cached copies before reading the header */
        hdr = (struct ncl_hdr *)ncl_meta_ptr(0);
        if (hdr->magic != NCL_MAGIC) {
            fprintf(stderr, "ncl: %s: shared region not initialized\n", path);
            goto bad;
        }
        if (hdr->cfg.app_tag != c->app_tag || hdr->participants != P ||
            hdr->lines_per_lock != lines_per_lock || hdr->scan_per_lock != scan_per_lock ||
            hdr->cfg.nnodes != c->nnodes || hdr->cfg.nlocks != c->nlocks ||
            hdr->cfg.alg != c->alg || hdr->cfg.mode != c->mode ||
            hdr->cfg.backend != c->backend || hdr->srv.grant_off != srv.grant_off) {
            fprintf(stderr, "ncl: %s was created with a different configuration (command "
                            "line); every node must be started with the creator's\n", path);
            goto bad;
        }
        cfg = hdr->cfg;
        return 0;
    bad:
        hdr = NULL;
        ncl_mem_fini();
        return -1;
    }

    /* The region starts zeroed: all flags/turns/x/y = 0 = initial state. */
    hdr = (struct ncl_hdr *)ncl_meta_ptr(0);
    hdr->cfg = *c;
    hdr->participants = P;
    hdr->lines_per_lock = lines_per_lock;
    hdr->scan_per_lock = scan_per_lock;
    hdr->base_lines = base_lines;
    hdr->base_scan = base_scan;
    hdr->slocks_off = 0;
    hdr->descs_off = hdr_sz;
    hdr->locks_off = hdr_sz + descs_sz;
    hdr->user_off = hdr_sz + descs_sz + locks_sz;
    hdr->done_off = hdr->user_off + user_sz;
    hdr->srv = srv;
    {
        struct ncl_lock_desc *d = (struct ncl_lock_desc *)ncl_meta_ptr(hdr->descs_off);
        for (int l = 0; l < c->nlocks; l++)
            d[l].alg = c->alg;
    }
    for (int l = 0; l < c->nlocks; l++)
        if (ncl_base_init_shared(c->alg, (ncl_line_t *)ncl_meta_ptr(hdr->locks_off) +
                                             (size_t)l * lines_per_lock) < 0)
            return -1;
    ncl_fence();
    hdr->magic = NCL_MAGIC;
    ncl_fence();
    cfg = *c;
    return 0;
}

int ncl_sys_create(const struct ncl_sys_cfg *c, const char *path) { return sys_open(c, path, 0); }
int ncl_sys_attach(const struct ncl_sys_cfg *c, const char *path) { return sys_open(c, path, 1); }
void ncl_sys_publish(void) { ncl_mem_publish(); }

void ncl_sys_destroy(void)
{
    if (local) {
        free(local);
        local = NULL;
    }
    hdr = NULL;
    ncl_mem_fini();
}

int ncl_lock_set_data(int lock, ncl_off_t off, size_t len)
{
    if (lock < 0 || lock >= cfg.nlocks)
        return -1;
    if (off % NCL_CACHELINE) {
        /* A writeback covers whole lines; unaligned ranges would let two
         * locks clobber each other's data (design §2 rule 2). */
        fprintf(stderr, "ncl: lock %d data offset %#lx not line-aligned\n", lock,
                (unsigned long)off);
        return -1;
    }
    if (off + len > ncl_mem.data_size)
        return -1;
    struct ncl_lock_desc *d = (struct ncl_lock_desc *)ncl_meta_ptr(hdr->descs_off);
    d[lock].data_off = off;
    d[lock].data_len = len;
    ncl_fence();
    return 0;
}

int ncl_lock_set_alg(int lock, ncl_alg_t alg)
{
    if (!hdr || lock < 0 || lock >= cfg.nlocks || !cfg.alg_per_lock ||
        (alg != NCL_ALG_LAMPORT && alg != NCL_ALG_TOURNAMENT)) {
        fprintf(stderr, "ncl: ncl_lock_set_alg(%d, %s): needs alg_per_lock and lamport or "
                        "tournament\n", lock, ncl_alg_name(alg));
        return -1;
    }
    /* Both start all-zero, so no ncl_base_init_shared. */
    struct ncl_lock_desc *d = (struct ncl_lock_desc *)ncl_meta_ptr(hdr->descs_off);
    d[lock].alg = (int32_t)alg;
    ncl_fence();
    return 0;
}

int ncl_lock_set_reactive_mode(int lock, int mode)
{
    if (!hdr || lock < 0 || lock >= cfg.nlocks || cfg.alg != NCL_ALG_REACTIVE) {
        fprintf(stderr, "ncl: ncl_lock_set_reactive_mode: needs -a reactive\n");
        return -1;
    }
    ncl_line_t *mb = (ncl_line_t *)ncl_meta_ptr(hdr->locks_off) + (size_t)lock * hdr->lines_per_lock;
    ncl_base_reactive_set_mode(mb, hdr->participants, mode);
    return 0;
}

ncl_alg_t ncl_lock_alg(int lock)
{
    const struct ncl_lock_desc *d = (const struct ncl_lock_desc *)ncl_meta_ptr(hdr->descs_off);
    return (ncl_alg_t)d[lock].alg;
}

int ncl_node_attach(int node_id)
{
    hdr = (struct ncl_hdr *)ncl_meta_ptr(0);
    if (hdr->magic != NCL_MAGIC) {
        fprintf(stderr, "ncl: region not initialized\n");
        return -1;
    }
    cfg = hdr->cfg;
    if (node_id < 0 || node_id >= cfg.nnodes)
        return -1;
    my_node = node_id;
    ncl_mem.debug_skip_meta_wb = cfg.debug_skip_meta_wb;
    ncl_mem.debug_skip_meta_inv = cfg.debug_skip_meta_inv;
    ncl_mem.emu_word_wb = cfg.emu_word_wb;
    {
        struct ncl_data_cfg dc = {.precise = cfg.precise_data,
                                  .skip_wb = cfg.debug_skip_data_wb,
                                  .skip_inv = cfg.debug_skip_data_inv,
                                  .debug_precise = cfg.debug_precise};
        ncl_data_configure(&dc);
    }
    ncl_mem_node_attach();

    local = aligned_alloc(NCL_CACHELINE, sizeof(*local) * (size_t)cfg.nlocks);
    if (!local)
        return -1;
    memset(local, 0, sizeof(*local) * (size_t)cfg.nlocks);

    const struct ncl_lock_desc *d = (const struct ncl_lock_desc *)ncl_meta_ptr(hdr->descs_off);
    ncl_line_t *blocks = (ncl_line_t *)ncl_meta_ptr(hdr->locks_off);
    ncl_line_t *sblocks = (ncl_line_t *)ncl_scan_ptr(hdr->slocks_off);
    for (int l = 0; l < cfg.nlocks; l++) {
        ncl_line_t *mb = blocks + (size_t)l * hdr->lines_per_lock;
        ncl_line_t *sb = sblocks + (size_t)l * hdr->scan_per_lock;
        ncl_sub_init(&local[l].sub);
        ncl_base_bind(&local[l].base, (ncl_alg_t)d[l].alg, hdr->participants, mb, sb);
        if (cfg.mode == NCL_MODE_HANDOFF && !is_server()) {
            local[l].owner = mb + hdr->base_lines;
            local[l].announce = sb + hdr->base_scan;
            local[l].pw_next = (node_id + 1) % hdr->participants;
        }
        ncl_data_ctx_init(&local[l].dc, d[l].data_len ? ncl_data_ptr(d[l].data_off) : NULL,
                          d[l].data_len);
    }
    if (is_server() && ncl_srv_node_start(&cfg, &hdr->srv, node_id, blocks, hdr->lines_per_lock,
                                          sblocks, hdr->scan_per_lock) < 0)
        return -1;
    return 0;
}

static ncl_line_t *done_line(int n) { return (ncl_line_t *)ncl_meta_ptr(hdr->done_off) + n; }

void ncl_node_detach(void)
{
    ncl_uc_store(done_line(my_node), 1);
    ncl_fence();
    for (int n = 0; n < cfg.nnodes; n++)
        while (!ncl_uc_load(done_line(n)))
            ncl_cpu_relax();
    if (is_server())
        ncl_srv_node_stop();
}

void ncl_thread_init(struct ncl_thread *t, int local_idx)
{
    memset(t, 0, sizeof *t);
    t->node = my_node;
    t->local_idx = local_idx;
    t->participant = my_node * cfg.threads_per_node + local_idx;
}

static void check_owner(uint64_t o)
{
    if (o > (uint64_t)hdr->participants && o != OWNER_HELD) {
        fprintf(stderr, "ncl: corrupt owner word %#llx\n", (unsigned long long)o);
        abort();
    }
}

/* Design §3.3 acquire, caller holds the submutex. Returns the NCL_PATH_*:
 * the gate's base path if the owner was FREE, else NCL_PATH_GLOBAL_HANDOFF. */
uint64_t ncl_debug_grant_delay = 0;

static int handoff_acquire(struct ncl_local_lock *L, int me, struct ncl_base_stats *bst)
{
    struct ncl_handoff_stats *h = &L->st.ho;
    (void)h; /* only NCL_STAT uses it */
    const uint64_t mine = (uint64_t)me + 1;
    ncl_line_t *ann = &L->announce[me];
    int path;

    /* WAIT_OWN: the announce word is a fresh nonzero sequence number, which
     * the releaser copies into our grant word (word 1 of this line). */
    const int own = cfg.handoff_wait == NCL_WAIT_OWN;
    const uint64_t aval = own ? ++L->ann_seq : 1;
    int need_grant = 0; /* handoff seen via owner: grant store may still be coming */
    /* GRANT: owner is only FREE/HELD; a handoff is our grant word alone, so
     * "owner == me" never means a handoff (and never occurs). */
    const int grant = cfg.handoff_opts & NCL_HO_GRANT;
    const uint64_t taken = grant ? OWNER_HELD : mine;

    if (cfg.check_announce && ncl_uc_load(ann) != 0) {
        /* We cleared it and have not announced since: a writeback of a stale
         * copy of our line (another node's grant, design §2) landed after our
         * clear. Not wrapped in NCL_STAT: the stress test fails on it. */
        h->stale_announce++;
    }
    const int place = cfg.announce_place;
    int announced = 0, waited = 0, skip = 0;
    uint64_t o;
    if (place == NCL_ANNOUNCE_BEFORE) {
        ncl_uc_store(ann, aval);
        ncl_fence(); /* announce visible before the gate's owner read */
        announced = 1;
        if ((cfg.handoff_opts & NCL_HO_SKIP) && L->acq_contended) {
            /* SKIP, only if our last global acquisition of this lock waited
             * (else the lock is likely FREE and the early read is one more
             * round trip before the gate). The gate serializes only
             * FREE -> taken, and releasers never take it, so a "held" owner
             * read here is as good as one inside the gate: the holder either
             * sees our announce (fenced above) or stores FREE after this
             * read, and our owner re-reads then send us through the gate. A
             * handoff to us (no GRANT) is ours already, as in the gate. */
            o = ncl_uc_load(L->owner);
            check_owner(o);
            if (!grant && o == mine) {
                NCL_STAT(h->wait_handoffs++);
                need_grant = own;
                L->acq_contended = 1;
                path = NCL_PATH_GLOBAL_HANDOFF;
                goto acquired;
            }
            skip = o != OWNER_FREE;
        }
    }
    for (;;) {
        if (skip) {
            skip = 0;
            NCL_STAT(h->gate_skips++);
        } else {
            int gpath = ncl_base_lock(&L->base, me, bst);
            NCL_STAT(h->gate_entries++);
            if (place == NCL_ANNOUNCE_INSIDE && !announced) {
                ncl_uc_store(ann, aval);
                announced = 1;
            }
            /* Announce (inside variant) before the owner read: a releaser that
             * misses it must store its FREE/handoff after our read of `owner`
             * returned "held", so our poll later sees FREE and we retry. */
            ncl_fence();
            o = ncl_uc_load(L->owner);
            check_owner(o);
            if (o == OWNER_FREE || (!grant && o == mine)) {
                /* Only gate users write a FREE owner, one at a time; a handoff
                 * to us is already ours. Single 8-byte store either way. */
                ncl_uc_store(L->owner, taken);
                ncl_fence(); /* owner store before the gate-exit stores */
                ncl_base_unlock(&L->base, me, gpath, bst);
                if (o == mine) {
                    NCL_STAT(h->gate_handoffs++);
                    need_grant = own;
                    L->acq_contended = 1;
                    path = NCL_PATH_GLOBAL_HANDOFF;
                } else {
                    NCL_STAT(h->gate_takes++);
                    L->acq_contended = waited;
                    path = gpath;
                }
                break;
            }
            ncl_base_unlock(&L->base, me, gpath, bst);
        }

        waited = 1;
        /* Wait: poll the owner field directly (no separate handoff flag).
         * Capped backoff paces the uncached polls. This wait depends on
         * another node's releaser and, after a handoff, on THIS thread
         * running: a handoff commits the lock to us, so if we spin while the
         * CPU is oversubscribed we can starve the very thread (ours or the
         * releaser's) that has to make progress. Hence spin-then-yield, like
         * the submutex and server waits (measured: 4 concurrent stress runs,
         * 48 threads on 20 CPUs, 14 s without the yield vs 0.14 s Phase 1). */
        struct ncl_backoff bo;
        ncl_backoff_init(&bo);
        unsigned spun = 0;
        uint64_t polls = 0, recheck = NCL_OWNER_RECHECK;
        for (;;) {
            /* WAIT_OWN: poll our own line. Re-read the shared owner word (to
             * notice FREE after a missed announce) at poll NCL_OWNER_RECHECK,
             * then at twice as many each time: O(log wait) owner loads per
             * wait, and a miss is noticed within twice the time already
             * waited. A fixed interval rebuilt the hot spot whenever waits
             * were long (Lamport at 32 nodes, docs/cxl_results.md). */
            if (own) {
                NCL_STAT(h->grant_polls++);
                if (ncl_uc_load_w(ann, 1) == aval) {
                    NCL_STAT(h->wait_handoffs++);
                    L->acq_contended = 1;
                    path = NCL_PATH_GLOBAL_HANDOFF;
                    goto acquired;
                }
            }
            if (!own || ++polls == recheck) {
                recheck *= 2;
                o = ncl_uc_load(L->owner);
                NCL_STAT(h->owner_polls++);
                check_owner(o);
                if (!grant && o == mine) {
                    NCL_STAT(h->wait_handoffs++);
                    L->acq_contended = 1;
                    path = NCL_PATH_GLOBAL_HANDOFF;
                    need_grant = own;
                    goto acquired;
                }
                if (o == OWNER_FREE) {
                    /* Releaser missed us: retry the gate. GRANT: a releaser
                     * either grants us or stores FREE, never both, so no
                     * grant can be on its way. */
                    NCL_STAT(h->missed_announce++);
                    break;
                }
            }
            ncl_backoff(&bo);
            if (spun < NCL_SPIN_BEFORE_YIELD)
                spun += bo.cur;
            else
                sched_yield();
        }
    }
acquired:
    if (cfg.handoff_opts & NCL_HO_CLEAR) {
        /* Keep the announce (and any wait for a grant still on its way) until
         * the global release: only the holder scans, and it skips itself. */
        L->ann_set = announced;
        L->ann_val = aval;
        L->need_grant = need_grant;
        ncl_fence(); /* ownership observed before the CS */
    } else {
        if (need_grant && !cfg.debug_no_grant_wait) {
            /* The releaser that set owner = us stores our grant next (owner,
             * fence, grant). Wait for it before clearing the announce: the two
             * share a line, and on a cached scan region a whole-line writeback
             * of its grant must not land after our clear (it would resurrect
             * the announce). Short: the grant store follows the owner store. */
            unsigned n = 0;
            NCL_STAT(h->grant_waits++);
            while (ncl_uc_load_w(ann, 1) != aval)
                ncl_spin_wait(&n);
        }
        ncl_fence(); /* ownership observed before the CS and the clear below */
        if (announced)
            ncl_uc_store(ann, 0);
    }
    return path;
}

static void handoff_release(struct ncl_local_lock *L, int me)
{
    struct ncl_handoff_stats *h = &L->st.ho;
    const int P = hdr->participants;
    const int grant = cfg.handoff_opts & NCL_HO_GRANT;
    int w = -1;
    uint64_t seq = 0; /* the picked waiter's announce word (WAIT_OWN: its sequence) */

    if (L->ann_set) {
        /* CLEAR: our announce stayed set while we held the lock. Clear it
         * now; the fence before the handoff store makes it visible before
         * anyone else can hold the lock and scan. */
        ncl_line_t *ann = &L->announce[me];
        if (L->need_grant && !cfg.debug_no_grant_wait) {
            /* See handoff_acquire: a grant still on its way must land before
             * our clear. Long since done by now, in practice. */
            unsigned n = 0;
            NCL_STAT(h->grant_waits++);
            while (ncl_uc_load_w(ann, 1) != L->ann_val)
                ncl_spin_wait(&n);
            L->need_grant = 0;
        }
        ncl_uc_store(ann, 0);
        L->ann_set = 0;
    }

    int probe = cfg.pick_policy == NCL_PICK_ADAPTIVE && !L->acq_contended;
    if (probe && P > 1) {
        /* Uncontended acquisition: one probe at this node's round-robin
         * pointer. Hit: hand off and continue after that node next time.
         * Miss: FREE (a waiter we did not probe recovers through the gate)
         * and advance by one. */
        int j = L->pw_next;
        NCL_STAT(h->announce_loads++);
        if ((seq = ncl_uc_load(&L->announce[j])))
            w = j;
        int nx = (j + 1) % P;
        if (nx == me)
            nx = (nx + 1) % P;
        L->pw_next = nx;
    } else {
        w = ncl_uc_scan_first_v(L->announce, P, me + 1, P - 1, me, &h->announce_loads, &seq); /* round-robin after me */
    }
    ncl_fence(); /* CS effects, data writeback, our clear and the scan before the handoff */
    if (w >= 0 && grant) {
        /* GRANT: owner stays HELD; the grant store alone passes the lock (and,
         * through the fence above, the data). Our copy of w's line is
         * current: we read it after an invalidate (or in the snapshot), and
         * w writes it only after seeing this grant. */
        ncl_uc_store_w(&L->announce[w], 1, seq);
        NCL_STAT(h->handoffs_sent++);
    } else if (w >= 0) {
        ncl_uc_store(L->owner, (uint64_t)w + 1); /* handoff = one 8-byte store */
        NCL_STAT(h->handoffs_sent++);
        if (cfg.handoff_wait == NCL_WAIT_OWN) {
            /* Grant after owner: w may run and release (storing owner) as soon
             * as it sees the grant, so our owner store must be visible first.
             * Uncached grant: x86 keeps UC stores in order, the store-store
             * fence suffices. Cached grant (split scan region, cached-flush):
             * its writeback is not ordered after the UC store without a full
             * fence. Our copy of w's line is current: we just read it after
             * an invalidate, and w writes it only after seeing this grant. */
            if (NCL_UNLIKELY(ncl_debug_grant_delay))
                ncl_lat_spin_cycles(ncl_debug_grant_delay);
            if (ncl_meta_cached(&L->announce[w]))
                ncl_fence();
            else
                ncl_fence_ss();
            ncl_uc_store_w(&L->announce[w], 1, seq);
        }
    } else {
        ncl_uc_store(L->owner, OWNER_FREE);
        NCL_STAT(h->frees++);
    }
    ncl_fence_ss(); /* handoff store before the submutex unlock */
}

static void acquire(struct ncl_thread *t, int lock, const struct ncl_range *r, int n);

void ncl_acquire(struct ncl_thread *t, int lock)
{
    if (cfg.precise_data) {
        fprintf(stderr, "ncl: precise_data: acquire with ncl_acquire_ranges\n");
        abort();
    }
    acquire(t, lock, NULL, 0);
}

void ncl_acquire_ranges(struct ncl_thread *t, int lock, const struct ncl_range *r, int n)
{
    if (!cfg.precise_data) {
        fprintf(stderr, "ncl: ncl_acquire_ranges needs cfg.precise_data\n");
        abort();
    }
    acquire(t, lock, r, n);
}

static void acquire(struct ncl_thread *t, int lock, const struct ncl_range *r, int n)
{
    struct ncl_local_lock *L = &local[lock];
    int part;
    if (cfg.no_submutex) {
        part = t->participant;
    } else {
        ncl_sub_lock(&L->sub, &t->q);
        part = my_node;
    }

    /* Only meaningful under the submutex: in the ablation a thread gets here
     * holding nothing and could see another thread's global_held. */
    if (cohort() && L->global_held) {
        /* Local handoff: the previous local holder passed us the submutex
         * with the node still owning the global lock. Data is coherent
         * within the node, so no invalidate (PRECISE: except ranges this
         * run has not fetched yet). */
        ncl_fence();
        L->st.acquires++;
        NCL_STAT(L->st.path[NCL_PATH_LOCAL_HANDOFF]++);
        ncl_data_acquired(&L->dc, 0, r, n);
        return;
    }

    struct ncl_base_stats tmp = {0};
    int path;
    if (cfg.alg == NCL_ALG_SERVER) {
        uint64_t g = ncl_srv_acquire(t, lock);
        path = NCL_PATH_SERVER;
        if (cfg.mode == NCL_MODE_HANDOFF) {
            L->srv_grant = g;
            if (ncl_srv_grant_passed(g)) {
                path = NCL_PATH_GLOBAL_HANDOFF;
                NCL_STAT(L->st.ho.wait_handoffs++);
            }
        }
    } else if (ncl_alg_fastsrv_like(cfg.alg)) {
        path = ncl_fastsrv_acquire(t, lock, &L->base, &tmp);
    } else if (cfg.mode == NCL_MODE_HANDOFF) {
        path = handoff_acquire(L, part, &tmp);
    } else {
        path = ncl_base_lock(&L->base, part, &tmp);
    }

    /* Global lock held: holder-only fields below (they stay with the lock,
     * not the thread, so any local thread can do the global release). */
    L->path = path;
    L->holder = part;
    L->global_held = 1;
    L->handoffs = 0;
    L->st.acquires++;
    NCL_STAT(L->st.path[path]++);
    NCL_STAT(ncl_base_stats_add(&L->st.base, &tmp));

    /* Global lock arrived at this node: the data layer drops stale data. */
    ncl_data_acquired(&L->dc, 1, r, n);
}

void ncl_release(int lock, const struct ncl_range *hints, int nhints)
{
    struct ncl_local_lock *L = &local[lock];

    /* Departing = the lock leaves the node; else a cohort pass to a local
     * waiter (under the cap) keeps global ownership here. */
    int departing = 1;
    if (cohort()) {
        int cap = cfg.local_cap > 0 ? cfg.local_cap : 64;
        if (ncl_sub_has_waiter(&L->sub)) {
            if (L->handoffs < cap)
                departing = 0;
            else
                NCL_STAT(L->st.cap_releases++);
        }
    }

    /* Data layer (design §2.1): remember the dirty ranges; on departure write
     * them back eagerly (design §2 rule 5). Overlap (ncl_wb_overlap,
     * MODE_HANDOFF only): issue the flushes without the trailing fence, so the
     * release path's leading loads (2b pick_waiter, the chain's succ[] load)
     * run while they complete. Both paths ncl_fence() before the store that
     * hands the lock (and so the data) to another node: handoff_release
     * before the owner store, ncl_srv_chain_release before the grant store,
     * send() before REL. */
    ncl_data_release(&L->dc, hints, nhints, departing,
                     ncl_wb_overlap && cfg.mode == NCL_MODE_HANDOFF);

    if (!departing) {
        /* Cohort pass: keep global ownership in the node. Fence so this
         * thread's uncached stores reach the device first. */
        L->handoffs++;
        ncl_fence();
        ncl_sub_unlock(&L->sub);
        return;
    }

    L->global_held = 0;
    L->st.global_releases++;
    if (cfg.alg == NCL_ALG_SERVER && cfg.mode == NCL_MODE_HANDOFF)
        ncl_srv_chain_release(lock, L->srv_grant, cfg.pick_policy == NCL_PICK_SCAN, &L->st.ho);
    else if (cfg.alg == NCL_ALG_SERVER)
        ncl_srv_release(lock);
    else if (ncl_alg_fastsrv_like(cfg.alg))
        ncl_fastsrv_release(lock, &L->base, L->path, &L->st.base);
    else if (cfg.mode == NCL_MODE_HANDOFF)
        handoff_release(L, L->holder);
    else
        ncl_base_unlock(&L->base, L->holder, L->path, &L->st.base);
    if (!cfg.no_submutex)
        ncl_sub_unlock(&L->sub);
}

const struct ncl_sys_cfg *ncl_sys_cfg(void) { return &cfg; }
int ncl_chain_window(void)
{
    return cfg.alg == NCL_ALG_SERVER && cfg.mode == NCL_MODE_HANDOFF ? (int)ncl_srv_chain_lines(&cfg) : 0;
}
int ncl_participants(void) { return hdr ? hdr->participants : 0; }
size_t ncl_lines_per_lock(void) { return hdr ? (size_t)hdr->lines_per_lock : 0; }

ncl_line_t *ncl_user_line(size_t i)
{
    if (i >= cfg.user_meta_lines)
        abort();
    return (ncl_line_t *)ncl_meta_ptr(hdr->user_off) + i;
}

void ncl_node_stats(struct ncl_node_stats *out)
{
    memset(out, 0, sizeof *out);
    for (int l = 0; l < cfg.nlocks; l++) {
        const struct ncl_node_stats *s = &local[l].st;
        out->acquires += s->acquires;
        out->global_releases += s->global_releases;
        out->cap_releases += s->cap_releases;
        for (int p = 0; p < NCL_PATH__COUNT; p++)
            out->path[p] += s->path[p];
        ncl_base_stats_add(&out->base, &s->base);
        for (size_t k = 0; k < NCL_HANDOFF_STATS_N; k++)
            ((uint64_t *)&out->ho)[k] += ((const uint64_t *)&s->ho)[k];
    }
    if (is_server())
        ncl_srv_stats(&out->srv);
}
