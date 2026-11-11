# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Status

`design.md` is the authoritative spec. It marks decisions as **Decided** (do not
change without reason), **Open** (must be settled before/while implementing), and
Future Work (out of scope for now).

Implemented so far (Phase 1 scaffold, C11 + CMake):
- `include/ncl/mem.h`, `src/mem.c`: memory-access layer. Optional injected device latency.
  Backends:
  - `uc`: the only backend with **really uncached** hot metadata. It comes from
    `kmod/ncl_uc.ko` and is verified at startup via the live PTE and load timing (see
    `kmod/README.md`). Data is cached and uses clflushopt.
  - `cached-flush`: metadata is ordinary cached memory with a flush around every access.
    It is a cost stand-in, NOT uncached.
  - `emu`: software non-coherence. Each node's data view is private, and writeback and
    invalidate copy whole lines to and from the device. Metadata is physically WB but
    behaves as uncached. Use it to check correctness.
  - `coherent`: baseline.
- `include/ncl/submutex.h`: MCS submutex with local-waiter detection.
- `include/ncl/base.h`, `src/base.c`: Lamport (a), tournament (b), hybrid (d) per design §4d
  (race/door/present splitter, Aspnes reset), plus splitter and top-lock primitives shared
  with (e).
- `src/server.c`: lock servers (c) and fast-path server (e), Phase 1 (fixed home =
  hash % N). Message and reply bit layouts are in `src/server.h`. Nodes must call
  `ncl_node_detach()` (an all-nodes barrier) before servers stop. (d) and (e) are **not
  model-checked**, including two fairness fixes added after measurement (fast winner reopens
  only if the node last seen present has left, `-F hint`, default; or only if alone, `-F scan`; the (e) server yields the SLOW side to a waiting fast winner). See
  design.md §4d/§4e. Implementation notes and measurements: `docs/hybrid.md`.
- Waits that depend on another thread (submutex handoff, server grants) spin and then
  `sched_yield()`, so oversubscribed runs make progress.
- `include/ncl/lock.h`, `src/lock.c`: two-level lock. Modes: Phase 1, Phase 2a local handoff,
  and Phase 2b `NCL_MODE_HANDOFF` (`-M handoff`): gate + owner field per design §3.3 for
  (a)/(b)/(d). Defaults: lazy announce (`-G lazy`; §3.5 alternatives `inside`, the spec
  baseline, and `before`) and adaptive `pick_waiter` (`-X adaptive`; `scan` = spec). Not
  model-checked; notes and
  measurements in `docs/handoff.md`. Includes the no-submutex ablation and full or precise data
  writeback.
- `tests/stress.c`; `bench/microbench.c`: closed loop (default) or open loop (`-r RATE`, `-A
  poisson|uniform|onoff:PERIOD_US:FRAC`). Open-loop latency is measured from the scheduled
  time, so there is no coordinated omission. Throughput counts completions in the measured
  window, and arrivals that never started are reported as `unserved` (= saturated).
  `bench/hist.h` is a log-linear histogram (<=6.25% overestimate). `-C`/`-H` give CSV, and
  `scripts/sweep_load.sh` sweeps offered load.
- Phase 2a local handoff: `NCL_MODE_LOCAL` (`-M local`, cap `-K`), for all variants; see
  `docs/local_handoff.md`. `ncl_bench` reports per-node fairness (min/max completions) and
  takes `-B` for the base algorithms' spin backoff cap and `-Y next|sibling` for (c)/(e)
  server placement (sibling = SMT sibling of the node's worker; use it when servers must share
  cores, e.g. N=8 on this 10-core host). Always report fairness with
  throughput: Lamport's throughput at the default backoff comes with severe starvation.
- Not yet implemented: home migration/NACK (2c), partial-write
  stress test (§5), skewed/phased access, LiTL interposition, KV store.

Emulated nodes are forked processes sharing a memfd mapped at a fixed VA
(`MAP_FIXED_NOREPLACE`). Process-private memory is node-local memory.

## Build & test

```
cmake -S . -B build && cmake --build build -j
cd build && ctest -j4                              # stress matrix + emu negative tests; uc skipped w/o driver
./ncl_stress -a hybrid -b emu -n 8 -t 2 -i 200000  # single stress run
./ncl_bench -a lamport -b uc -n 8 -s 2 -c 200          # closed loop
./ncl_bench -a lamport -b uc -n 8 -c 200 -r 500k        # open loop, Poisson
scripts/sweep_load.sh -b uc -n 8 -c 200 > sweep.csv     # latency vs offered load
```
Build and load the kernel module with `cd kmod && make && sudo insmod ncl_uc.ko`; the `uc`
ctests are skipped when it isn't loaded. Never fake uncached memory without saying so: if
the driver is unavailable, say which backend was used.
Under `emu`, `--skip-wb` and `--skip-inv` must make the stress test FAIL. ctest checks
this, to confirm the emulator really models missing cache maintenance.

## What this project is

A family of mutual-exclusion locks for multi-node systems whose shared memory is
**cache-coherent within a node but NOT hardware-coherent across nodes** (target:
up to ~16 nodes sharing a CXL memory device, N kept as a parameter). Plus a phased
plan to evaluate them. Real CXL hardware is not required — an emulated NUMA setup
with explicit flushes / uncached remote memory (optionally with injected latency)
is acceptable.

## Non-negotiable invariants (the whole point of the design)

These constraints are what make the code correct on non-coherent memory. Violating
them silently reintroduces the bugs the design exists to avoid:

- **No cross-node atomics.** Cross-node algorithms use only plain loads and stores.
  Node-local coordination may use ordinary (coherent) atomics.
- **One aligned 8-byte store per message/state update.** Never rely on 64-byte line
  writes being atomic at the device. Anything a reader must observe consistently
  (payload + sequence/request ID) must fit in one 8-byte word.
- **Explicit cache management for cross-node visibility.** Writeback (`clwb` /
  `clflushopt` + `sfence`) before another node reads; invalidate before reading
  what another node wrote — unless the mapping is uncached.
- **Explicit fences** wherever uncached and cached accesses mix, and between a store
  and a later load whose order matters (Dekker/Peterson/Lamport patterns). Do not
  rely on memory-type ordering alone — keep fences even where x86 makes them
  redundant, for portability and model checking.
- **Cached multi-writer variables each get their own cache line** (a writeback
  writes the whole line and can clobber a neighbor). Applies to read-mostly metadata
  like the lock-to-home map.
- **Compact cross-node messages use small indices, not pointers** (an 8-byte pointer
  can't share an 8-byte word with other fields). Servers bounds-check any
  index/offset before writing on a message's behalf.

## Memory regions (design §2)

Keep the memory-access layer abstract so the backend (uncached mapping vs. explicit
flush vs. emulator) can change. Four kinds of state:

- **Hot metadata (uncached):** owner fields, base-algorithm variables (Peterson
  `flag`/`turn`; Lamport `x`/`y`/`b[]`), announcement flags, queue slots, grant slots.
- **Read-mostly metadata (cached, staleness-detected):** lock-to-home map (refreshed
  on NACK via version number), static config.
- **Data (cached):** the data the locks protect. Written back when the global lock
  leaves a node, invalidated when it arrives. Two modes: conservative full
  writeback/invalidate, and precise caller-supplied dirty-range hints (used by KV store).
- **Node-local (ordinary coherent memory):** submutexes, per-acquisition path info,
  local tail counters, handoff counts.

Address spaces: support both (1) same virtual address on all nodes (preferred;
`MAP_FIXED_NOREPLACE` reserved early, fail loudly if it collides) and (2) per-node
mapping where anything stored in shared memory referring to shared memory is an offset.

## Lock architecture (design §3)

Two-level hierarchy. Each global lock has a **node-local submutex** per node
(MCS-style or ticket, using coherent atomics, with local-waiter detection). A thread
takes its node's submutex first, so at most one thread per node contends globally and
the global algorithm always has exactly N participants indexed by node ID.

- **Local handoff (cohort passing):** on release, if the submutex has a waiter and the
  consecutive-local-handoff count is under a cap, pass both the submutex and global
  ownership locally without touching global state. Fence before handoff.
- **Thread-obliviousness is required:** global ownership belongs to the *node*, not the
  thread. Per-acquisition state needed at release (e.g. which path a hybrid took) lives
  **with the lock in node-local memory, never in thread-local storage.**
- **Cross-node handoff = gate + owner field:** the base algorithm is a short *gate* that
  protects a single uncached 8-byte **owner field** (`FREE` or a node ID). The base
  algorithm is NOT held across the critical section. Handoff is one 8-byte store to
  `owner`; waiters poll `owner` directly (no separate handoff flag). See the pseudocode
  in §3.3.

**Phase 1 mode (build/runtime option):** no gate, no owner field — the base algorithm
*is* held across the critical section (`acquire = base_lock`, `release = base_unlock`).

## Global algorithms (design §4)

Four variants over N node-indexed participants:
- **(a) Lamport's fast mutex** — ~2 serialized store-fence-load round trips uncontended;
  O(N) slow path; not starvation-free.
- **(b) Tournament of Peterson locks** — binary tree, ~log₂N round trips; starvation-free.
- **(d) Hybrid** — Lamport fast path, tournament fallback; records the path taken in
  node-local state. Fast-path reset rule is **Open** — specify and model-check first.
- **(c) Lock servers** — each lock has a home node with a background server thread.
  MPSC node-to-server queues (atomic FAA on a node-local tail to reserve a slot, publish
  via per-slot sequence number; capacity ≥ 2× locks-homed-here). Replies written directly
  to a request-named grant slot (index, bounds-checked; reply carries request ID so stale
  replies are ignored). Supports home migration (only while held, version-bumped;
  stale requests get NACK-with-new-home).

## Correctness & evaluation (design §5–6)

- **Model checking** (GenMC, or TLA+ modeling writeback/invalidation) for base algorithms
  under the memory rules, the gate+owner+announce protocol, hybrid reset, and the
  lock-server queues/migration.
- **Runtime stress:** each lock guards a counter + checksum, inject random delays, check
  invariants after millions of acquisitions. Run per phase.
- **Workloads:** SPLASH/PARSEC via LiTL-style pthread interposition
  (https://github.com/multicore-locks/litl), a YCSB-driven KV store (modifiable → precise
  dirty-range hints), and a parameterized microbenchmark (closed- and open-loop).
- **Phases:** Phase 1 (base, no handoff) → ablation (no submutex) → Phase 2a (local
  handoff) → 2b (local + cross-node) → 2c ((c) migration). Implement and evaluate in
  this order.

## Working in this repo

- When adding code, build structure around the invariants above and keep the memory-access
  layer (map/writeback/invalidate/fence) behind an abstraction so the backend can swap.
- Resolve the **Open** items in `design.md` explicitly (and model-check where the spec
  says to) rather than picking a default silently.
- Keep Phase 1 vs handoff, and uncached vs cached-with-flush backends, selectable at
  build or runtime — the evaluation depends on comparing them.
