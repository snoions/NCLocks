/*
 * Lock servers: variant (c) (design §4c) and the slow path of variant (e)
 * (design §4e). Private to the library.
 *
 * Phase 1: fixed home = hash(lock) % N, no migration, no local passing.
 *
 * Queues: for every (source node s, server node h) a ring of cap[h] scan-region
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
 * Starvation fix (not in design §4e as first written): after at least one
 * grant in the current hold, a release that leaves the queue non-empty while
 * the FAST flag is set makes the server yield the SLOW side (unlock and
 * re-announce) instead of granting, so a waiting fast winner gets its turn.
 * Without this, sustained slow-path contention keeps the queue non-empty
 * forever and the fast winner never enters.
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
    ncl_off_t q_off[NCL_MAX_NODES];    /* SCAN-region offset of h's N incoming rings */
    ncl_off_t grant_off;               /* meta offset of grant[N][G] */
};

/* Fill `L`: grant slots from meta offset base_off, queue rings (read by the
 * batched poll) from scan offset scan_off. Returns meta bytes needed (0 on
 * error) and the scan bytes in *scan_bytes. */
size_t ncl_srv_layout(const struct ncl_sys_cfg *c, struct ncl_srv_layout *L, ncl_off_t base_off,
                      ncl_off_t scan_off, size_t *scan_bytes);
int ncl_srv_home(int lock, int nnodes);

/* blocks/lines_per_lock locate each lock's meta lines: FASTSRV top lock and
 * splitter; SERVER in MODE_HANDOFF: the succ[] ring. sblocks/scan_per_lock:
 * its scan lines (FASTSRV present[]). */
int ncl_srv_node_start(const struct ncl_sys_cfg *c, const struct ncl_srv_layout *L, int node,
                       ncl_line_t *blocks, size_t lines_per_lock, ncl_line_t *sblocks,
                       size_t scan_per_lock);
void ncl_srv_node_stop(void);

/* (c). Returns the grant word (chain mode needs it at release). */
uint64_t ncl_srv_acquire(struct ncl_thread *t, int lock);
void ncl_srv_release(int lock);
/* (c) in MODE_HANDOFF ("chain"): succ[] lines per lock; release by passing to
 * the published successor (returns 1) or REL to the server (returns 0).
 * always_read: read succ[] even when the grant's hint bit is clear. */
uint32_t ncl_srv_chain_lines(const struct ncl_sys_cfg *c);
int ncl_srv_grant_passed(uint64_t grant);
int ncl_srv_chain_release(int lock, uint64_t grant, int always_read, struct ncl_handoff_stats *h);
/* (e): returns NCL_PATH_FASTSRV_FAST or _SLOW; release with that path. */
int ncl_fastsrv_acquire(struct ncl_thread *t, int lock, struct ncl_base *b,
                        struct ncl_base_stats *st);
void ncl_fastsrv_release(int lock, struct ncl_base *b, int path, struct ncl_base_stats *st);

void ncl_srv_stats(struct ncl_srv_stats *out);

#endif
