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

**Measured on the CXL host (2026-11-17, `docs/cxl_results.md` "Device concurrency"): on that device, uncached hot metadata is throughput-capped, so use cached metadata with full-line writeback there.** Every 8-byte write to the CXL memory, whether an uncached store or an 8-byte non-temporal store from write-back memory, is served at a fixed aggregate rate: 8.4-9.0M writes/s in total from 8 cores on, about 1/45 of the device's 24-26 GB/s full-line bandwidth. Full-line writes and uncached loads scale linearly to 60 cores (150-180M/s), and the same 8-byte writes scale on DRAM. The device, or the host's CXL path, serves partial writes (`MemWrPtl`) slowly. Every uncached-metadata lock reached this cap, while the cached mapping (`-b cached-flush`: each variable in its own line, writeback after each store, invalidate before each read) was faster in every phase. **Decision:** the uncached mapping above stays the reference design and the model for correctness arguments. On a device with this cap, the recommended configuration maps hot metadata cached with explicit full-line writeback and invalidate (`cached-flush`). That is correct under the existing rules (2 and 4: own line per variable, explicit writeback/invalidate, fences), but it moves hot metadata under the cached-memory assumptions of Section 1: no clean writebacks, and own line at the host's writeback granularity. Every hot-metadata write then becomes a full-line write, so a line with a second writer is overwritten whole by each writer's writeback. The only such lines are the announce lines under `--wait own` (waiter's announce + releaser's grant), whose writes are causally ordered (§3.3). The emulator writes back the node's whole line, so ctest checks that argument (Section 5). Other platforms should measure this (`ncl_memconc`, sweep phase `conc`) before choosing a mapping.

**Option (implemented, `-U split`): split hot metadata by access pattern.** Uncached loads cannot overlap, so a scan of N uncached lines costs N serialized round trips, while N cached lines can be invalidated together and read in parallel after one fence. The split keeps single words that are polled, stored or handed off (owner, grant slots, `x`/`y`, Peterson flags/turn, splitter door/race, the chain's `succ[]`) uncached and moves the scanned arrays (Lamport `b[]`, `present[]`, `announce[]`, server queue rings) to a **cached** region with explicit writeback after each store and invalidate before each (batched) read. Every such array element has a single writer, except the announce lines under `--wait own`, whose second writer (the granting releaser) is causally ordered with the owner (§3.3 "No separate handoff flag"). The cached region then carries the cached-memory assumptions of Section 1 (no clean writebacks; own line at the host's writeback granularity), and stores to it are weakly ordered (`clflushopt`), so the explicit store-store fences of rule 4 matter. Measured on the uc testbed it is mostly slower than all-uncached: an uncached access waits for an earlier flush, so each cached store costs ~100 ns extra, and only the lock server's poll gains (`docs/backends.md`, "Per-region memory types"). Whether it pays on a CXL device is **Open**.

Rules:

1. **One aligned 8-byte store per message/state update.** Never rely on 64-byte line writes being atomic at the device. Anything a reader must see consistently (e.g. payload + sequence number) fits in one 8-byte word.
2. **Cached multi-writer variables get their own cache line** (at the host's writeback granularity, Section 1). A writeback writes the whole line and can overwrite another node's data. (Applies to anything kept in a cached mapping.)
3. **Packing uncached slots below one per line.** CXL 3.1 defines byte-precise partial writes: `MemWrPtl` carries 64 byte enables that indicate which bytes are valid (Table 3-41, §3.3.6.1). The spec says nothing about atomicity: it does not say whether a write can be observed torn by a concurrent read, or whether partial writes from different hosts to one line are merged atomically. Nor does it say what a host emits for an 8-byte uncached store. With host-side memory encryption, `MemWrPtl` is allowed only if the byte enables are aligned to encryption boundaries (e.g. 32 B), and merging moves to the host (`MemRdFill`, Table C-8), i.e. a host-side read-modify-write. Two cases:
   - **Single-writer lines** (e.g. the (c) chain's `succ[]` ring, written only by the home server): need only that the device honors byte enables, plus untorn 8-byte words, which rule 1 already assumes. A host-side read-modify-write rewrites only that writer's own words. **Open, low risk:** confirm with the partial-write stress test (Section 5).
   - **Multi-writer lines** (e.g. one flag per node in one line): additionally need the device to apply concurrent partial writes from different hosts without losing any (an atomic per-line merge), and no host-side read-modify-write (e.g. no host-side encryption on the region). Neither is guaranteed by the spec. **Open:** do not pack until verified on the target device.
   - **On the measured CXL device, packing does not pay** whatever the test shows: partial writes are capped at ~9M/s in total (above). Packing cuts the lines a scan reads but keeps every update a partial write, and the cached mapping recommended there cannot pack multi-writer lines at all (a full-line writeback overwrites the neighbors).
   - Default to one slot per line until verified. If packed, no node may map those lines as cacheable.
4. **Explicit fences** wherever uncached and cached accesses are mixed, and between a store and a subsequent load that must observe ordering (Dekker-style patterns). Do not rely on memory-type ordering alone.
5. **Protected data** is written back when the global lock leaves a node and invalidated when it arrives (by the data layer, §2.1). Support two modes: full writeback/invalidate (conservative) and precise dirty-range hints supplied by the caller (used by the KV store). Write back eagerly, as soon as the lock is known to be leaving the node. *Implemented:* precise mode also covers reads (`cfg.precise_data`, `ncl_acquire_ranges`): the acquirer names every range it will read or write, and only those are invalidated, when the lock arrives and, on a local handoff, if not yet fetched in the node's current run (writing back any part the run dirtied first). Release hints are then mandatory, and an overflowing dirty list is written back early rather than replaced by a whole-range writeback, since lines never fetched in the run may be stale. A lock covering many records (the KV store's stripes) needs this: the conservative mode invalidates the whole range on every transfer (`docs/kv.md`).
6. **Read-mostly metadata** (e.g. the lock-to-home map in the lock-server variant) stays cached and is refreshed only when a reply indicates it is stale (NACK with version). Because different nodes may update different entries concurrently, **each entry gets its own cache line**, and the writer issues a writeback + fence after updating it.
7. **Open:** check whether the device supports hardware coherence (CXL back-invalidation) for any region; if so, metadata could live there.

### 2.1 Data maintenance layer (Decided 2026-11-18)

Writeback and invalidation of protected data are a layer of their own, separate from the lock algorithms (`include/ncl/data.h`, `src/data.c`). Locks do mutual exclusion and ordering only; the data layer decides which lines to invalidate and write back. Applications whose locks do not guard a fixed region (pointer-linked structures, objects moving between locks) need this separation.

**Correctness rule (program free of data races).** Every access to shared data happens while holding a lock that protects it. Then it suffices that (i) before a lock is released to another node, everything written under it since the lock arrived at the node is written back, and (ii) after the lock arrives at a node, everything about to be read or written is invalidated before first use. The releaser's writeback happens before its handoff store, and the acquirer's invalidation after it observed that store, so the acquirer reads the released values. A line must also be invalidated before it is written: writing back a stale copy would overwrite another node's bytes in that line.

**What a lock tells the data layer (the only coupling).**
1. *How it was acquired:* the lock **arrived** at the node (a cross-node acquisition; every acquisition in Phase 1) or was passed by a **local handoff** within the node's current run of ownership (2a, 2b). During a run no other node can write the protected data, and the node's own caches are coherent.
2. *Whether a release is a **departure**:* the lock is leaving the node (every release in Phase 1) or being passed locally. The data layer's departure work runs before the lock's handoff store, and the lock guarantees a fence between the two; it may issue writebacks without their own fence when the lock says so (2b's `-O on`: the flushes overlap the release's metadata reads).

**Policies** (per system, `cfg.precise_data`):
- *Conservative:* each lock has a registered data range (`ncl_lock_set_data`). On arrival invalidate the whole range; on departure write back the whole range or, if every release in the run gave dirty hints, those ranges.
- *Precise:* the acquirer names the ranges it will read or write (`ncl_acquire_ranges`), the releaser the ranges it wrote (mandatory). Named ranges are invalidated on arrival, and on a local handoff unless already fetched in the run (dirty parts written back first). Dirty ranges are written back on departure; an overflowing dirty list is written back early, never replaced by a whole-range writeback (lines not fetched in the run may be stale).

**Lock metadata layout.** Each lock's metadata occupies a block of lines in the meta region and another in the scan region, laid out at a fixed stride. On the CXL host every stride that is a multiple of 512 B costs 4-6% throughput, for every lock algorithm (`docs/cxl_results.md`, "Lock-block stride"); other strides are within ~1%. So a per-lock line count that is a multiple of 8 gets 2 more lines (keeping even counts even, which the dev box prefers); `cfg.raw_layout` turns the rule off for layout experiments.

**Scope of the current policies.** Both track fetched and dirty ranges **per lock** and require the ranges to lie inside the lock's registered range. That is correct when data stays bound to one lock, as in the microbenchmark, the stress test and the KV store. Data that moves between locks (e.g. written under A, passed locally, then published under B) needs **node-wide** dirty tracking: on any departure, write back everything the node dirtied and has not written back. That, ranges outside a registered range, and naming ranges during the critical section (for pointer chasing) are **Future work**.

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
- **No separate handoff flag (Decided, then revised by measurement).** Waiters poll `owner` directly. Split out per-waiter flags only if measurements show a hotspot on the owner line or a handoff needs a larger payload. *The CXL host shows that hotspot:* with T=1 every handoff waits behind the N-1 waiters' queued owner polls (~420 ns each), so 2b throughput falls as 1/N (`docs/cxl_results.md`). Option `--wait own` (`cfg.handoff_wait`): the announce word holds a per-node sequence number, and the releaser stores `owner = j`, fences, then copies j's sequence into word 1 of j's announce line (the grant). Waiters poll their own line and re-read `owner` after 16, 32, 64, ... polls (a missed announce leaves it FREE; O(log wait) owner loads per wait). A waiter that learns of its handoff from `owner` waits for the grant before clearing its announce. That makes the announce line a two-writer line whose stores are causally ordered (announce → releaser reads it → grant → waiter reads it → clear), so whole-line writebacks never overlap. Measured on the CXL host it removes the 1/N collapse (2.4-6x at 32 nodes for tournament, hybrid and AK), so it is the **default**; `--wait owner` keeps the original. Not model-checked.
- **Shorter handoff path (default, measured, not model-checked):** `--ho grant` makes `owner` FREE/HELD only and the grant store alone the handoff (no `owner` store or fence on the critical path); `--ho clear` clears the announce at the holder's release instead of before its critical section; `--ho skip` has a waiter read `owner` right after announcing and enter the gate only if it is FREE (valid because releasers never take the gate, so a "held" read outside it is as good as one inside; adaptive: only if the node's last acquisition of the lock waited; needs announce-before). **Default on** (all options that apply) since the CXL measurements: `skip` removes the gate as a second serial resource and brings every gate algorithm to Phase 1 throughput at saturation; `--ho none` keeps the original rule above (`docs/handoff.md`).
- `pick_waiter` scans announcements round-robin starting after the releasing node, for fairness.

### 3.4 Phase 1 mode (no handoff)

For Phase 1 of the evaluation, the base algorithm **is** held across the critical section (no gate, no owner field): `acquire = base_lock()`, `release = base_unlock()`. Keep this as a build/runtime mode.

### 3.5 Open items in the handoff design

- **Announce before entering the gate** (instead of inside it) would shorten gate hold time and is safe given the "owner == me means handoff" rule. Evaluate both; current baseline announces inside.
- Epoch bits in the owner word: useful for debugging and data-writeback versioning; bit allocation TBD.
- *Measured (Phase 2b, `docs/handoff.md`):* both placements are implemented (`-G inside|before`), plus a third, **lazy** (announce inside the gate only after reading a held owner; removed 2026-11-17). **Default: `before`.** Lazy (the earlier default) collapses when the gate is slow relative to the handoff: waiters pile up in the gate unannounced, releasers store FREE, and 2b degrades to Phase 1 (Lamport-2b on cached-flush: 1.03M with `before` vs 0.52M, fairness 0.76, with lazy). A releaser's `pick_waiter` may also probe a single slot when its own acquisition was uncontended, instead of scanning (`-X adaptive`, default; a miss stores FREE and the waiter recovers through the gate). Always probing (`-X probe`, removed 2026-11-17) misses waiters under contention. Neither changes the safety argument above. `before` cuts missed-announcement recoveries ~100x with otherwise similar latency and capacity. The uncontended cost is ≈ +4 round trips, not +1 (announce store, owner load, owner store, announce clear, plus fences), and the release scan of `announce[]` adds up to N−1 loads.

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
- **Fast-path winner's reset rule (added after measurement; needed for safety as well as fairness; model-check):** with an unconditional `door = OPEN`, the winner's node re-wins the splitter immediately (its next local thread, or the same thread in a closed loop), while deflected nodes wait on the slow path and never retry the splitter. The top Peterson lock alternates FAST and SLOW, so one node gets about 50% of all acquisitions (measured 4:2:1:1 at 4 nodes with local handoff). The winner therefore reopens only if every other `present[j]` is 0; otherwise the last slow-path releaser reopens it via the normal reset rule. Cost: N−1 uncached loads on an uncontended fast release (+450 ns sojourn at N=8 on uc).
- **Reset race (found 2026-11-11; why the strict rule is required):** the slow releaser's reset is check-then-act: read `door == CLOSED`, scan `present[]`, store `door = OPEN`. If a fast winner reopens the door inside that window, a new node can win (closing the door) after the scan, and the resetter's stale `door = OPEN` then admits a second winner while the first holds the fast path. The strict rule prevents this: the resetter's own `present[i]` stays set for the whole window, and a winner reopens only if it sees every other `present[j] == 0` (a Dekker pair on `present[]`: each side sets its flag, fences, then reads the other's). The earlier default (hint: probe one remembered slot, reopen on a miss) and the original unconditional reopen both allow the race; it was reproduced by stretching the window (`ncl_stress --reset-delay`; hint failed 8 of 10 runs on uc) and is rare without it, but a preempted releaser opens the same window. Neither is selectable any more; the hint rule survives only as the stress test's `--debug-unsafe-reopen`, to show the race is detected.
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
- **Alternative reset (implemented as `-a ak`, not model-checked):** Anderson & Kim's bounded fast path (Figure 4 of the paper, https://cs.unc.edu/~anderson/papers/dc00.pdf) on the same tournament and top lock. It replaces the splitter and `present[]` with `X`, `Y = (free, indx)` in one word, `Reset`, `Infast`, `Name_Taken[N]` and `Obstacle[N]`: a process that takes the fast path is renamed `Y.indx`, so the reopener checks one `Name_Taken` entry and one `Obstacle` entry (O(1)) instead of scanning `present[]`. The uncontended path costs one more store–fence–load step (`Name_Taken` vs `Reset`). As published (`-F always`) the fast winner reopens unconditionally, which measured fairness 0.31 at N=8; the default (`-F fair`) is a fairness variant (reopen only if the top lock's SLOW flag is clear; every slow-path releaser runs the reset). Measurements: `docs/hybrid.md`, "Anderson-Kim fast path".

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

**Fast path (node i):** `present[i] = 1`; run the splitter as in (d); if it wins (`stop`), acquire the top lock's `FAST` side and enter. The server is not involved. On release: unlock the `FAST` side, reopen the door under (d)'s strict rule (only if every other `present[j]`, including the server's marker below, is 0), clear `present[i]`.

**Slow path (node i):** if the splitter deflects it, send a request to the lock's home server and wait for a grant in its grant slot (Section 4c). On release, clear `present[i]`, fence, then send the release message to the server. (If `present[i]` were cleared after the release message, the server's reset check could still see it set and leave the fast path closed until the next contended episode.)

**Variant (implemented, `-a aksrv`, not model-checked):** Anderson & Kim's fast path (see §4d, "Alternative reset") instead of the splitter and `present[]`. The fast winner reopens only if the top lock's SLOW flag is clear (the server has nothing queued); the home server runs AK's reset (statements 29–37, with `Obstacle[p]` cleared by each SLOW 2 grantee before its REL) when its queue drains, while still holding the SLOW side. Constant-cost fast release and reset; measured equal capacity, flat uncontended cost in N, and much better latency after bursts (`docs/hybrid.md`, "AK fast path in the fast-path server").

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
        # splitter reset: reopen the fast path only if no node can be on it.
        # present[N] is the server's marker, set for the whole check, so a
        # fast winner's strict scan never reopens inside it (reset race, §4d)
        present[N] = 1; fence
        if door == CLOSED and all(present[j] == 0 for j < N):
            door = OPEN
        present[N] = 0
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

### (f) Reactive Lamport/tournament lock (sixth variant, measured, not model-checked)

Motivation (`docs/cxl_results.md`, "KV store"): with several threads per node, cold locks are fastest on Lamport and locks hot across nodes need the tournament; a per-lock oracle beat the best uniform choice by 31-36% on Zipfian keys. The reactive lock finds the per-lock choice online, after Lim and Agarwal's reactive locks (ASPLOS 1994): only a holder may change the protocol.

- **State per lock:** a Lamport lock, a tournament, and an uncached `mode` word (0 = Lamport, initial; 1 = tournament) written only by a holder. While the tournament is active, Lamport's `y` holds a reserved value INVALID. Node-local, per lock: the mode last seen and a 64-bit history of whether each of this node's global acquisitions met another node.
- **Invariant (I):** the Lamport lock is acquirable only while `mode == 0`. Switching to the tournament is done by a Lamport holder, which stores `mode = 1` while still holding Lamport and releases it with `y = INVALID`; switching back by a tournament holder, which first acquires the INVALID Lamport lock (treating INVALID as free) and releases it normally later.
- **Acquire:** in Lamport mode, run Lamport. Lamport gives up (holding nothing) when it sees INVALID, and a successful acquisition needs **no `mode` read** (I). In tournament mode, take the tournament, reading `mode` in the root's entry check after the root's flag and turn loads (load order, `ncl_fence_ll`: a compiler barrier on x86, which keeps loads in order, an acquire fence elsewhere; on cached-flush the three loads share one round trip); on a mismatch release it and retry in the other mode. The value is fresh enough because a switcher stores `mode` before releasing the tournament, and the root check enters only after observing that release (flag clear) or a later announcement that causally follows it (turn).
- **Late `y` stores:** a node can read `y == 0`, stall, and store `y` after another node has switched and released `y = INVALID`, overwriting it. That node always takes Lamport's slow path (the switcher wrote `x` after it), so a **slow-path success re-reads `mode`**; if the tournament is active, it restores INVALID (it holds the Lamport lock by Lamport's rules) and moves to the tournament. A fast-path success needs `x` unchanged since its own store, so no switch happened in between. Measured: such late stores occur thousands of times per stress run.
- **Switch policy** (by the global holder, right after acquiring; `--react U:D[:W]`, default 8:4:64): to the tournament when at least U of the node's last 16 global acquisitions of the lock met another node; back to Lamport when at most D of the last 64 did, after at least W acquisitions by this node in tournament mode. A node that finds the lock in another mode than at its previous acquisition (another node switched) restarts its history and dwell count. The switcher takes the new mode's lock too and releases both at its global release. The two modes report contention differently (the same load gives fewer tournament waits than Lamport slow paths), so a symmetric rule makes warm locks flip; a long dwell W lets U be low (promote eagerly) without that.
- **Contention signal** = another node competed: Lamport's slow path or a retry, or a tournament level whose opposite side made us wait. Contention among a node's own threads (the submutex) does not count, so a lock hot within one node stays on Lamport.
- **Safety:** a node is inside either holding Lamport (so `mode == 0`, by I) or holding the tournament having read `mode == 1` after acquiring it. A switch needs the current mode's lock and keeps the new one until the switcher's release; if P (Lamport) and Q (tournament) were inside at once, the switch between their checks was made by one of them while holding the other's lock: contradiction. **Liveness:** a stale node gives up the wrong lock without waiting while holding it.
- **Layout:** the block is [x][y][mode][tree] (default; `--react-layout mode-first` for the old order), and like every lock block it gets 2 extra lines when its line count is a multiple of 8 (below). Measured on CXL: [mode][x][y] cost 2 points, and the 48-line block's 3 KB stride 4-6% (`docs/cxl_results.md`, "Lock-block stride").
- **Cost:** none extra on a Lamport fast-path acquisition; one `mode` load on a Lamport slow-path success; on a tournament acquisition the `mode` load joins the root check's batch (no extra round trip on cached-flush; one more serialized load on uc).
- **Tested:** stress in every mode and layout at 8:4, with a switch on every acquisition (`--debug-react 8`), that plus a widened late-store window (`--lamport-delay`), and 0:64 with a widened mode-check window (`--mode-delay`). Each removed step fails: switching without taking the tournament (mutex violations), no `mode` re-check, no INVALID release, no slow-path re-check, and loading `mode` before the root's flag and turn with a gap between them (hangs) (ctests `stress_reactive_*`, `reactive_*_detects_*`). Without an artificial gap the reversed order is not observable: the switcher stores `mode` a whole critical section before its release. **Not model-checked.**
- **Measured, first version** (consecutive-event trigger, `mode` read on every acquisition; `docs/cxl_results.md`): 14-30% below the per-lock oracle on CXL; the version above removes both causes.

### (g) Biased tournaments (Open, not implemented, not model-checked)

Goal: a node that held a lock recently re-acquires it through fewer arbitration levels, while cold nodes keep a fair arbitration point. Every design changes the arbitration structure only while holding the lock, with the departing owner's cooperation. Reshaping the tree while nodes are mid-path is rejected: it needs quiescence and the same cooperative revocation anyway. Baseline is Phase 2a (cohort passing already covers a dominant node with waiting threads), so the bias pays only when a node re-acquires after its local queue drained or after the cap `-K` forced a release. Measured upper bound on cf: a Peterson level costs ~0.5-2 us (tournament 8 -> 10 us from 4 to 5 levels, `docs/cxl_results.md`); compare also with Lamport, AK and the reactive lock (§4f).

**Shared rules.** Single writer per line, one line per variable. Batch invalidations and loads; fence only where the algorithm orders. Recency is tracked node-locally; shared metadata is written only on admission, eviction or drop, never on every acquisition. Retained or owned shared state needs a node-side lease, because without cross-node atomics no other node can revoke it. The lease state is node-local (`PARKED`/`HELD`/`RELEASING`-style word changed with coherent CAS, as in §7 "Lease parking"), so a node's sweeper never drops state while one of its threads re-enters on it. Where a per-lock submutex is on the path (g1), the submutex is that lease and no extra state word is needed.

**(g1) Prefix retention** on the balanced Peterson tournament: `-a tournament --retain` (implemented 2026-11-24, `src/base.c`, sweeper in `src/lock.c`; Phase 1 and 2a, needs the submutex; NOT model-checked).
- Release: clear the root first, then (after the release check) levels top-down with a store-store fence between levels (as §4b), keeping a contiguous leaf-side prefix 1..j. Next acquire starts at level j+1. The retained depth j is a node-local per-lock field `kept_levels` (0 = nothing kept), next to `global_held` in the lock's node-local state.
- **Contiguity is a safety rule.** Keep only the levels below the lowest level with a waiter: dropping level i while keeping a higher level lets the sibling that wins level i climb onto a side still held above (two nodes on one side).
- The root is never retained (retaining it is holding the lock).
- **Waiter checks.** A node blocked on a retained level waits until the retainer drops; only the retainer may write its flags. Three places look for such waiters, by reading the opposing flags of the retained levels (one batch) and dropping every retained level at and above the lowest one with a waiter:
  - *release check*: at the global release, after clearing the root, one batched read of the opposing flags of levels 1..depth-1 (one load round; `--retain-check release`);
  - *acquire check*: at the next acquisition, in the batch the node already reads at level j+1 (free on cf; `--retain-check acquire`). On a waiter at retained level i the node drops levels j..i top-down and re-enters level i by plain Peterson. Default `both`; measure each;
  - *sweeper*: a per-node thread for idle retainers, below.
  A waiter whose flag became visible after the last check is found by the next one; the sweeper's poll period bounds that wait when the node is idle.
- **The sweeper is required for liveness, not only for idle nodes** (found in stress, 2026-11-24). The checks cover only the lock being acquired or released: node A keeps levels of lock X and waits for lock Y on node B's kept levels of Y, while B waits for X on A's kept levels: a deadlock that no check breaks. The sweeper breaks it, since A's submutex for X is free while A works on Y. `--retain-poll off` runs without a sweeper and must hang with several locks (ctest `retain_detects_no_sweeper_deadlock`).
- **The lease is the submutex.** `kept_levels` and the node's flags in the lock's tree are read or written only by the holder of that lock's node-local submutex. Workers already hold it while in the global protocol. Without this rule the sweeper races a re-entering thread: thread T reads `kept_levels = j` and starts at level j+1 while the sweeper clears level i <= j; the sibling waiting at i wins it, climbs, and meets T on the same side of level j+1 (two nodes inside a Peterson side). In the other order, T climbs from a level it no longer holds.
- **Sweeper.** One thread per node, mostly sleeping (poll period P, `--retain-poll`, default 10 us; 0 = `sched_yield`), started at `ncl_node_attach`. `ncl_node_detach` stops it and drops every kept level before the detach barrier. At most R locks keep levels per node (`--retain-max`, default no limit): a release keeps nothing while R locks are kept (simpler than evicting the least recently acquired; bounds the sweeper's reads).
  - Poll: scan the node's locks for `kept_levels > 0` (node-local, read unlocked) and read the opposing flags of their kept levels in batches of up to 64 lines, without any claim. Stale depths only make it look at the wrong lines, never act.
  - Act, on a waiter or an expired lease (lease L since the levels were kept, `--retain-lease`, default 1 ms; 0 = drop at the next poll): **try-lock** the lock's submutex (`ncl_sub_trylock`, CAS of the MCS tail from empty to the sweeper's qnode). On success: re-read `kept_levels` (a worker may have changed it since the poll), drop every kept level top-down (store-store fences), set `kept_levels = 0`, then a **plain submutex unlock** that never passes global ownership (`global_held` stays 0; the next thread knows it holds nothing globally). On failure: skip the lock until the next poll; a worker is in or entering the protocol and its acquire or release check will find the waiter.
  - Dropping retained levels is always a valid release of levels the node holds; the submutex only keeps a drop from overlapping a climb. In 2a a node passing the lock locally still releases globally at the cap `-K`, and that release check sees the waiter, so a busy node does not stall it.
  - Cost: the sweeper holds the submutex only for the drop (about one write round per level); a worker arriving then queues behind it. Polls do not touch the submutex. Sweep P and L; report open-loop p99 of cold nodes (the (e) poll-backoff trade-off).
- **Adaptive keep policy (`--retain-penalty P`, default 16; 0 = keep at every release).** Keeping levels for an idle node is expensive for anyone who needs them before the node returns: the waiter waits for that node's sweeper (tens of us with nanosleep) instead of climbing a free level (~1.3 us). Keeping at every release (P = 0) on the dev box, 8x2, many locks, no reuse: L=16 0.40M vs plain 8.5M, L=256 0.59M vs 10.9M; a busy (yielding) sweeper only reaches 1.5M. So a node keeps levels only where that has paid: per lock, node-local, under the submutex, a credit that a hit (an acquisition starting above level 1 whose acquire check found no waiter) raises by 1 (up to 8) and a drop forced by a waiter (acquire check or sweeper; not the release check, whose waiter waits for the lock anyway, nor lease expiry) lowers by the current penalty; a release keeps levels only while the credit is >= 0, and each release that keeps nothing raises it by 1, so it probes again after that many releases. The penalty starts at P, doubles with each forced drop up to 4096, and returns to P when the credit reaches 8. A fixed penalty was not enough (P=64: L=256 6.7M; the sweepers themselves cost 3%). Dev box, 8x2, cf on DRAM, closed loop: L=16 8.26M vs plain 8.23M; L=256 10.2M vs 10.6M; {0} alone 0.89M vs 0.71M; {0,4} 1.42M vs 1.27M; {0,1} and all 8 nodes at L=1 within 3% (every release finds a waiter). Counters `ret_policy_skips`; CSV `ret_policy_skips_per_acq`.
- **Where it can win.** Contended handoff at L=1 with all nodes: nothing (every level has a waiter). Many locks with no reuse: nothing (reactive runs Lamport there). A lock shared by a few nodes, which reactive sends to the tournament: today each climbs log2 N levels though only the level where their paths meet is contended; with g1 a node pays only from the meeting level to the root. Contenders that meet high in the tree (node ids far apart) gain most; contenders that meet at a leaf gain nothing (g2's case). Measured uncontended cost per level on CXL cf ~1.8 us (tournament 7.7 us per acquisition at 16x1 L=256, 9.5 us at 32x1; Lamport 4.1 us); under contention part of the climb overlaps the other node's critical section.
- **Go/no-go (sweep phase `g1bound`, `ncl_bench --active LIST`).** Only a subset of the N nodes acquires; the others take part in the tree but never acquire. g1's ceiling is the tournament over the levels it would still climb: one node {0} at N vs `-n 2` with node 0 alone (root only); {0, N/2} vs `-n 2`; {0, N/4, N/2, 3N/4} vs `-n 4`. {0,1} and {0-3} meet at the leaves (g1 keeps nothing; they measure what g2 could save). Lamport and reactive at the same subsets are the competitors. Phase 1 and 2a, L=1, think time 0 / 2 / 10 us. If the bound is under ~10% above the plain tournament, or the tournament's bound stays below reactive, where it matters, stop before building the sweeper. Dev box (cf on DRAM, T=1, think 0): {0,8} at N=16 1.15M vs bound 1.50M; {0} 0.58M vs 1.14M; {0,1} 0.41M (the waiter's climb of the 3 upper levels is on the handoff's critical path). **Measured on CXL (2026-11-24, `docs/cxl_results.md`, "Prefix-retention go/no-go"): go.** The bound beats the best existing lock by >= 10% in 36 of 54 cells, 8-106% (median 21%) in 2a at think 2 us; little where cohort passing hides the global path (2a, think 0) and none for 4 contenders at 16x3 Phase 1. One uncontended level ~1.3 us vs Lamport's fast path 2.2 us, so the release check must stay cheap.
- **Tests and counters.** Stress on emu (ctests `stress_retain_*`, keeping at every release so the retention paths run on each acquisition, plus the default policy): Phase 1 and 2a, a yielding sweeper with zero lease and a widened window between reading `kept_levels` and climbing (`--retain-delay`), each check alone, uneven trees, mixed per-lock algorithms, cached-flush and uc. Must FAIL: the sweeper dropping without the submutex (`--debug-retain 1`), a non-contiguous drop (`--debug-retain 2`: the release check drops only the waiter's level), and no sweeper with several locks (`--retain-poll off`, the cross-lock deadlock, caught by `--timeout`). Counters (`ret_*` in `ncl_base_stats`): hits and levels skipped, acquire-, release-, sweeper- and lease drops, cap and policy skips, sweeper try-lock failures and polls. Sweep phase `g1`: retention vs the plain tournament, reactive and the CPU-matched bounds at the g1bound subsets, plus every node at L = 1, 16, 256. **Measured on CXL (2026-11-24 22:32, `docs/cxl_results.md`, "Prefix retention"):** one node 1.82x the tournament and 1.04x reactive (median), {0, N/2} 1.74x and 1.48x, the best lock (within 1%) in every far2 cell; every node at L = 16, 256 within 0-4% (keeping at every release: -71 to -84%). Two losses, both from the policy: the keep decision is all-or-nothing, so four far contenders (who should keep depth-2 levels) are charged at level depth-1 and stop keeping (16x1 P1 think 2 us: tournament level vs 1.14x for keep-always), and the release check runs where nothing can be kept (near2 -4 to -12%, all nodes at L=1 up to -10%). **Open: learned keep height** — per lock, cap the kept prefix below the lowest level where a waiter forced a drop or the release check found one; skip the release check while the cap is 0, re-probing occasionally.

**(g2) Hot-slot subtree under an asymmetric root.** Root Peterson: one side a k-slot Peterson subtree, the other the full cold tree (the §4d/§4e top-lock shape). A node claims a free slot only while holding the lock (admit on a second recent hit), and evicts itself after clearing its hot-tree state; one path per acquisition. Hot flags are cleared at every release, so an idle slot owner delays only admissions, never acquisitions. Root alternation guarantees the cold side every other acquisition under contention.

**(g3) n-ary root: bakery with the choosing flag merged into the ticket.** The root becomes an n-way mutex over the two subtree winners and r recent-holder seats (leased as in g2). Seat i owns one word `slot[i] = {choosing (bit 63), ticket (bits 0-62)}`, one line per slot, single writer:

```
acquire(i):
  slot[i] = {choosing=1, ticket=0}; writeback; fence     # round 1
  invalidate all slot[j]; fence; read all                # round 2 (one batch)
  t = 1 + max ticket seen
  slot[i] = {choosing=0, ticket=t}; writeback; fence     # round 3
  invalidate all slot[j]; fence; read all                # round 4 (one batch)
  for each j != i: wait while slot[j].choosing
                   or (slot[j].ticket != 0 and (slot[j].ticket, j) < (t, i))
release(i):
  slot[i] = {0, 0}; writeback
```
- The fence before each batch is a full store-load fence (`ncl_fence`); an `sfence` alone does not order the slot store before the loads. On cf this is `ncl_uc_store` + `ncl_uc_load_many`.
- The single store in round 3 publishes the ticket and clears `choosing` together, which classic bakery needs in that order.
- Seeing `choosing` again after `choosing == 0` was seen is harmless: j's new doorway started after `t` was visible, so it draws a larger ticket. At most one extra doorway of delay per j. `--bakery-wait classic` (implemented, not model-checked) removes that delay: a blocker j once read with `choosing == 0` after our round-3 fence is waited on by its ticket only, and is dropped when seen choosing again (a choosing slot holds ticket 0). Classic is the default since 2026-11-23 (measured below); `--bakery-wait strict` keeps the rule above. Clearing j from the round-2 read, made before `t` was visible, is unsafe (`--debug-bakery 8` FAILS). Counter `bak_classic_drops`; sweep phases `bkwait`, `kvbkwait`. Measured on CXL (`docs/cxl_results.md`, "Bakery wait rule"): flat bakery 1.4-2.1x faster in Phase 1 at L=1, K=4 0-11%, K=2 unchanged at N >= 8; the tournament still leads by 22-27% under contention.
- Uncontended: 4 rounds; FCFS at the root. Tickets grow only while the root stays contended and return to 0 when it drains.
- A seat changes owner only while the lock is held, after the departing owner's `{0, 0}` is written back.

**(g4) Composition.** Retention below the hot or n-ary node; hot-slot owners may retain inside the hot subtree.

**Cost model behind g5-g9 (cf).** Width is nearly free and serialized rounds are not: a batched read of K lines costs about one round (`docs/backends.md`: 7 lines 112 ns batched vs 531 ns serial), and on the CXL host 7 stores + `clwb` + one `mfence` take 401 ns. A binary Peterson level is 2 serialized write rounds (flag, then turn) + 1 load round; a merged-bakery node is 2 write + 2 load rounds at any width. On uc, loads are serial (~500 ns each), width costs K x L, and none of this holds. **Measured (`docs/cxl_results.md`, "Flat merged bakery"): width is cheap for latency but not for bandwidth.** A flat bakery reads 2(N-1) lines per acquisition, and its throughput plateaus at the device's read ceiling (230-275M lines/s): it beats Lamport at N <= 8 and loses 20-60% at 16-32 nodes. Count lines read per acquisition, not only rounds.

**(g5) Flatten before biasing: `-a bakery` (implemented, not model-checked).** One merged bakery (g3) over all N participants: ~4 rounds at any N, FCFS, single-writer lines only. N=16: binary tournament ~12 rounds, 4-ary bakery tree ~8, flat ~4. Bias saves levels, and the flat bakery has one; it is the baseline every biased design must beat on cf. Slots are in the scan region (one line each).

**(g5b) k-ary bakery tree: `--bakery-arity K` (implemented, not model-checked).** The flat bakery's reads cap it at the device's read bandwidth (above); a tree of K-way merged-bakery nodes reads 2(w-1) lines per node of width w at the cost of 4 rounds per level. Level l has cnt[l] seats (cnt[0] = P, cnt[l+1] = ceil(cnt[l] / K)), grouped K per node, each node as wide as its existing children; climbed leaf to root, released **root to leaf** with a store-store fence between levels (a seat at a higher node is shared over time by its whole subtree: releasing a lower node first lets its next winner write a seat still held above; `--debug-bakery 4` with `--bakery-delay` FAILS). Lines read per uncontended acquisition, N=16 / 32: flat 30 / 62, K=8 16 / 20, K=4 12 / 14, K=2 8 / 10; rounds 4 / 4, 8 / 8, 8 / 12, 16 / 20. Each node is FCFS, so the tree is starvation-free. Sweep phases `bktree`, `kvbktree` (Phase 1 and 2a). Measured (`docs/cxl_results.md`, "k-ary bakery tree"): each level costs ~1.9 us uncontended; K=2 reaches 78-81% of the tournament under contention at 8-32 nodes (flat 21-36%), and K=8 lifts the 32-node bandwidth ceiling (7.5M vs flat 4.5M, tournament 6.0M, Lamport 11.0M). No arity beats the tournament under contention or Lamport uncontended.

**(g6) Predecessor polling.** After round 4 the waiter keeps the set of blockers. It polls only its predecessor p (the non-choosing blocker with the largest (ticket, id) below its own; a choosing blocker if there is none) until `slot[p]` differs from the value seen (not until it is 0: p may release and re-enter, so 0 can be too brief to see), then re-reads the remaining blockers in one batch and drops those that no longer block. A blocker, once seen not blocking, is never re-checked (classic bakery's per-j waits; one read with `choosing == 0` and a non-blocking ticket satisfies both of its waits at once). Under FCFS the batch usually empties, so each waiter polls one line: a near-queue handoff instead of every waiter reading every slot (the 2b owner-line hot spot, `docs/cxl_results.md`). A periodic full re-read bounds the cost of a predecessor that keeps re-entering. Not model-checked.

**(g7) Adaptive n-ary root, switched at release.** Per lock, the root runs either a Peterson node between the two subtree winners (mode P) or a bakery node with recent-holder seats (mode B), chosen by a `mode` word written only by a holder. Both root algorithms re-read `mode` after acquiring (in the batch they already do, after the entry-check loads as in §4f); on a mismatch the node releases and retries in the other mode. The **switch happens at release**, after the critical section, and the switcher never takes the other mode's lock:
```
critical section
data writeback; fence
mode = new; writeback; fence_ss     # only on a switch: the release point for new-mode arrivals
release the old mode's lock         # as usual
```
- *New-mode arrivals* may acquire the new lock at any time; reading `mode == new` orders their entry after the `mode` store, so after the switcher's critical section and data writeback. Reading the old value, they retry.
- *Old-mode arrivals* acquire the old lock only after the switcher releases it, so their post-acquire `mode` read sees the new value; they release and retry.
- Invariant: the `mode` store is a release point and follows the switcher's critical section. Requirements: data writeback and fence before the `mode` store; the store-store fence between it and the old release; both algorithms re-read `mode` after acquiring (Lamport's fast path does not, hence §4f's INVALID `y` and switch-with-both-locks).
- Overhead: 0 extra rounds in mode P (one more line in an existing batch); in mode B, paths through the root pay bakery's second load round. The switch costs one write round, at release only. Policy: node-local history as in §4f.

**(g8) Permanent seats.** Since width is cheap, every node owns a seat at the n-ary root permanently (N + 2 slots), so each slot has one writer forever: no admission or eviction writes, no leases, no sweeper, no seat hand-over rule. A node chooses per acquisition, from node-local history only, whether to use its seat or climb the tree. Zero slots cost nothing in the scan. Caveat: if the seat is always faster, the tree survives only to cut root polling under heavy contention, which g6 already reduces; then the choice depends on load, not recency, and the design is no longer a bias. Measure whether the tree is still worth it.

**(g9) Single-writer Peterson levels (speculative).** Kessels' variant splits `turn` into one bit per side; merging that bit into the side's flag word might cut a level to one serialized write. Whether reading the other side's bit before our flag is visible stays safe needs model checking. Pursue only if the tree survives g5.

**Overhead of an adaptive n-ary node over a plain tournament:** mode discovery 0 rounds (g7), seat lookup 0 (node-local, g8), width a few ns per line (cf), bakery's second load round (inherent; mode B only, paths through that node). A bakery node pays off when it replaces at least one Peterson level (2 write + 1 load rounds).

**Order of work:** g5 (`-a bakery`) with g6, measured on CXL cf against the tournament, Lamport and reactive; then g7 + g8, or g1 alone if the flat bakery loses under contention. Done for the bakery line (g3/g5/g5b/g6, `docs/cxl_results.md`): no bakery variant beats the tournament under contention or Lamport uncontended, so g7 and g8 are not pursued; next is g1, after its go/no-go. g2 if clustering hurts g1 under Zipfian skew. Evaluate in Phase 1 and 2a before 2b, where the tournament is only the gate.

**Path counters:** retained-level hits (and levels skipped), release-time drops, lease drops, hot-node fast and slow entries, admissions, evictions, yields.

**Stress negative tests (emu, must FAIL):** non-contiguous drop; no release check plus no sweeper (hang, `--timeout`); bakery without the `choosing` store, or ignoring `choosing` in the wait (`ncl_stress --debug-bakery 1|2` with `--bakery-delay` widening the doorway). A missing round-2 fence is a store-load reordering the emulator does not model (x86 TSO underneath); it needs model checking or the target device.

## 5. Correctness verification

- **Model checking** (e.g. GenMC, or TLA+ with an explicit model of writeback and invalidation): base algorithms under the memory rules; gate + owner field + announcements; hybrid (splitter + tournament + top Peterson, including the splitter-reset rule and its ordering); lock-server queues, grant slots, NACK/migration; (e) server ownership of the top lock's slow side and its splitter reset.
- **Runtime stress tests:** each lock protects a counter and a checksum over shared data; inject random delays; check invariants after millions of acquisitions. Run per phase.
- **Partial-write stress test:** gates dense packing (Section 2 rule 3). Two modes:
  - *Multi-writer:* nodes repeatedly write their own bytes or words within a shared uncached line while checking the other nodes' values.
  - *Single-writer:* one node writes sequence-tagged 8-byte words across a line while the others read and check for torn words and disturbed neighbors. This also checks the untorn-word assumption (Section 1) that rule 1 relies on.
  - Meaningful only on the target device and host. On this single-host testbed, the DRAM controller does the merge. The CXL host (one host, CXL memory on node 2) is a real target for its one-host part: 8-byte uncached stores there do reach the device as a distinct, rate-capped kind of write (Section 2), so the test exercises the device's partial-write handling from many cores of one host. Writers on different hosts (needed for the multi-writer merge question) still need a multi-headed device.
  - The fixed aggregate rate (~9M/s, ~110 ns per write if served one at a time) hints at a serialized read-modify-write at the device or host. A serialized merge would be safe for multi-writer lines, while an unserialized one loses updates. The rate alone cannot tell which; only the multi-writer mode can. Since packing does not pay on that device (Section 2 rule 3), the test there mainly checks the untorn-word assumption (Section 1) that rule 1 relies on.
  - *Full-line mode (needed for the cached mapping recommended on CXL, Section 2):* every hot-metadata store is then a full-line writeback. Check that a reader never sees a torn 8-byte word while another core writes back the line, and stress the one two-writer line (announce + grant): a writeback of a stale copy must never land after the other writer's word. **Emulated (implemented):** the emulator writes back a node's whole private copy of a cached metadata line (its default; `--emu-word-wb` restores the old word-only model), and `ncl_stress` checks at every 2b global acquisition that the node's announce word is still 0 (`stale_announce`). `--grant-delay` widens the window in which a waiter learns of its handoff from `owner` before its grant arrives. The real protocol passes; with `--debug-no-grant-wait` (clear the announce without waiting for that grant) the releaser's late full-line writeback resurrects the announce and the test fails (ctests `emu_line_wb_*`). The word-only model passes the broken protocol, which is why full-line writeback is the default. The torn-word and multi-host parts still need the target device.

## 6. Evaluation plan

### 6.1 Workloads (Decided)

- **SPLASH** and **PARSEC** (PARSEC 3.0 bundles SPLASH-2/2x), run unmodified via LiTL-style pthread interposition (https://github.com/multicore-locks/litl). Start with the lock-sensitive subset identified in Guiroux et al., "Multicore Locks: The Case Is Not Closed Yet" (USENIX ATC'16).
- **Key-value store driven by YCSB.** This one is modifiable, so it provides precise dirty-range hints. *Implemented:* `ncl_kv` (`docs/kv.md`): records striped over L locks, static key index (no inserts: workloads A/B/C/F), uniform or scrambled-Zipfian keys, key-to-node affinity, per-lock algorithm map (`--algmap top:H`), precise or full data maintenance, open or closed loop, every read and the final state verified.
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
- An **atomic-based lock** (ticket/MCS in device memory), if cross-host atomics are available. *Implemented:* `-a pthread`, a process-shared futex mutex on `-b coherent` (Phase 1, with or without the submutex), as the "coherent atomics available" reference point; ticket/MCS not yet.
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
