#include "ncl/lock.h"
#include "server.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCL_MAGIC 0x4e434c4f434b5331ull /* "NCLOCKS1" */

/* Static configuration, at meta offset 0. Read-mostly: written by the parent
 * before any node starts, never modified afterwards. */
struct ncl_hdr {
    uint64_t magic;
    struct ncl_sys_cfg cfg;
    int32_t participants;
    uint64_t lines_per_lock;
    ncl_off_t locks_off;  /* meta offset of first lock block */
    ncl_off_t user_off;   /* meta offset of user lines */
    ncl_off_t descs_off;  /* meta offset of struct ncl_lock_desc[nlocks] */
    ncl_off_t done_off;   /* meta offset of per-node "done" lines (detach barrier) */
    struct ncl_srv_layout srv; /* SERVER/FASTSRV queues and grant slots */
};

struct ncl_lock_desc {
    ncl_off_t data_off;
    uint64_t data_len;
};

/* Node-local per-lock state (ordinary coherent memory, design §2 table).
 * Fields other than `sub` are written only by the current holder, so the
 * state needed at release lives with the lock, not in the thread. */
#define NCL_DIRTY_MAX 16

struct ncl_local_lock {
    struct ncl_submutex sub;
    struct ncl_base base;
    int path;            /* global path taken when the lock arrived at this node */
    int holder;          /* participant index used for the global acquisition */
    int global_held;     /* LOCAL/HANDOFF: node owns the global lock (passed locally) */
    int handoffs;        /* consecutive local handoffs in the current run */
    void *data;
    size_t data_len;
    ncl_line_t *owner;    /* HANDOFF: uncached owner word (0 = FREE, node+1) */
    ncl_line_t *announce; /* HANDOFF: announce[P], one line per node */
    int pw_next;          /* HANDOFF, pick_policy: next announce[] slot to probe */
    int acq_contended;    /* HANDOFF: this global acquisition waited or got a handoff */
    uint64_t srv_grant;   /* SERVER in HANDOFF: grant word (seq + hint) for the release */
    /* LOCAL/HANDOFF precise hints accumulated over a local run */
    int ndirty, dirty_full;
    struct ncl_range dirty[NCL_DIRTY_MAX];
    struct ncl_node_stats st;
} NCL_ALIGNED(NCL_CACHELINE);

static struct ncl_hdr *hdr;          /* in shared meta */
static struct ncl_sys_cfg cfg;       /* node-local copy */
static struct ncl_local_lock *local; /* node-local array [nlocks] */
static int my_node = -1;
static int is_server(void) { return cfg.alg == NCL_ALG_SERVER || cfg.alg == NCL_ALG_FASTSRV; }
/* Modes with local cohort passing (Phase 2a and 2b). */
static int cohort(void) { return cfg.mode == NCL_MODE_LOCAL || cfg.mode == NCL_MODE_HANDOFF; }

/* MODE_HANDOFF lines appended to each lock's base-algorithm block. */
static size_t handoff_lines(const struct ncl_sys_cfg *c, int P)
{
    if (c->mode != NCL_MODE_HANDOFF)
        return 0;
    if (c->alg == NCL_ALG_SERVER)
        return ncl_srv_chain_lines(c); /* succ[] ring */
    return 1 + (size_t)P;
}
#define OWNER_FREE 0ull

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

int ncl_mode_parse(const char *s, ncl_mode_t *out)
{
    if (!strcmp(s, "phase1")) *out = NCL_MODE_PHASE1;
    else if (!strcmp(s, "local")) *out = NCL_MODE_LOCAL;
    else if (!strcmp(s, "handoff")) *out = NCL_MODE_HANDOFF;
    else return -1;
    return 0;
}

int ncl_sys_create(const struct ncl_sys_cfg *c)
{
    if ((c->mode == NCL_MODE_LOCAL || c->mode == NCL_MODE_HANDOFF) && c->no_submutex) {
        fprintf(stderr, "ncl: mode %s needs the submutex (no ablation)\n", ncl_mode_name(c->mode));
        return -1;
    }
    if (c->mode == NCL_MODE_HANDOFF && c->alg == NCL_ALG_FASTSRV) {
        fprintf(stderr, "ncl: mode handoff applies to lamport, tournament, hybrid (gate + "
                        "owner) and server (holder-to-waiter chain); not fastsrv (use -M local)\n");
        return -1;
    }
    if (c->chain_lines && (c->chain_lines < 2 || c->chain_lines > NCL_SRV_CHAIN_MAX ||
                           (c->chain_lines & (c->chain_lines - 1)))) {
        fprintf(stderr, "ncl: chain window must be a power of two in [2, %d]\n", NCL_SRV_CHAIN_MAX);
        return -1;
    }
    if (c->mode == NCL_MODE_HANDOFF && c->alg == NCL_ALG_SERVER && c->pick_policy == NCL_PICK_PROBE) {
        fprintf(stderr, "ncl: server handoff: pick policy is adaptive (follow the hint) or scan "
                        "(always read succ[])\n");
        return -1;
    }
    if (c->nnodes < 1 || c->nnodes > NCL_MAX_NODES || c->nlocks < 1) {
        fprintf(stderr, "ncl: bad nnodes=%d / nlocks=%d\n", c->nnodes, c->nlocks);
        return -1;
    }
    int server = c->alg == NCL_ALG_SERVER || c->alg == NCL_ALG_FASTSRV;
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

    size_t lines_per_lock = ncl_base_nlines(c->alg, P) + handoff_lines(c, P);
    size_t hdr_sz = round_line(sizeof(struct ncl_hdr));
    size_t descs_sz = round_line(sizeof(struct ncl_lock_desc) * (size_t)c->nlocks);
    size_t locks_sz = lines_per_lock * NCL_CACHELINE * (size_t)c->nlocks;
    size_t user_sz = c->user_meta_lines * NCL_CACHELINE;
    size_t done_sz = (size_t)c->nnodes * NCL_CACHELINE;
    ncl_off_t srv_off = hdr_sz + descs_sz + locks_sz + user_sz + done_sz;
    struct ncl_srv_layout srv;
    memset(&srv, 0, sizeof srv);
    size_t srv_sz = 0;
    if (server && !(srv_sz = ncl_srv_layout(c, &srv, srv_off)))
        return -1;

    struct ncl_mem_cfg mc = {
        .backend = c->backend,
        .meta_size = srv_off + srv_sz,
        .data_size = c->data_size,
        .base_addr = c->base_addr,
        .dev_lat_ns = c->dev_lat_ns,
    };
    if (ncl_mem_init(&mc) < 0)
        return -1;

    /* memfd starts zeroed: all flags/turns/x/y = 0 = initial state. */
    hdr = (struct ncl_hdr *)ncl_meta_ptr(0);
    hdr->cfg = *c;
    hdr->participants = P;
    hdr->lines_per_lock = lines_per_lock;
    hdr->descs_off = hdr_sz;
    hdr->locks_off = hdr_sz + descs_sz;
    hdr->user_off = hdr_sz + descs_sz + locks_sz;
    hdr->done_off = hdr->user_off + user_sz;
    hdr->srv = srv;
    ncl_fence();
    hdr->magic = NCL_MAGIC;
    ncl_fence();
    cfg = *c;
    return 0;
}

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
    ncl_mem_node_attach();

    local = aligned_alloc(NCL_CACHELINE, sizeof(*local) * (size_t)cfg.nlocks);
    if (!local)
        return -1;
    memset(local, 0, sizeof(*local) * (size_t)cfg.nlocks);

    const struct ncl_lock_desc *d = (const struct ncl_lock_desc *)ncl_meta_ptr(hdr->descs_off);
    ncl_line_t *blocks = (ncl_line_t *)ncl_meta_ptr(hdr->locks_off);
    for (int l = 0; l < cfg.nlocks; l++) {
        ncl_sub_init(&local[l].sub);
        ncl_base_bind(&local[l].base, cfg.alg, hdr->participants,
                      blocks + (size_t)l * hdr->lines_per_lock);
        if (cfg.mode == NCL_MODE_HANDOFF && !is_server()) {
            ncl_line_t *ho = blocks + (size_t)l * hdr->lines_per_lock +
                             ncl_base_nlines(cfg.alg, hdr->participants);
            local[l].owner = ho;
            local[l].announce = ho + 1;
            local[l].pw_next = (node_id + 1) % hdr->participants;
        }
        local[l].data = d[l].data_len ? ncl_data_ptr(d[l].data_off) : NULL;
        local[l].data_len = d[l].data_len;
    }
    if (is_server() && ncl_srv_node_start(&cfg, &hdr->srv, node_id, blocks, hdr->lines_per_lock) < 0)
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
    if (o > (uint64_t)hdr->participants) {
        fprintf(stderr, "ncl: corrupt owner word %#llx\n", (unsigned long long)o);
        abort();
    }
}

/* Design §3.3 acquire, caller holds the submutex. Returns the NCL_PATH_*:
 * the gate's base path if the owner was FREE, else NCL_PATH_GLOBAL_HANDOFF. */
static int handoff_acquire(struct ncl_local_lock *L, int me, struct ncl_base_stats *bst)
{
    struct ncl_handoff_stats *h = &L->st.ho;
    const uint64_t mine = (uint64_t)me + 1;
    ncl_line_t *ann = &L->announce[me];
    int path;

    const int place = cfg.announce_place;
    int announced = 0, waited = 0;
    if (place == NCL_ANNOUNCE_BEFORE) {
        ncl_uc_store(ann, 1);
        ncl_fence(); /* announce visible before the gate's owner read */
        announced = 1;
    }
    for (;;) {
        int gpath = ncl_base_lock(&L->base, me, bst);
        h->gate_entries++;
        if (place == NCL_ANNOUNCE_INSIDE && !announced) {
            ncl_uc_store(ann, 1);
            announced = 1;
        }
        /* Announce (inside variant) before the owner read: a releaser that
         * misses it must store its FREE/handoff after our read of `owner`
         * returned "held", so our poll later sees FREE and we retry. */
        ncl_fence();
        uint64_t o = ncl_uc_load(L->owner);
        check_owner(o);
        if (o == OWNER_FREE || o == mine) {
            /* Only gate users write a FREE owner, one at a time; a handoff
             * to us is already ours. Single 8-byte store either way. */
            ncl_uc_store(L->owner, mine);
            ncl_fence(); /* owner store before the gate-exit stores */
            ncl_base_unlock(&L->base, me, gpath, bst);
            if (o == mine) {
                h->gate_handoffs++;
                L->acq_contended = 1;
                path = NCL_PATH_GLOBAL_HANDOFF;
            } else {
                h->gate_takes++;
                L->acq_contended = waited;
                path = gpath;
            }
            break;
        }
        if (place == NCL_ANNOUNCE_LAZY && !announced) {
            /* Lazy: announce only now that the owner is known to be held.
             * Same argument as above: if the holder's pick_waiter misses
             * this store, its FREE lands after our read of "held", and our
             * poll sees FREE and retries the gate. */
            ncl_uc_store(ann, 1);
            ncl_fence();
            announced = 1;
        }
        ncl_base_unlock(&L->base, me, gpath, bst);

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
        for (;;) {
            o = ncl_uc_load(L->owner);
            h->owner_polls++;
            check_owner(o);
            if (o == mine) {
                h->wait_handoffs++;
                L->acq_contended = 1;
                path = NCL_PATH_GLOBAL_HANDOFF;
                goto acquired;
            }
            if (o == OWNER_FREE) {
                h->missed_announce++; /* releaser missed us: retry the gate */
                break;
            }
            ncl_backoff(&bo);
            if (spun < NCL_SPIN_BEFORE_YIELD)
                spun += bo.cur;
            else
                sched_yield();
        }
    }
acquired:
    ncl_fence(); /* ownership observed before the CS and the clear below */
    if (announced)
        ncl_uc_store(ann, 0);
    return path;
}

/* Design §3.3 release: never enters the gate. Data is already written back. */
static void handoff_release(struct ncl_local_lock *L, int me)
{
    struct ncl_handoff_stats *h = &L->st.ho;
    const int P = hdr->participants;
    int w = -1;
    int probe = cfg.pick_policy == NCL_PICK_PROBE ||
                (cfg.pick_policy == NCL_PICK_ADAPTIVE && !L->acq_contended);
    if (probe && P > 1) {
        /* One probe at this node's round-robin pointer. Hit: hand off and
         * continue after that node next time. Miss: FREE (a waiter we did
         * not probe recovers through the gate) and advance by one. Under
         * saturation every slot is set, so the pointers sweep the ring. */
        int j = L->pw_next;
        h->announce_loads++;
        if (ncl_uc_load(&L->announce[j]))
            w = j;
        int nx = (j + 1) % P;
        if (nx == me)
            nx = (nx + 1) % P;
        L->pw_next = nx;
    } else
    w = ncl_uc_scan_first(L->announce, P, me + 1, P - 1, me, &h->announce_loads); /* round-robin after me */
    ncl_fence(); /* CS effects, data writeback and the scan before the owner store */
    if (w >= 0) {
        ncl_uc_store(L->owner, (uint64_t)w + 1); /* handoff = one 8-byte store */
        h->handoffs_sent++;
    } else {
        ncl_uc_store(L->owner, OWNER_FREE);
        h->frees++;
    }
    ncl_fence_ss(); /* owner store before the submutex unlock */
}

void ncl_acquire(struct ncl_thread *t, int lock)
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
         * within the node, so no invalidate. */
        ncl_fence();
        L->st.acquires++;
        L->st.path[NCL_PATH_LOCAL_HANDOFF]++;
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
                L->st.ho.wait_handoffs++;
            }
        }
    } else if (cfg.alg == NCL_ALG_FASTSRV) {
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
    L->ndirty = 0;
    L->dirty_full = 0;
    L->st.acquires++;
    L->st.path[path]++;
    ncl_base_stats_add(&L->st.base, &tmp);

    /* Global lock arrived at this node: drop stale cached data. */
    if (L->data_len && !cfg.debug_skip_data_inv)
        ncl_data_invalidate(L->data, L->data_len);
}

static void check_hint(const struct ncl_range *h)
{
    const char *p = h->p;
    if (p < ncl_mem.data || p + h->len > ncl_mem.data + ncl_mem.data_size) {
        fprintf(stderr, "ncl: dirty hint outside data region\n");
        abort();
    }
}

/* Remember this release's dirty ranges for the eventual global release. */
static void accumulate_dirty(struct ncl_local_lock *L, const struct ncl_range *hints, int nhints)
{
    if (!hints) {
        L->dirty_full = 1;
        return;
    }
    for (int k = 0; k < nhints && !L->dirty_full; k++) {
        check_hint(&hints[k]);
        if (L->ndirty == NCL_DIRTY_MAX)
            L->dirty_full = 1;
        else
            L->dirty[L->ndirty++] = hints[k];
    }
}

void ncl_release(int lock, const struct ncl_range *hints, int nhints)
{
    struct ncl_local_lock *L = &local[lock];

    if (cohort()) {
        accumulate_dirty(L, hints, nhints);
        int cap = cfg.local_cap > 0 ? cfg.local_cap : 64;
        if (ncl_sub_has_waiter(&L->sub)) {
            if (L->handoffs < cap) {
                /* Cohort pass: keep global ownership in the node. Fence so
                 * this thread's uncached stores reach the device first. */
                L->handoffs++;
                ncl_fence();
                ncl_sub_unlock(&L->sub);
                return;
            }
            L->st.cap_releases++;
        }
    }

    /* Global lock leaving this node: write back eagerly (design §2 rule 5). */
    if (!cfg.debug_skip_data_wb) {
        if (cohort()) {
            if (L->dirty_full) {
                if (L->data_len)
                    ncl_data_writeback(L->data, L->data_len);
            } else {
                for (int k = 0; k < L->ndirty; k++)
                    ncl_data_writeback(L->dirty[k].p, L->dirty[k].len);
            }
        } else if (!hints) {
            if (L->data_len)
                ncl_data_writeback(L->data, L->data_len);
        } else {
            for (int k = 0; k < nhints; k++) {
                check_hint(&hints[k]);
                ncl_data_writeback(hints[k].p, hints[k].len);
            }
        }
    }

    L->global_held = 0;
    L->st.global_releases++;
    if (cfg.alg == NCL_ALG_SERVER && cfg.mode == NCL_MODE_HANDOFF)
        ncl_srv_chain_release(lock, L->srv_grant, cfg.pick_policy == NCL_PICK_SCAN, &L->st.ho);
    else if (cfg.alg == NCL_ALG_SERVER)
        ncl_srv_release(lock);
    else if (cfg.alg == NCL_ALG_FASTSRV)
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
