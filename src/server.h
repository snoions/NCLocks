/*
 * Lock servers: variant (c) (design §4c) and the slow path of variant (e)
 * (design §4e). Private to the library.
 *
 * Phase 1: fixed home = hash(lock) % N, no migration, no local passing.
 *
 * Queues: for every (source node s, server node h) a ring of cap[h] uncached
 * lines, written only by node s, read only by h's server thread. Producers on
 * s reserve a position with a coherent fetch-and-add on a node-local tail and
 * publish with ONE 8-byte store:
 *
 *   63      48 47 46 45      26 25        14 13      0
 *   [ pos16  ][type][  lock   ][ grant_slot ][ req_id ]
 *
 * The server accepts the word at its expected position p only if pos16 ==
 * p & 0xffff and type != 0, so a zeroed or previous-lap slot is never read as
 * a new message (requires cap < 65536). cap[h] >= 2 x (locks homed at h): with
 * the submutex, each node has at most one request and one release in flight
 * per lock, so producers never need the consumer's head.
 *
 * Replies go straight to the requester's grant slot (one per thread, uncached,
 * written only by servers), also one 8-byte store:
 *
 *   [ lock (20) << 16 ][ status (2) << 14 ][ req_id (14) ]
 *
 * The waiter accepts only its own (lock, req_id); slots are never cleared.
 * The server bounds-checks lock, home, grant_slot and the releaser's identity
 * and counts violations as errors instead of acting on them.
 *
 * (e): the server also holds the top Peterson lock's SLOW side for each lock
 * it homes, from when its queue becomes non-empty until it drains. The
 * Peterson wait is polled across server rounds rather than spun inline, so a
 * fast-path holder of one lock never stalls the server's other locks.
 */
#ifndef NCL_SERVER_H
#define NCL_SERVER_H

#include "ncl/lock.h"

#define NCL_SRV_LOCK_BITS 20
#define NCL_SRV_GSLOT_BITS 12
#define NCL_SRV_REQID_BITS 14
#define NCL_SRV_MAX_CAP 32768 /* must stay < 2^16 for the pos16 check */

struct ncl_srv_layout {
    uint32_t grants_per_node;          /* = threads_per_node */
    uint32_t cap[NCL_MAX_NODES];       /* ring capacity of each queue into h */
    ncl_off_t q_off[NCL_MAX_NODES];    /* meta offset of h's N incoming rings */
    ncl_off_t grant_off;               /* meta offset of grant[N][G] */
};

/* Fill `L` with offsets starting at base_off; returns bytes needed, 0 on error. */
size_t ncl_srv_layout(const struct ncl_sys_cfg *c, struct ncl_srv_layout *L, ncl_off_t base_off);
int ncl_srv_home(int lock, int nnodes);

/* blocks/lines_per_lock locate each lock's base lines (FASTSRV top lock and
 * splitter); unused for SERVER. */
int ncl_srv_node_start(const struct ncl_sys_cfg *c, const struct ncl_srv_layout *L, int node,
                       ncl_line_t *blocks, size_t lines_per_lock);
void ncl_srv_node_stop(void);

/* (c) */
void ncl_srv_acquire(struct ncl_thread *t, int lock);
void ncl_srv_release(int lock);
/* (e): returns NCL_PATH_FASTSRV_FAST or _SLOW; release with that path. */
int ncl_fastsrv_acquire(struct ncl_thread *t, int lock, struct ncl_base *b,
                        struct ncl_base_stats *st);
void ncl_fastsrv_release(int lock, struct ncl_base *b, int path);

void ncl_srv_stats(struct ncl_srv_stats *out);

#endif
