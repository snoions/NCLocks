# Locks for Non-Coherent Shared Memory: Design and Evaluation Spec

This document specifies a family of mutual-exclusion locks for multi-node systems whose shared memory is **not hardware-coherent across nodes**, and a phased plan for evaluating them. It is written as an implementation handoff: decisions are marked **Decided**, items still to be settled are marked **Open**, and anything deferred is listed under Future Work.

---

## 1. System model and assumptions

- **Nodes.** A small number of nodes (target: up to ~16, e.g. hosts sharing a CXL memory device) share a memory region. Design for N ≤ 16 but keep N a parameter.
- **Coherence.** Memory is cache-coherent *within* a node and **non-coherent across nodes**. Cross-node visibility requires explicit cache writeback (e.g. `clwb`/`clflushopt` + `sfence`) and invalidation before reads, or an uncached mapping.
- **No cross-node atomics assumed.** Algorithms use only loads and stores across nodes. Node-local coordination may use ordinary atomics (coherent).
- **Address spaces.** Two options; the memory-access layer should support either:
  - **Same virtual address on all nodes (preferred where possible).** Every process on every node maps the shared region at one agreed address (e.g. `mmap` with `MAP_FIXED_NOREPLACE`, reserved early at startup before other mappings can collide). Pointers into the region are then valid everywhere and can be stored in shared structures directly. If the reservation fails on any node, fail loudly rather than silently falling back.
  - **Per-node mapping address.** Anything stored in shared memory that refers to shared memory must be an offset into the region.
  - Either way, **compact messages use small indices** (e.g. a grant-slot index), because a full 8-byte pointer cannot fit in a single 8-byte message alongside the other fields. Anything a server writes on behalf of a message is still bounds-checked.
- **CXL 3.1 model (spec §2.4.4).** Memory shared by several hosts is "Shared FAM"; plain host-managed device memory (HDM-H) supports only *software-managed coherency*, which is this design's model. The spec leaves coordinating line ownership to software.
- **No clean writebacks (assumption, CXL 3.1 §2.4.4 implementation note).** Some caching agents spontaneously write back *clean* lines that hardware prefetched but software never modified. Such a writeback "can overwrite updates performed by a host or device that executed writes to the cacheline. This breaks software-managed coherency." The design's cached regions (protected data, read-mostly metadata) are correct only if every caching agent that can touch them never issues clean writebacks: all hosts and devices advertise "No Clean Writebacks" (CXL System Description Structure §9.18.1.6; DVSEC CXL Capability2 §8.1.3.7). Invalidate-before-read does not protect against this: a node that merely prefetched a data line could later overwrite the holder's writes. Uncached metadata is unaffected. **Open:** check the bit on the target platform; without it, protected data and the lock-to-home map would have to be uncached or confined to lines no other host can cache. This testbed (one host, emulated nodes) cannot exhibit the problem.
- **Writeback granularity.** A host's internal cache line may be larger than 64 B, so one writeback can become several CXL writes (same note). "Own cache line" in Section 2 means the host's writeback granularity (e.g. 128 B if adjacent-line pairs are written back together), not necessarily 64 B. **Open:** confirm for the target host; this testbed uses 64 B.
- **Untorn aligned 8-byte words (assumption).** Section 2 rule 1 relies on another node observing an aligned 8-byte store all-or-nothing. On x86 the store is a single access (SDM Vol. 3, guaranteed atomic operations). On CXL.mem it travels as one message (64 B data plus byte enables), and a cache line's data transfer "must occur with no interleaved transfers" (§3.3.11); a load returns the whole line in one response. The spec does not say how the device applies a write relative to a concurrent read, so untorn words *at the device* are assumed, not guaranteed. (PCIe likewise leaves write granularity out of scope and only recommends untorn aligned 4/8-byte writes.) **Open:** confirm with the device vendor and the single-writer partial-write test (Section 5).
- **Ordering (CXL 3.1 §3.3.11).** CXL.mem imposes no ordering between different addresses, and "write data is only guaranteed to be visible to a later access after the write is complete". Store-fence-load patterns (Dekker, Peterson, Lamport, the announce/owner handshake) therefore rely on the host's fence waiting for write completion (`Cmp`) of uncached stores. That is a host guarantee to confirm per platform, not something CXL provides.
- **Platform.** Real CXL hardware is not required; an emulated setup (e.g. NUMA machine with explicit flushes / uncached remote memory, optionally with injected latency) is acceptable. Keep the memory-access layer abstract so the backend can change.

## 2. Memory layout and access rules (Decided)

Three kinds of shared state, plus node-local state:

| Region | Mapping | Contents |
|---|---|---|
| Hot metadata | **Uncached** | Owner fields, base-algorithm variables (Peterson flags/turn, Lamport `x`/`y`/`b[]`), announcement flags, queue slots, grant slots |
| Read-mostly metadata | Cached, staleness-detected | Lock-to-home map for (c) (refreshed on NACK via version number); static configuration (region layout, node count) |
| Data | Cached | Data protected by the locks |
| Node-local (not shared across nodes) | Ordinary coherent memory | Local submutexes, per-acquisition path info (e.g. hybrid fast/tournament), local queue tail counters, local handoff counts |

**Option (implemented, `-U split`): split hot metadata by access pattern.** Uncached loads cannot overlap, so a scan of N uncached lines costs N serialized round trips, while N cached lines can be invalidated together and read in parallel after one fence. The split keeps single words that are polled, stored or handed off (owner, grant slots, `x`/`y`, Peterson flags/turn, splitter door/race, the chain's `succ[]`) uncached and moves the scanned arrays (Lamport `b[]`, `present[]`, `announce[]`, server queue rings) to a **cached** region with explicit writeback after each store and invalidate before each (batched) read. Every such array element has a single writer. The cached region then carries the cached-memory assumptions of Section 1 (no clean writebacks; own line at the host's writeback granularity), and stores to it are weakly ordered (`clflushopt`), so the explicit store-store fences of rule 4 matter. Measured on the uc testbed it is mostly slower than all-uncached: an uncached access waits for an earlier flush, so each cached store costs ~100 ns extra, and only the lock server's poll gains (`docs/backends.md`, "Per-region memory types"). Whether it pays on a CXL device is **Open**.

Rules:

1. **One aligned 8-byte store per message/state update.** Never rely on 64-byte line writes being atomic at the device. Anything a reader must see consistently (e.g. payload + sequence number) fits in one 8-byte word.
2. **Cached multi-writer variables get their own cache line** (at the host's writeback granularity, Section 1). A writeback writes the whole line and can overwrite another node's data. (Applies to anything kept in a cached mapping.)
3. **Packing uncached slots below one per line.** CXL 3.1 defines byte-precise partial writes: `MemWrPtl` carries 64 byte enables that indicate which bytes are valid (Table 3-41, §3.3.6.1). The spec says nothing about atomicity: it does not say whether a write can be observed torn by a concurrent read, or whether partial writes from different hosts to one line are merged atomically. Nor does it say what a host emits for an 8-byte uncached store. With host-side memory encryption, `MemWrPtl` is allowed only if the byte enables are aligned to encryption boundaries (e.g. 32 B), and merging moves to the host (`MemRdFill`, Table C-8), i.e. a host-side read-modify-write. Two cases:
   - **Single-writer lines** (e.g. the (c) chain's `succ[]` ring, written only by the home server): need only that the device honors byte enables, plus untorn 8-byte words, which rule 1 already assumes. A host-side read-modify-write rewrites only that writer's own words. **Open, low risk:** confirm with the partial-write stress test (Section 5).
   - **Multi-writer lines** (e.g. one flag per node in one line): additionally need the device to apply concurrent partial writes from different hosts without losing any (an atomic per-line merge), and no host-side read-modify-write (e.g. no host-side encryption on the region). Neither is guaranteed by the spec. **Open:** do not pack until verified on the target device.
   - Default to one slot per line until verified. If packed, no node may map those lines as cacheable.
4. **Explicit fences** wherever uncached and cached accesses are mixed, and between a store and a subsequent load that must observe ordering (Dekker-style patterns). Do not rely on memory-type ordering alone.
5. **Protected data** is written back when the global lock leaves a node and invalidated when it arrives. Support two modes: full writeback/invalidate (conservative) and precise dirty-range hints supplied by the caller (used by the KV store). Write back eagerly, as soon as the lock is known to be leaving the node.
6. **Read-mostly metadata** (e.g. the lock-to-home map in the lock-server variant) stays cached and is refreshed only when a reply indicates it is stale (NACK with version). Because different nodes may update different entries concurrently, **each entry gets its own cache line**, and the writer issues a writeback + fence after updating it.
7. **Open:** check whether the device supports hardware coherence (CXL back-invalidation) for any region; if so, metadata could live there.

## 3. Lock architecture

### 3.1 Hierarchy (Decided)

Each global lock has one **node-local submutex** per node. A thread acquires its node's submutex first, so at most one thread per node contends for the global lock and the global algorithm has exactly N participants (indexed by node ID).

- Local submutex: MCS-style (or ticket) lock using coherent atomics. Must support **local-waiter detection** (is there a successor?) for cohort handoff.

### 3.2 Local handoff (cohort passing) (Decided)

On release, if the local submutex has a waiter and the consecutive-local-handoff count is below a cap, pass both the submutex and global ownership to the next local thread without touching global state.

- **Cap** on consecutive local handoffs (parameter; sweep in evaluation) for cross-node fairness.
- **Thread-oblivious requirement:** global ownership must belong to the node, not the thread. Any per-acquisition state needed at release (e.g. which path the hybrid used) is stored **with the lock in node-local memory**, never in thread-local storage.
- Before a local handoff, issue a fence so the outgoing thread's uncached stores reach the device before the next thread proceeds.

### 3.3 Cross-node handoff: gate + owner field (Decided)

Applies to variants (a), (b), (d). The base algorithm is **not** held during the critical section. It is a short **gate** protecting a single **owner field** (uncached, 8 bytes): `FREE` or a node ID (optionally with an epoch in the remaining bits).

```
acquire(L):                      # caller holds node-local submutex
    announce[L][me] = 1          # Open: announce inside gate (current) or before entering it (see 3.5)
gate:                            # try to acquire through the gate
    gate_enter(L)                # base algorithm (a), (b) or (d)
    o = L.owner
    if o == FREE or o == me:     # free, or a handoff already arrived
        L.owner = me             # single 8-byte store
        gate_exit(L)
        goto acquired
    gate_exit(L)
wait:                            # poll the owner field directly
    o = L.owner
    if o == me:   goto acquired  # handoff received
    if o == FREE: goto gate      # missed announcement: retry through the gate
    backoff()
    goto wait
acquired:
    announce[L][me] = 0
    return

release(L):                      # never enters the gate
    w = pick_waiter(L)           # scan announce[L][*], round-robin from me
    if w != NONE: L.owner = w    # handoff = one 8-byte store
    else:         L.owner = FREE
```

Properties:

- **Safety:** while held, only the owner writes `owner`; gate users read either "held" (wait) or `FREE` (take).
- **Liveness:** a missed announcement only means the waiter sees `FREE` on a later poll and goes back through the gate. Count these recoveries.
- Every node only ever writes its own base-algorithm variables (each node enters and exits the gate itself).
- **No separate handoff flag (Decided).** Waiters poll `owner` directly. Split out per-waiter flags only if measurements show a hotspot on the owner line or a handoff needs a larger payload.
- `pick_waiter` scans announcements round-robin starting after the releasing node, for fairness.

### 3.4 Phase 1 mode (no handoff)

For Phase 1 of the evaluation, the base algorithm **is** held across the critical section (no gate, no owner field): `acquire = base_lock()`, `release = base_unlock()`. Keep this as a build/runtime mode.

### 3.5 Open items in the handoff design

- **Announce before entering the gate** (instead of inside it) would shorten gate hold time and is safe given the "owner == me means handoff" rule. Evaluate both; current baseline announces inside.
- Epoch bits in the owner word: useful for debugging and data-writeback versioning; bit allocation TBD.
- *Measured (Phase 2b, `docs/handoff.md`):* both placements are implemented (`-G inside|before`), plus a third, **lazy** (announce inside the gate only after reading a held owner). **Default: `before`.** Lazy (the earlier default) collapses when the gate is slow relative to the handoff: waiters pile up in the gate unannounced, releasers store FREE, and 2b degrades to Phase 1 (Lamport-2b on cached-flush: 1.03M with `before` vs 0.52M, fairness 0.76, with lazy). A releaser's `pick_waiter` may also probe a single slot when its own acquisition was uncontended, instead of scanning (`-X adaptive`, default; a miss stores FREE and the waiter recovers through the gate). Neither changes the safety argument above. `before` cuts missed-announcement recoveries ~100x with otherwise similar latency and capacity. The uncontended cost is ≈ +4 round trips, not +1 (announce store, owner load, owner store, announce clear, plus fences), and the release scan of `announce[]` adds up to N−1 loads.

## 4. Global algorithms

All operate on N node-indexed participants. Every shared variable lives in the uncached metadata region. Insert fences between a store and a later load whose order matters.

### (a) Lamport's fast mutex

Node IDs 1..N; `x`, `y` (0 = empty), `b[1..N]`.

```
start:
    b[i] = true                  # store-store order with x = i (free on x86; fence on weaker ISAs)
    x = i
    fence                        # (1) x = i must complete before reading y
    if y != 0:
        b[i] = false
        await y == 0
        goto start
    y = i
    fence                        # (2) y = i must complete before reading x
    if x != i:
        b[i] = false
        fence                    # b[i] = false before scanning b[] (slow path)
        for j in 1..N: await b[j] == false
        if y != i:
            await y == 0
            goto start
    # critical section (or gate body)
exit:
    fence                        # critical-section effects (incl. data writeback) before release
    y = 0
    b[i] = false
```

Uncontended cost ≈ 2 serialized store-fence-load round trips, at fences (1) and (2). Slow path is O(N) and not starvation-free. With uncached mappings on x86 some fences may be redundant; keep them for portability, model checking, and cached-mapping emulation.

### (b) Tournament of Peterson locks

Binary tree over N nodes (depth ⌈log₂N⌉). At each tree node, a two-party Peterson lock with `flag[2]`, `turn`; a participant's side at each level is derived from its node ID.

```
peterson_lock(t, side):
    t.flag[side] = true
    t.turn = side            # "victim" convention
    fence
    while t.flag[1-side] and t.turn == side: backoff()
peterson_unlock(t, side):
    t.flag[side] = false
```

Acquire climbs leaf to root; release unlocks root to leaf (release writes can share one fence). Uncontended ≈ log₂N round trips; starvation-free.

### (d) Hybrid (fourth variant, Decided)

A splitter fast path, a tournament slow path, and a final two-party Peterson lock between them. This is the standard fast-path composition from the mutual-exclusion literature, with the splitter-reset rule taken from Aspnes, *Notes on Theory of Distributed Systems*, §18.5.2 (arXiv:2001.04235).

**Components (all variables in the uncached metadata region):**

- Splitter: `race` (node ID or none) and `door` (`OPEN`/`CLOSED`, initially `OPEN`).
- `present[1..N]`: each node's presence flag, written only by that node.
- Tournament over N nodes (Section 4b).
- Top lock: two-party Peterson lock with a `FAST` side and a `SLOW` side.

```
acquire(i):
    present[i] = 1
    race = i
    fence                              # (1)
    if door == CLOSED: goto slow       # splitter: "right"
    door = CLOSED
    fence                              # (2)
    if race != i: goto slow            # splitter: "down"
    path = FAST                        # splitter: "stop" (at most one node)
    peterson_lock(top, FAST)
    goto cs
slow:
    path = SLOW
    tournament_lock(i)
    peterson_lock(top, SLOW)
cs:
    # critical section (or gate body)

release(i):
    fence
    if path == FAST:
        peterson_unlock(top, FAST)
        if present[hint] == 0:         # fairness rule (see below); was: unconditional
                                       # (strict form: all(present[j] == 0 for j != i))
            door = OPEN                # fast-path winner resets the splitter
    else:
        peterson_unlock(top, SLOW)
        # Reset the splitter only if no node can be on the fast path:
        # door is CLOSED and every other present[j] is 0.
        if door == CLOSED and all(present[j] == 0 for j != i):
            door = OPEN
        tournament_unlock(i)
    present[i] = 0
```

- **Why the top lock is needed:** the splitter admits at most one fast-path winner and the tournament admits at most one slow-path winner, but nothing else stops one of each from entering together.
- **Why the reset rule is needed:** if every splitter contender is deflected, nobody wins the fast path, so no fast-path winner exists to reopen `door`. A slow-path node may reopen it only when it can be sure no node is on, or about to take, the fast path. Every node sets `present[i]` before touching the splitter, so a node that is about to take the fast path is visible. Under sustained contention the fast path stays closed (by design); the last slow-path node to leave reopens it.
- **No restarts anywhere:** every node either wins the splitter once or goes to the tournament, so starvation-freedom follows from the tournament and the top Peterson lock.
- **Fairness rule for the fast-path winner's reset (added after measurement; model-check):** with an unconditional `door = OPEN`, the winner's node re-wins the splitter immediately (its next local thread, or the same thread in a closed loop), while deflected nodes wait on the slow path and never retry the splitter. The top Peterson lock alternates FAST and SLOW, so one node gets about 50% of all acquisitions (measured 4:2:1:1 at 4 nodes with local handoff). The winner therefore reopens only if no other node is present; otherwise the last slow-path releaser reopens it via the normal reset rule. Skipping a reopen is always safe, and no other resetter can act while `present[i]` is still set, so the check needs no `door` read. Strict form: reopen only if every other `present[j]` is 0, at a cost of N−1 uncached loads on an uncontended fast release (measured +600 ns per hold at N=8). **Default form (hint):** probe only `present[hint]`, where `hint` is the node-local index last seen set by a reset check or by this rule, and reopen if it is 0. One load. A miss can reopen while some other node waits; that is the original unconditional reopen, so safety is unaffected and only fairness is weaker. Measured fairness stays 1.00, because under contention the remembered node is almost always still present (`docs/hybrid.md`, "Fast-release check").
- **Open:** the exact order of the reset check relative to `tournament_unlock` and clearing `present[i]` must follow the source precisely; model-check it under the memory rules.
- **Cost:** uncontended fast path ≈ 3 round trips (splitter fences (1) and (2), plus the top Peterson lock). Slow path ≈ tournament (⌈log₂N⌉) + 1 for the top lock. Slow-path release adds a scan of `present[]`, whose cost depends on layout:

| `present[]` layout | Lines | Scan cost |
|---|---|---|
| Uncached, one slot per line (default) | N | ~N serialized round trips (uncached loads don't overlap) |
| Cached, one slot per line (each writer writes back + fences after updating its own line) | N | ~1 round trip + N invalidates (loads overlap) |
| Uncached, densely packed, e.g. one byte per node (multi-writer: only if partial writes are verified on the target device, Section 2 rule 3) | 1 | ~1–2 reads |

- Record in node-local lock state which path was taken (needed at release, and for thread-obliviousness).
- **Possible optimization (Open, only if measurements warrant):** overlap the top-lock writes with the splitter's second fence so the fast path costs ≈ 2 round trips. The fast side of the top lock must stay owned by the unique splitter winner; a deflected node must never touch it.
- **Further reading if the slow-path release scan matters:** Anderson & Kim, "A new fast-path mechanism for mutual exclusion" (Distributed Computing, 2001), which keeps O(1) contention-free cost and O(log N) under contention; and Anderson, Kim & Herman, "Shared-memory mutual exclusion: major research trends since 1986" (survey, https://www.cs.unc.edu/~anderson/papers/survey.pdf).

### (c) Lock servers

Each lock has a **home node** whose background server thread grants it.

**Messages toward the server** (requests, releases) go through pairwise node-to-server queues in the metadata region:

- **Multi-producer within a node, single consumer at the server.** Producers reserve a slot with an atomic fetch-and-add on a node-local tail counter (coherent), write the message, and publish it via a per-slot sequence number. Cross-node, each queue line is written only by one node.
- Each message is **one 8-byte store** containing type, lock ID, request ID / sequence, and grant-slot index. Bit allocation TBD.
- **Capacity ≥ 2 × (locks homed at that server)**: at most one request and one release per lock per node can be in flight, so producers never need to read the consumer's head.
- Keep the window between slot reservation and publish tiny; a preempted producer stalls all locks homed at that server.

**Replies** (grant, busy, NACK-with-new-home) are written by the server **directly to a grant slot named in the request** (Decided):

- The request carries a **grant-slot index** (an index rather than a pointer, so it fits in the 8-byte message; see Section 1). The server bounds-checks it before writing.
- Grant slots are per thread (small reusable pool). Only the server writes them; the waiter polls its slot.
- The reply word includes the request ID; the waiter accepts only a matching ID, so slots are never cleared and stale replies are ignored.
- **Late grants:** either do not support abandoning a queued request (trylock gets an immediate busy-or-grant reply), or the node must release any grant for a request it abandoned.

**Handoff:** the server's queue provides handoff naturally. With a fixed home, a cross-node handoff is two hops (holder → server → waiter). Local handoff: the node keeps the grant across its local waiters and notifies the server only when the lock leaves the node.

**Holder-to-waiter chain (Phase 2b for (c); implemented, not model-checked; `docs/handoff.md`):** removes the server from the critical path of a contended handoff.
- The home server gives each request a grant number n in FIFO order and publishes "holder of n−1: pass to waiter n" as one 8-byte word in a per-lock uncached `succ[R]` ring (written only by the server).
- At release, the holder of k reads `succ[k % R]` once. If the tag is k, it stores grant k+1 straight into that waiter's grant slot (no message). Otherwise it sends `REL(k)`, and the server grants k+1 itself if assigned. Each holder decides exactly once, and the server grants k+1 only after `REL(k)`, so k+1 is granted exactly once. Grant slots therefore have two possible writers per request (server or previous holder), but exactly one writes.
- A hint bit in the grant ("succ[] already published when granted"; set on every pass) lets an uncontended holder skip the `succ[]` load.
- The server never sees passes, so it assigns at most R numbers past the last one it knows finished (R = `succ[]` lines per lock, a power of two; default 4N rounded up, `-R`). A full window holds requests back, which ends the chain and resynchronizes the server at least every R grants; a larger window trades uncached memory for fewer server round trips. Memory: R lines per lock (2 KB at N=8), the largest per-lock item of any variant; reductions (smaller R, a pool of rings assigned only to contended locks, packing entries: single-writer, so only byte-enable semantics are needed, Section 2 rule 3) are listed in `docs/handoff.md` (Open, needed before the KV store).
- The server clears slot n % R when assigning n. Without this, a slot the chain never refills keeps an old entry until the 14-bit tag wraps and matches again (found by stress: a pass to a long-finished request lost the lock).

**Home migration:**

- Migrate when one node dominates recent acquisitions (adaptive policy with hysteresis; high threshold to avoid thrashing under uniform access). If the home moves to the holder, the holder grants directly (one hop).
- Change the home **only while the lock is held**, bumping a version number. Requests reaching an old home get a NACK with the new home and version; the requester retries.
- The lock-to-home map is read-mostly: cache it and refresh on NACK. Spread initial homes (e.g. hash of lock ID).
- Servers poll incoming queues with adaptive backoff. Background poll traffic is O(N²) cluster-wide; measure it.

### (e) Fast-path + lock-server hybrid (fifth variant)

Same composition as (d), with the lock server of (c) replacing the tournament as the slow path.

**Components:**

- Splitter (`race`, `door`) and `present[1..N]`, as in (d).
- Lock server, queues, and grant slots, as in (c).
- Top lock: two-party Peterson lock with a `FAST` side and a `SLOW` side. **The `SLOW` side is held by the lock's server thread**, not by individual grantees, so only the server writes the slow side's variables.

**Fast path (node i):** `present[i] = 1`; run the splitter as in (d); if it wins (`stop`), acquire the top lock's `FAST` side and enter. The server is not involved. On release: unlock the `FAST` side, set `door = OPEN`, clear `present[i]`.

**Slow path (node i):** if the splitter deflects it, send a request to the lock's home server and wait for a grant in its grant slot (Section 4c). On release, clear `present[i]`, fence, then send the release message to the server. (If `present[i]` were cleared after the release message, the server's reset check could still see it set and leave the fast path closed until the next contended episode.)

**Server:**

```
on request(r):
    enqueue r
    if not holding_slow:
        peterson_lock(top, SLOW)       # waits out a fast-path holder, if any
        holding_slow = true
    if lock is free: grant(dequeue())

on release:
    if queue not empty and granted_since_acquire > 0 and flag[FAST]:
        peterson_unlock(top, SLOW)     # starvation fix (see below): let the
        peterson_lock(top, SLOW)       # waiting fast winner in, then resume
    if queue not empty:
        grant(dequeue())               # contended handoff: no top-lock traffic
    else:
        peterson_unlock(top, SLOW)
        holding_slow = false
        # splitter reset: reopen the fast path only if no node can be on it
        if door == CLOSED and all(present[j] == 0):
            door = OPEN
```

- **Starvation fix (added after measurement; model-check):** as first written, the server released the SLOW side only when its queue emptied. Under sustained slow-path contention the queue never empties, so a fast-path winner waiting at the top lock starved (measured: one node with zero completions). The server now yields the SLOW side between grants whenever `flag[FAST]` is set, after granting at least once in the current hold, so a stream of fast winners cannot starve the queue either.
- **Why the server owns the slow side:** contended handoffs go grantee to grantee through the server's queue without touching the top lock. The top lock is acquired once when contention starts and released once when the queue empties. The splitter-reset scan moves off the releasing node's critical path onto the server.
- **Benefit over (c):** uncontended acquisitions bypass the server (≈ 3 round trips, no polling delay), so the server's poll backoff only affects contended acquisitions. The server can back off aggressively when idle, cutting background poll traffic and CPU.
- **Benefit over (d):** contended behavior is (c)'s queue-based handoff with no tournament.
- **When the fast path is not cheaper:** a request/grant to a busy, aggressively polling server (≈ 1 round trip + polling delays) can beat the fast path's ≈ 3 round trips. The fast path wins when the server is idle or backed off, i.e. the uncontended case.
- **Overlap with home migration:** both make uncontended reacquisition cheap; migration is cheaper still when the home is local. Evaluate whether (e) still needs migration (Phase 2c).
- **Open (unverified design, model-check before benchmarking):**
  - the server acquiring and releasing the top lock's `SLOW` side while grantees come and go;
  - the splitter-reset ordering relative to grantees clearing `present[i]` (a deflected node must stay visible in `present[]` until it no longer needs the slow path);
  - NACK and home migration while the server holds the `SLOW` side (the side must move with, or be released before, a home change);
  - interaction with local handoff (a node keeping a grant across its local waiters keeps the server's queue non-empty from the server's point of view).

## 5. Correctness verification

- **Model checking** (e.g. GenMC, or TLA+ with an explicit model of writeback and invalidation): base algorithms under the memory rules; gate + owner field + announcements; hybrid (splitter + tournament + top Peterson, including the splitter-reset rule and its ordering); lock-server queues, grant slots, NACK/migration; (e) server ownership of the top lock's slow side and its splitter reset.
- **Runtime stress tests:** each lock protects a counter and a checksum over shared data; inject random delays; check invariants after millions of acquisitions. Run per phase.
- **Partial-write stress test:** gates dense packing (Section 2 rule 3). Two modes:
  - *Multi-writer:* nodes repeatedly write their own bytes or words within a shared uncached line while checking the other nodes' values.
  - *Single-writer:* one node writes sequence-tagged 8-byte words across a line while the others read and check for torn words and disturbed neighbors. This also checks the untorn-word assumption (Section 1) that rule 1 relies on.
  - Meaningful only on the target device and host. On this single-host testbed, the DRAM controller does the merge.

## 6. Evaluation plan

### 6.1 Workloads (Decided)

- **SPLASH** and **PARSEC** (PARSEC 3.0 bundles SPLASH-2/2x), run unmodified via LiTL-style pthread interposition (https://github.com/multicore-locks/litl). Start with the lock-sensitive subset identified in Guiroux et al., "Multicore Locks: The Case Is Not Closed Yet" (USENIX ATC'16).
- **Key-value store driven by YCSB.** This one is modifiable, so it provides precise dirty-range hints.
- **Parameterized microbenchmark.**

Interposition requirements: thread-to-slot mapping; protected-data writeback (interposition cannot scope it, so use full flush or uncached data for these apps and state which); `pthread_mutex_trylock` for every variant; condition-variable support; `pthread_create` interposition fix for glibc ≥ 2.34.

### 6.2 Microbenchmark axes

- Node count N
- Contention level (arrival rate) and arrival burstiness at fixed rate
- Access skew: uniform, one dominant node, phased (dominant node changes over time)
- Critical-section length and data footprint
- Ratio of shared vs node-local memory accesses inside critical sections
- Threads per node and how evenly they are spread

**Load-generation modes:**

- **Closed-loop** (each thread: acquire → critical section → release → optional think time → repeat): throughput microbenchmarks. SPLASH/PARSEC are batch jobs measured by completion time or throughput.
- **Open-loop arrivals** (each thread acquires at scheduled times drawn from an arrival process, e.g. Poisson or temporally clustered bursts; lock latency measured from the scheduled time): required for the arrival-rate and burstiness axes and for latency results, since closed-loop threads set their own rate and back off under contention.

### 6.3 YCSB mapping

- Zipfian vs uniform keys → skew
- Lock granularity (per-bucket vs coarse) → number of locks, collision rate
- Workloads A/B/C/F → read/write mix
- Value size → data footprint
- **Open-loop** load generation at fixed offered loads, with latency measured from each request's intended start time (YCSB is closed-loop by default; check whether the version in use can correct for this, or use a separate open-loop driver)

### 6.4 Phases

**Phase 1: base algorithms, no handoff.** Local submutex kept; base algorithm held across the critical section (Section 3.4); (c) and (e) with a fixed home, no migration, no local passing.
Look for: the (a)/(b) crossover vs N and burstiness; whether (d) tracks the better of the two; (c) fixed-home latency; (e) vs (c) uncontended latency and poll traffic as the server's poll backoff varies; degradation under contention.

**Ablation: no local submutex.** Run right after Phase 1, reusing its code. Variants (a), (b), (d) sized for the total thread count. Sweep threads per node at fixed N; microbenchmarks plus one or two lock-sensitive apps. Skip (c) and (e) (without a submutex they need per-thread queues, which is a different design). Purpose: show why the hierarchy is needed.

**Phase 2: with handoff.**

- **2a, local handoff only.** Sweep the local cap, threads per node, placement balance, and data footprint. For (c) and (e), the node keeps the grant across local waiters.
- **2b, local + cross-node handoff.** Gate + owner field. First measure uncontended cost vs Phase 1 (expected ≈ +1 round trip for the owner-field read). Sweep skew and burstiness. Measure release-to-next-start latency vs Phase 1, gate wait time, missed-announcement recoveries, and contention on the owner line.
- **2c, (c)/(e)-specific.** Home migration; sweep the reassignment threshold with phased skew; compare one-hop vs two-hop handoff; check whether local handoff and migration overlap enough that one suffices; check whether (e)'s fast path makes migration unnecessary; sweep the server's idle poll backoff.

### 6.5 Baselines

- Same locks on a **coherent NUMA** machine (separates algorithm cost from the cost of non-coherence)
- An **atomic-based lock** (ticket/MCS in device memory), if cross-host atomics are available
- A **no-lock / single-thread** upper bound
- **Data-movement cost references:** (i) microbenchmark runs with zero-footprint critical sections, to isolate lock overhead from data movement; (ii) the same microbenchmark with all threads on one node, as an approximate best case for keeping data in one place

### 6.6 Metrics and instrumentation

- Throughput; tail latency (p50/p99/p99.9) at fixed offered load
- Uncontended round trips and latency per acquire-release pair
- **Per-acquisition path breakdown:** fast path, local handoff, global handoff, base algorithm, server request, missed-announcement recovery
- Time split: in lock, in data writeback, in critical section
- Background interconnect bandwidth from polling
- CPU time per operation, including server threads
- If emulating: sweep injected interconnect latency

### 6.7 Parameters to sweep

Local handoff cap; reassignment threshold and migration window for (c)/(e); server idle poll backoff for (c)/(e); poll backoff caps; N; threads per node; injected latency.

## 7. Future work

- **Reader-writer locks.** E.g. per-node reader counters (single writer per node, so no multi-writer lines); writers take the global lock and scan the counters; readers on a node share the local submutex in read mode; each reading node invalidates protected data on acquire.
- **Abortable locks.** Timeout/trylock semantics across all variants: withdrawing cleanly from the gate and base algorithms; for (c), handling grants that arrive after a request was abandoned (Section 4c).
- **Partial failure tolerance.** A node crashing while holding the lock, or while inside the gate or a queue operation, must not block other nodes indefinitely. Likely ingredients: leases or epochs on ownership, failure detection, and recovery of base-algorithm state and in-flight queue slots.
- **Lease parking of idle global locks.** A node keeps an idle global lock under a short lease so local reacquisition is free. If pursued: all state changes stay on the owner node (node-local states `PARKED`/`HELD`/`RELEASING` via coherent CAS); release is triggered by a per-node timer and/or a poller watching announcements; the releasing agent writes back protected data before releasing.
