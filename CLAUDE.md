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
    NOT uncached. On the dev box it is a cost stand-in. On the CXL host it is the
    **recommended configuration**: there, 8-byte (partial) writes to CXL memory are capped at
    ~9M/s in total, and every hot-metadata write is a full line on cf (design.md §2,
    `docs/cxl_results.md` "Device concurrency"). It relies on §1's no-clean-writebacks
    assumption.
  - `emu`: software non-coherence. Each node's data view is private, and writeback and
    invalidate copy whole lines to and from the device. Metadata is physically WB but
    behaves as uncached, except the split layout's scan region (cached, private views):
    a store there writes back the node's whole line, including stale copies of words
    other nodes wrote, as hardware does (`--emu-word-wb`: the stored word only).
    `ncl_stress` checks the 2b announce invariant (`stale_announce`); `--grant-delay` and
    `--debug-no-grant-wait` show it catches the two-writer announce-line hazard
    (design.md §5). Use it to check correctness.
  - Multi-line metadata reads (scans, server poll) go through `ncl_uc_load_many` /
    `ncl_uc_scan_first`: batched on cached-flush (all flushes, one mfence, parallel loads;
    scans probe the first, hinted slot alone first: `-Q probe`, default; `batch`, `serial`), serial with early exit elsewhere. Loads within a batch are
    unordered. 2b and the (c) chain issue the release's data writeback without a fence
    and rely on their fence before the handoff store (`-O on|off`). Notes and uc vs
    cached-flush measurements: `docs/backends.md`.
  - Metadata layout (`-U uniform|split`, `cfg.meta_split`): metadata lives in two regions,
    `meta` (single words: owner, grant slots, x/y, flag/turn, door/race, `succ[]`) and `scan`
    (the scanned, single-writer arrays: Lamport `b[]`, `present[]`, `announce[]`, server
    queue rings). Uniform gives both the backend's type. Split (uc, emu only) makes `scan`
    cached with explicit invalidate/writeback and batched reads. The `ncl_uc_*` calls pick
    the access pattern from the line's address. On emu the split scan region has per-node
    private views, and `--skip-meta-wb` / `--skip-meta-inv` must make stress FAIL (hybrid:
    mutex violations; server: hang caught by `--timeout`).
  - `coherent`: baseline.
- `include/ncl/submutex.h`: MCS submutex with local-waiter detection.
- `include/ncl/base.h`, `src/base.c`: Lamport (a), tournament (b), hybrid (d) per design §4d
  (race/door/present splitter, Aspnes reset), plus splitter and top-lock primitives shared
  with (e).
- `-a ak`: Anderson & Kim's bounded fast path (Distrib. Comput. 2001, Fig. 4) with our
  tournament and top Peterson lock as slow path and ENTRY 2. It reopens the fast path with O(1)
  accesses instead of the hybrid's `present[]` scan. Default `-F fair` = our fairness
  variant (the fast winner reopens only if no slow waiter is at the top lock; every slow
  releaser resets). `-F always` = as published (unfair). `-F` applies to ak only. Not
  model-checked; `docs/hybrid.md`.
- `-a aksrv`: (e) with AK's fast path; the home server runs AK's reset when its queue drains,
  while holding the top SLOW side. Not model-checked; `docs/hybrid.md`.
- `-a pthread`: reference baseline, a process-shared `pthread_mutex_t` (futex, i.e. coherent
  cross-node atomics) in the lock's metadata line. `-b coherent` and Phase 1 only (rejected
  elsewhere: on uc it would be x86 bus locks, meaningless for CXL); `-m` gives every thread
  direct access to the mutex.
- NUMA/CXL hosts (`docs/cxl_host.md`): `--mem-node N` binds the shared regions to node N
  with `mbind` and checks every page with `move_pages` (uc: the kmod's `nid=N` pool, checked
  per page). `--cpus LIST` (`src/cpus.c`) sets the CPUs for pinned threads; the default is
  the process affinity, so `numactl --cpunodebind` works. A warning is printed when pinned
  threads share CPUs or span NUMA nodes. `--wb clwb` uses clwb for writebacks; invalidates
  stay clflushopt. `NCL_MAX_NODES` is 64.
- `src/server.c`: lock servers (c) and fast-path server (e), Phase 1 (fixed home =
  hash % N). Message and reply bit layouts are in `src/server.h`. Nodes must call
  `ncl_node_detach()` (an all-nodes barrier) before servers stop. (d) and (e) are **not
  model-checked**, including two fixes added after measurement: the fast winner reopens only
  if no other node is present (strict scan of `present[]`; required for SAFETY: the splitter
  reset is check-then-act, and any cheaper reopen rule races a slow releaser's stale `door =
  OPEN`; (e)'s server holds a `present[N]` marker during its reset; `ncl_stress --reset-delay`
  widens the window and `--debug-unsafe-reopen` shows the race); the (e) server yields the SLOW side to a waiting fast winner. See
  design.md §4d/§4e. Implementation notes and measurements: `docs/hybrid.md`.
- Waits that depend on another thread (submutex handoff, server grants) spin and then
  `sched_yield()`, so oversubscribed runs make progress.
- `include/ncl/lock.h`, `src/lock.c`: two-level lock. Modes: Phase 1, Phase 2a local handoff,
  and Phase 2b `NCL_MODE_HANDOFF` (`-M handoff`): gate + owner field per design §3.3 for
  (a)/(b)/(d). Defaults: announce before the gate (`-G before`; §3.5 alternative `inside`, the
  spec baseline) and adaptive `pick_waiter` (`-X adaptive`; `scan` = spec). For (c),
  `-M handoff` is a holder-to-waiter chain instead: the server publishes each request's
  predecessor in a per-lock `succ[]` ring, and the holder passes the lock by writing the
  successor's grant slot (REL to the server only when it finds none; hint bit in the grant,
  `-X scan` = always read; window `-R`, default 4N). (e) is rejected. Gate waiters
  poll a grant word in their own announce line (`--wait own`, default) instead of the shared
  `owner` (`--wait owner`; a hot spot on CXL, `docs/cxl_results.md`), re-reading `owner`
  after 16, 32, 64, ... polls. `--ho grant,clear,skip|all|none` (default all that apply) shortens the
  handoff path: grant-only handoff with `owner` FREE/HELD, announce cleared at release,
  owner read before the gate (the gate is entered only to take a FREE lock). Not
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
- `bench/memconc.c` (`ncl_memconc`): aggregate throughput of T threads issuing one access
  pattern (uc/cf load, store, Dekker step, same-line load, full-line writeback, 8- vs
  64-byte movnti), to locate the uc ceiling on CXL; `docs/backends.md`, sweep phase `conc`.
- `-a reactive` (design §4f): per-lock switch between Lamport and the tournament on the
  rate of cross-node contention (`--react U:D[:W]`, default 8:4:64: up when >= U of the node's
  last 16 global acquisitions met another node, down when <= D of the last 64, after >= W
  acquisitions in tournament mode). Lamport's `y`
  holds INVALID while the tournament is active, so a Lamport fast-path acquisition needs no
  `mode` read; a slow-path success re-reads `mode`, and a tournament acquisition reads it
  in the root's entry check, ordered after the root's flag and turn (`ncl_fence_ll`;
  one batched round trip on cached-flush). Phase 1, 2a
  and 2b. Not model-checked. Sweep phase `kvreact`.
- Lock metadata stride rule (`src/lock.c`, design §2.1): a per-lock line count (meta or scan
  region) that is a multiple of 8 gets 2 more lines; strides that are multiples of 512 B cost
  4-6% on the CXL host. `cfg.raw_layout` / `ncl_kv --raw-layout` turns it off.
- `include/ncl/data.h`, `src/data.c`: data maintenance layer (design §2.1), separate from the
  lock algorithms. A lock only reports "arrived" vs "local handoff" after acquiring and
  "departing" (with its fence before the handoff store) at release; the layer holds each
  lock's data context (registered range, run's dirty and fetched ranges) and the
  conservative and precise policies. Tracking is per lock, so data must stay bound to one
  lock; node-wide tracking for data moving between locks is future work.
- `bench/kv.c` (`ncl_kv`, `docs/kv.md`): YCSB-style KV store. Records striped over L locks
  in the data region, static node-local key index, workloads A/B/C/F, uniform or scrambled
  Zipfian keys, key-to-node affinity (`-F`), per-lock algorithm map (`--algmap top:H`: H
  hottest stripes on the tournament, the rest Lamport; library: `cfg.alg_per_lock`,
  `ncl_lock_set_alg`), precise data maintenance (`cfg.precise_data`,
  `ncl_acquire_ranges`: only the named ranges are invalidated; release hints mandatory)
  or `--full`. Every read and the final state are verified; `--debug ...` knobs must FAIL
  on emu (ctests `kv_*`). Sweep phase `kv`.
- Not yet implemented: home migration/NACK (2c), partial-write
  stress test (§5), skewed/phased access in `ncl_bench`, LiTL interposition.

Emulated nodes are forked processes sharing a memfd mapped at a fixed VA
(`MAP_FIXED_NOREPLACE`). Process-private memory is node-local memory.

## Build & test

```
cmake -S . -B build && cmake --build build -j   # -DNCL_STATS=OFF: counters compiled out (docs/backends.md)
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
  redundant, for portability and model checking. Exception (user-approved
  2026-11-19): x86's load-load ordering may be relied on, through `ncl_fence_ll()`
  (`include/ncl/mem.h`), which is only a compiler barrier on x86 and an acquire fence
  elsewhere; always write it where the order matters.
- **Cached multi-writer variables each get their own cache line** (a writeback
  writes the whole line and can clobber a neighbor). Applies to read-mostly metadata
  like the lock-to-home map.
- **Compact cross-node messages use small indices, not pointers** (an 8-byte pointer
  can't share an 8-byte word with other fields). Servers bounds-check any
  index/offset before writing on a message's behalf.

## Platform assumptions from CXL 3.1 (design §1, §2 rule 3)

The spec is in `CXL-3.1-Specification.pdf` (untracked, not for git). The cached regions are
correct only if no caching agent issues *clean writebacks* of prefetched lines ("No Clean
Writebacks" bit, spec §2.4.4); "own cache line" means the host's writeback granularity
(may exceed 64 B). CXL.mem orders nothing across addresses (§3.3.11), so store-fence-load
patterns rely on the host's fence waiting for write completion. Untorn aligned 8-byte words
are guaranteed by x86 and the CXL link but only *assumed* at the device. Partial writes
(`MemWrPtl`, byte enables) exist, but their atomicity is unspecified: packing single-writer
lines is low risk; multi-writer lines need verification on the target device. Measured on
the CXL host, partial writes (UC 8-byte stores, 8-byte movnti) are served at ~9M/s in total
while full-line writes scale to 150-180M/s, so packing does not pay there and hot metadata
should be cached with full-line writeback (design.md §2).

## Memory regions (design §2)

Keep the memory-access layer abstract so the backend (uncached mapping vs. explicit
flush vs. emulator) can change. Four kinds of state:

- **Hot metadata (uncached in the reference design; cached with full-line writeback on the measured CXL device, design §2):** owner fields, base-algorithm variables (Peterson
  `flag`/`turn`; Lamport `x`/`y`/`b[]`), announcement flags, queue slots, grant slots.
- **Read-mostly metadata (cached, staleness-detected):** lock-to-home map (refreshed
  on NACK via version number), static config.
- **Data (cached):** the data the locks protect. Written back when the global lock
  leaves a node, invalidated when it arrives. Two modes: conservative full
  writeback/invalidate, and precise caller-supplied ranges for both directions
  (`cfg.precise_data`: read/write set at acquire, dirty hints at release; used by the KV store).
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
