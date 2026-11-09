#include "ncl/lock.h"

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
};

struct ncl_lock_desc {
    ncl_off_t data_off;
    uint64_t data_len;
};

/* Node-local per-lock state (ordinary coherent memory, design §2 table).
 * Fields other than `sub` are written only by the current holder, so the
 * state needed at release lives with the lock, not in the thread. */
struct ncl_local_lock {
    struct ncl_submutex sub;
    struct ncl_base base;
    int path;            /* path taken by the current holder */
    int holder;          /* participant index of the current holder */
    void *data;
    size_t data_len;
    struct ncl_node_stats st;
} NCL_ALIGNED(NCL_CACHELINE);

static struct ncl_hdr *hdr;          /* in shared meta */
static struct ncl_sys_cfg cfg;       /* node-local copy */
static struct ncl_local_lock *local; /* node-local array [nlocks] */
static int my_node = -1;

static size_t round_line(size_t n) { return (n + NCL_CACHELINE - 1) & ~(size_t)(NCL_CACHELINE - 1); }

const char *ncl_mode_name(ncl_mode_t m)
{
    return m == NCL_MODE_PHASE1 ? "phase1" : m == NCL_MODE_HANDOFF ? "handoff" : "?";
}

int ncl_sys_create(const struct ncl_sys_cfg *c)
{
    if (c->mode != NCL_MODE_PHASE1) {
        fprintf(stderr, "ncl: mode %s not implemented yet\n", ncl_mode_name(c->mode));
        return -1;
    }
    if (c->nnodes < 1 || c->nnodes > NCL_MAX_NODES || c->nlocks < 1) {
        fprintf(stderr, "ncl: bad nnodes=%d / nlocks=%d\n", c->nnodes, c->nlocks);
        return -1;
    }
    int P = c->no_submutex ? c->nnodes * c->threads_per_node : c->nnodes;
    if (P < 1 || P > NCL_MAX_PARTICIPANTS) {
        fprintf(stderr, "ncl: %d participants exceeds NCL_MAX_PARTICIPANTS=%d\n", P,
                NCL_MAX_PARTICIPANTS);
        return -1;
    }

    size_t lines_per_lock = ncl_base_nlines(c->alg, P);
    size_t hdr_sz = round_line(sizeof(struct ncl_hdr));
    size_t descs_sz = round_line(sizeof(struct ncl_lock_desc) * (size_t)c->nlocks);
    size_t locks_sz = lines_per_lock * NCL_CACHELINE * (size_t)c->nlocks;
    size_t user_sz = c->user_meta_lines * NCL_CACHELINE;

    struct ncl_mem_cfg mc = {
        .backend = c->backend,
        .meta_size = hdr_sz + descs_sz + locks_sz + user_sz,
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
        local[l].data = d[l].data_len ? ncl_data_ptr(d[l].data_off) : NULL;
        local[l].data_len = d[l].data_len;
    }
    return 0;
}

void ncl_thread_init(struct ncl_thread *t, int local_idx)
{
    memset(t, 0, sizeof *t);
    t->node = my_node;
    t->local_idx = local_idx;
    t->participant = my_node * cfg.threads_per_node + local_idx;
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

    struct ncl_base_stats tmp = {0};
    int path = ncl_base_lock(&L->base, part, &tmp);

    /* Global lock held: holder-only fields below. */
    L->path = path;
    L->holder = part;
    L->st.acquires++;
    L->st.path[path]++;
    ncl_base_stats_add(&L->st.base, &tmp);

    /* Global lock arrived at this node: drop stale cached data. */
    if (L->data_len && !cfg.debug_skip_data_inv)
        ncl_data_invalidate(L->data, L->data_len);
}

void ncl_release(int lock, const struct ncl_range *hints, int nhints)
{
    struct ncl_local_lock *L = &local[lock];

    /* Global lock leaving this node: write back eagerly (design §2 rule 5). */
    if (!cfg.debug_skip_data_wb) {
        if (!hints) {
            if (L->data_len)
                ncl_data_writeback(L->data, L->data_len);
        } else {
            for (int k = 0; k < nhints; k++) {
                const char *p = hints[k].p;
                if (p < ncl_mem.data || p + hints[k].len > ncl_mem.data + ncl_mem.data_size) {
                    fprintf(stderr, "ncl: dirty hint outside data region\n");
                    abort();
                }
                ncl_data_writeback(p, hints[k].len);
            }
        }
    }

    ncl_base_unlock(&L->base, L->holder, L->path, &L->st.base);
    if (!cfg.no_submutex)
        ncl_sub_unlock(&L->sub);
}

const struct ncl_sys_cfg *ncl_sys_cfg(void) { return &cfg; }
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
        for (int p = 0; p < NCL_PATH__COUNT; p++)
            out->path[p] += s->path[p];
        ncl_base_stats_add(&out->base, &s->base);
    }
}
