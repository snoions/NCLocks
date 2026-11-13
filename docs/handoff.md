# Phase 2b: gate + owner cross-node handoff — implementation notes

The protocol is design.md §3.3 (Decided); this file records implementation choices and
measurements. Mode `NCL_MODE_HANDOFF` (`-M handoff`) = Phase 2a local cohort passing
(`docs/local_handoff.md`) plus cross-node handoff through a gate and an owner field.
Variants (a) Lamport, (b) tournament, (d) hybrid serve as the gate. (c) uses a different
cross-node handoff, the holder-to-waiter chain (last section). (e) is rejected (use `-M
local`), as is the no-submutex ablation.

## Implementation (`src/lock.c`: `handoff_acquire`, `handoff_release`)

- **Layout:** per lock, after the base-algorithm lines: one uncached `owner` line
  (0 = FREE, node+1 = held) and `announce[N]`, one line per node. Epoch bits in the
  owner word (§3.5 Open) are not implemented.
- **Acquire** (caller holds the submutex): gate = `ncl_base_lock`; fence; read `owner`;
  if FREE or me, store me (one 8-byte store), fence, `ncl_base_unlock`. Otherwise
  announce, fence, leave the gate and poll `owner`: me = handoff received, FREE = missed
  announcement (back through the gate). On success: fence, `announce = 0` if announced.
- **Release** (never enters the gate): data writeback as before; `pick_waiter`; fence;
  store the chosen waiter (handoff) or FREE; then the submutex unlock. Local cohort passing
  happens first, exactly as in 2a.
- **Announce placement (§3.5 Open), `-G`:** `before` (**default** since the backend
  study, see "Announce placement revisited" below): announce, fence, then the gate.
  `inside` (the spec's baseline): announce right after entering the gate. `lazy` (the
  default before that): inside the gate, and only after reading a held owner, so an
  uncontended take never touches `announce[]`. All are safe by the "owner == me means handoff" rule plus
  the missed-announcement recovery.
- **`pick_waiter`, `-X`:** `adaptive` (**default**): if this node's own global acquisition
  was uncontended (it took a FREE owner without waiting), probe ONE `announce[]` slot at a
  per-node round-robin pointer; otherwise scan round-robin from me+1 as the spec says.
  `scan`: always scan (spec). `probe`: always one probe, which collapses at moderate load
  (below). A probe that misses stores FREE; a waiter it skipped recovers through the gate.
  The pointer advances past the node served (or by one on a miss), so under saturation the
  pointers sweep the ring. A pointer that only remembered the last waiter would let two
  nodes hand the lock back and forth while the others never see FREE.
- **Safety sketch:** `owner` is written only by the holder (at release) or by a gate user
  that read FREE (or its own ID); gate users are mutually excluded, and a FREE owner has no
  holder, so no two writers race. A releaser that misses an announcement stores FREE after
  that waiter's owner read returned "held", so the waiter's later poll sees FREE and it
  retries. Not model-checked (design §5).
- **Owner poll: backoff + spin-then-yield.** A handoff commits the lock to one specific
  node; if its waiting thread is descheduled, the lock idles while other spinners hold the
  CPUs. Four concurrent stress runs (48 threads, 20 CPUs) took 14 s without the yield vs
  0.14 s in Phase 1, and 0.77 s with it. Same convention as the submutex and server waits.
- **Stats** (`struct ncl_handoff_stats`, CSV columns `global_handoff_frac`,
  `local_handoff_frac`, `missed_announce_per_acq`, `owner_polls_per_acq`,
  `gate_entries_per_acq`, `announce_loads_per_acq`); path `NCL_PATH_GLOBAL_HANDOFF`.
- **Tests:** `stress_{lamport,tournament,hybrid}_{emu,uc}_handoff{,_cap1,_inside_scan,
  _lazy_cap1,_probe_cap1}` (`-K 1` makes almost every release a cross-node handoff;
  defaults are before/adaptive), emu negative tests with
  `--skip-wb` / `--skip-inv` in handoff mode, and `handoff_rejects_server`.

## Measurements: spec baseline (uc, 200 ns CS, 64 B footprint, `-B 16`, 3 interleaved reps)

This section uses the spec's baseline, `-G inside -X scan` (plus `-G before`). The
optimizations and the current defaults are in the next section.

Raw data: `results/handoff_uc.csv`.

**Uncontended** (Poisson 100k; acquire p50; hold = sojourn p50 − acquire p50):

| | Phase 1 | handoff (inside) | handoff (before) |
|---|---|---|---|
| tournament N=4 | 415 ns | 799 ns | 831 ns |
| tournament N=8 | 575 ns | 991 ns | 1023 ns |
| Lamport N=8 | 383 ns | 735 ns | 767 ns |
| hybrid N=8 | 543 ns | 1087 ns | 1087 ns |

The design expected about +1 round trip (the owner read). Measured: about +4, i.e.
announce store + fence, owner load, owner store + fence, announce clear. The release adds
the `announce[]` scan, N−1 loads when nobody waits (6.7 loads/acq at N=8): tournament hold
time at N=8 grows ~416 -> ~930 ns.

**Capacity** (closed loop, Mops/s; fairness min/max):

| | Phase 1 / 2a | handoff inside | handoff before |
|---|---|---|---|
| tournament 4x1 | 1.57 | 0.93 | 0.94 |
| tournament 8x1 | 1.66 (0.98) | 0.92 (1.00) | 0.92 (1.00) |
| hybrid 4x1 / 8x1 | 1.11 / 1.13 | 0.90 / 0.90 | 0.88 / 0.90 |
| Lamport 4x1 | 0.84 (0.75) | 0.94 (1.00) | 0.93 (1.00) |
| Lamport 8x1 | 0.50 (0.84) | **0.93 (1.00)** | 0.92 (1.00) |
| 4x2 (vs 2a local) | 2.43-2.46 | 2.43-2.46 | 2.45-2.47 |

Saturated, nearly every acquisition is a global handoff (the gate is idle), so capacity is
the handoff cycle, ~0.92M/s for every gate. A diagnostic run (tournament 8x1) rules out
poll backoff (`-B 1`: 0.85M, `-B 4`: 0.95M, `-B 64`: 0.54M) and data movement (no footprint:
1.03M vs Phase 1 2.16M; no CS: 1.02M vs 2.29M). The cycle is about 1 us with four
serialized uncached operations (releaser's owner store, waiter's poll, waiter's
`announce = 0`, which the next UC load waits for, and the releaser's `announce[]` scan load)
plus two `mfence`s. A saturated tournament instead pipelines its lower levels and pays about
one store plus one load per handoff. At 4x2, 98% of acquisitions are local handoffs, so 2b
equals 2a.

**Open loop, N=8** (acquire p50 / p99):

| offered | tournament P1 | tournament handoff | Lamport P1 | Lamport handoff | hybrid P1 | hybrid handoff |
|---|---|---|---|---|---|---|
| 300k | 607 ns / 2.2 us | 1.0 / 4.6 us | 383 ns / 4.4 us | 799 ns / 6.1 us | 543 ns / 4.0 us | 1.2 / 6.1 us |
| 600k | 607 ns / 3.2 us | 1.8 / 10 us | sat (19%) | 2.0 / 23 us | 927 ns / 10 us | 2.4 / 13 us |
| 900k | 895 ns / 5.4 us | 27 / 279 us | sat (50%) | 111 / 688 us | 4.1 / 32 us | 82 / 819 us |
| 1.2M | 1.3 / 10 us | sat (26%) | sat | sat (25%) | sat (6%) | sat (27%) |

On/off bursts (1 ms period, 25% on, so 4x the mean rate while on), p50: at 300k mean,
tournament 1.3 us (P1) vs 35 us (handoff), Lamport 156 us vs 45-57 us; at 600k mean, the
tournament 49 us vs 205 us. The bursts exceed handoff's capacity.

**Announce placement:** `before` cuts missed announcements ~100x (tournament 600k: 0.051
-> 0.0002 per acquisition) and raises the handoff share (65 -> 73%); latency and capacity
are otherwise about the same. Recoveries are rare in either case (at most 7.5% of
acquisitions, at moderate load).

**Summary.** Gate + owner pays off where the base algorithm is expensive to hold across the
CS (Lamport: 0.50 -> 0.93M at N=8 and starvation gone). With the tournament or hybrid as the
gate it costs 20-45% capacity and about +400 ns uncontended on this platform. The
optimizations below recover most of the uncontended hold-time cost but not the handoff
cycle.

## Optimizations: lazy announce and adaptive `pick_waiter`

(Lazy announce was the default until "Announce placement revisited" below; adaptive
`pick_waiter` still is.)

Raw data: `results/handoff_opt_uc.csv` (Phase 1, inside/scan, lazy/scan, inside/probe,
lazy/probe; 4x1, 8x1, Poisson 100k at N=4 and 100k-900k at N=8, on/off at N=8) and
`results/handoff_adaptive_uc.csv` (inside/lazy x scan/adaptive). 3 interleaved reps each.

**Lazy announce** (`-G lazy`): saves the announce store and its clear on an uncontended take.
- Acquire p50 improves by 32-64 ns, about one histogram bucket: tournament N=4 799 -> 767 ns,
  Lamport N=8 767 -> 703 ns, hybrid N=8 1087 -> 991 ns. That is less than the two round
  trips expected; uncached stores are cheaper than loads.
- Capacity is unchanged.
- Missed-announcement recoveries double (tournament 600k: 0.05 -> 0.10 per acquisition)
  because waiters announce later. Latency at 300-600k is unchanged to slightly worse.

**Always-probe** (`-X probe`): rejected as a default.
- It gets the expected win where every slot agrees: uncontended (tournament N=8 hold −450
  ns) and saturated (tournament 4x1 0.92 -> 0.97M).
- At moderate load, the single probe usually hits an idle slot and stores FREE while others
  wait, so they all retry through the gate. Recoveries reach 0.25-0.79 per acquisition at
  300k-900k.

  | N=8, 600k, p50 | scan | probe |
  |---|---|---|
  | Lamport | 2.0 us | 139 us |
  | hybrid | 2.4 us | 39 us |

  Hybrid 8x1 capacity also drops from 0.90 to 0.75M.

**Adaptive** (`-X adaptive`): probe only after an uncontended acquisition.

| N=8 | scan | adaptive |
|---|---|---|
| hold at 100k (sojourn p50), tournament / Lamport / hybrid | 1983 / 1727 / 2047 ns | **1535 / 1279 / 1599 ns** |
| release loads per acquisition at 100k | 6.7 | 1.4 |
| closed-loop capacity, tournament / Lamport / hybrid | 0.913 / 0.922 / 0.893M | 0.919 / 0.908 / 0.899M |
| fairness | 1.00 | 1.00 |
| recoveries per acquisition at 300k / 600k | 0.05 / 0.05 | 0.18 / 0.22 |
| p99 at 300k, tournament / hybrid | 4.4 / 6.1 us | 5.4 / 9.2 us |

p50 at 300k and 600k is unchanged. At 900k, close to saturation, and in the on/off runs,
every variant is bimodal from run to run (e.g. tournament p99 at 900k ranges 0.2-1.8 ms
across variants), so those points do not rank the variants.

**With both defaults**, tournament N=8 hold time at 100k is ~1471 ns, versus ~1983 ns for
the spec baseline and ~991 ns for Phase 1. The acquire still costs about +380 ns over
Phase 1, and saturated capacity stays at ~0.92M: neither change shortens the handoff cycle.
The spec baseline remains available as `-G inside -X scan`.

## Announce placement revisited: `before` is the default

Lazy announce was chosen on uc data. On the cached-flush backend, where every metadata
load costs a flush plus a fence, it collapses for Lamport and partly for hybrid.

**Mechanism.** A lazy waiter announces only after it gets through the gate. When the gate
is slow compared with the handoff cycle (Lamport's gate with costly loads), waiters pile up
inside it, retrying (5.3 Lamport retries per acquisition), and none of them is announced
yet. The releaser finds nobody, stores FREE, and the next gate winner takes the lock: 2b
degrades to Phase 1 Lamport, with its starvation. Once in that mode it stays there. With
`before`, a waiter is announced before it enters the gate, so a releaser always sees it,
however congested the gate is. Whether `lazy` works therefore depends on the gate's speed
relative to the handoff, which differs between backends. Real CXL latencies could push
`uc` into the same mode.

N=8 x 1, 200 ns CS, 3 interleaved reps (`results/handoff_announce_uc_cf.csv`;
cf = cached-flush):

| | before | lazy |
|---|---|---|
| Lamport-2b cf, capacity (fairness, handoff share) | **1.03M (1.00, 100%)** | 0.52M (0.76, 11%) |
| Lamport-2b cf, 600k p50 | **2.6 us** | saturated (13% unserved) |
| hybrid-2b cf, capacity (fairness) | **1.06M (1.00)** | 0.99M (0.95) |
| hybrid-2b cf, 600k p50 / p99 | **4.4 / 24 us** | 5.1 / 27 us |
| tournament-2b cf, capacity | 1.03M | 1.03M |
| uc capacity, Lamport / tournament / hybrid | 0.94 / 0.94 / 0.92M | 0.93 / 0.93 / 0.92M |
| uc 600k p50, Lamport / tournament / hybrid | **2.0 / 1.7 / 2.9 us** | 2.7 / 1.9 / 3.2 us |
| uncontended acquire p50 at 100k, uc (L / T / H) | 767 / 1023 / 1087 ns | 703 / 959 / 991 ns |
| uncontended acquire p50 at 100k, cf (L / T / H) | 1087 / 1343 / 1407 ns | 895 / 1151 / 1279 ns |

`before` costs 64-96 ns uncontended on uc and 128-192 ns on cached-flush: the announce
store, a fence, and the clear. It wins at moderate load on both backends, because missed
announcements halve (600k: 0.08-0.15 vs 0.18-0.26 per acquisition). Most importantly, it
never falls into gate mode. The lazy measurements above remain valid for uc.

## Own-line wait (`--wait own`, default)

**Why.** On the CXL host, with T=1, every gate algorithm loses 2b throughput roughly as
1/N (tournament on uc: 229k at 4 nodes, 38k at 32, against 359k in Phase 1). The handoff
interval equals owner polls per acquisition × ~420 ns: the device serves the one shared
`owner` line one access at a time, so the releaser's store and the new owner's load queue
behind the other waiters' polls. A larger `-B` helps up to a point, then the new owner
notices late (`docs/cxl_results.md`, "Backoff check"). The server chain does not have
this problem, because each waiter polls its own grant slot.

**Protocol** (`cfg.handoff_wait = NCL_WAIT_OWN`, the default since the CXL measurements
below; `--wait owner` = design §3.3 as first specified):
- **Announce:** the announce word (word 0 of node j's `announce[]` line) holds a fresh
  per-node sequence number `s` (`ncl_local_lock.ann_seq`, never 0) instead of 1.
- **Release:** `pick_waiter` also returns the word it read (`ncl_uc_scan_first_v`). After
  `owner = j`, the releaser fences and stores `s` into word 1 of j's line (the grant).
  The fence is full when the line is cached (split scan region, cached-flush), because a
  `clwb`/`clflushopt` writeback is not ordered after an earlier UC store. It is a
  store-store fence when the line is uncached.
- **Wait:** poll word 1 of the own line until it equals `s`. After `NCL_OWNER_RECHECK`
  (16) polls, and then after twice as many each time (16, 32, 64, ...), also read
  `owner`: FREE = missed announcement (back through the gate, as before), me = handoff.
  So a wait costs O(log wait) owner loads, and a miss is noticed within twice the time
  already waited. The first version re-read every 16th poll. On the CXL host that rebuilt
  the hot spot whenever waits were long: Lamport at 32 nodes made 25-32 owner loads per
  acquisition and gained nothing.
- **Grant before clear:** a waiter that learns of its handoff from `owner` (in the gate or
  on a recheck) waits for its grant before clearing its announce (`grant_waits`).
- **Order of writes to the shared line:** the waiter's announce, the releaser's read of
  it, the releaser's grant, the waiter's read of the grant, the waiter's clear. No two
  stores to the line overlap, so on the split layout neither node's whole-line writeback
  can clobber the other word. The releaser's copy is current: it read the line after an
  invalidate, and the waiter does not write it until it sees the grant.
- **Stale grants:** a grant from an earlier acquisition holds an older sequence number
  and is ignored.
- **Cost:** one more metadata store per cross-node handoff, plus a fence. In exchange, the
  owner line sees only gate traffic and rechecks.

**Safety** is unchanged: `owner` is written exactly as before, and the grant only lets a
waiter stop polling earlier than an `owner == me` read would. Liveness: an announce lost
to a missing writeback or invalidate only causes FREE handoffs (the waiter recovers via
the recheck), so `--skip-meta-wb`/`--skip-meta-inv` do not make it fail. emu copies words,
not whole lines, so it cannot show the clobber that grant-before-clear prevents; that part
rests on the ordering argument above. Not model-checked.

**Dev box** (uc split, T=1, 200 ns CS, `-B 16`, 3 reps; DRAM-backed UC, no hot spot):
`own` vs `owner`: tournament 922k / 830k (N=4), 841k / 813k (N=8); Lamport 911k / 838k,
870k / 816k; hybrid 903k / 852k, 859k / 819k. Fairness is 1.0, owner polls per
acquisition drop from ~2N to under 0.2, and grant polls are ~2N.

**CXL host** (fixed every-16th recheck, `docs/cxl_results.md`): with T=1, `own` removes the
1/N collapse for tournament, hybrid and AK, at 2.4-6x `owner`'s throughput at 32 nodes,
fairness 1.0. With local handoff (16x3) it adds 10-22%. The doubling recheck targets the
Lamport case and is not yet measured there.

**Tests:** all `-M handoff` gate tests run `own` (the default), including the split
layout (`..._split_handoff_own_cap1`, `stress_tournament_{emu,uc}_split_handoff_own_n8_probe`,
`stress_tournament_cached-flush_handoff_own_n8`). `owner` is covered by
`stress_{lamport,tournament,hybrid,ak}_{emu,uc}_handoff_owner_{cap1,lazy}` and
`stress_hybrid_{emu,uc}_split_handoff_owner_cap1`. Also `wait_rejects_{server,phase1}`.
CSV columns `wait`, `grant_polls_per_acq`.

## (c) lock server: holder-to-waiter chain (`-a server -M handoff`)

In Phase 1, a contended (c) handoff is two hops through the server: the holder sends REL,
the server's poll loop finds it, then the server stores GRANT and the waiter's poll sees
it. The chain takes the server off that path. Protocol and safety argument: design.md §4c
"Holder-to-waiter chain". Code: `src/server.c` (`chain_*`, `ncl_srv_chain_release`). Not
model-checked.

- **Server:** each REQ gets a grant number n (FIFO). If n has a predecessor, the server
  publishes `succ[(n-1) % R] = (n-1, node, gslot, reqid)`, one 8-byte store into a per-lock
  uncached ring that only the server writes. On `REL(k)` it grants k+1 itself if that
  number is assigned, otherwise the lock becomes free.
- **Holder of k at release:** after data writeback, it reads `succ[k % R]` once. If the tag
  is k, it fences and stores grant k+1 (seq, hint and passed bits) into the successor's
  grant slot: no message. Otherwise it sends `REL(k)`, carrying k. Local cohort passing
  (2a) happens first, as in every handoff mode.
- **Hint** (default; `-X scan` = always read): the grant word says whether `succ[k]` was
  already published when the server granted, and every pass sets it. An uncontended holder
  therefore skips the uncached load, and its release is one REL store, as in Phase 1.
- **Window** (`-R`, default 4N rounded up to a power of two; ≤ 128): the server cannot see
  passes, so it assigns at most R numbers past the last one it knows finished. A full window
  makes the chain end with a REL, which resynchronizes the server. Under saturation this
  costs one server round trip per R grants (`succ_misses`).
- **Bug found by stress:** the window kept ending at the same ring slot, so that slot was
  never rewritten. 2^14 grants later its 14-bit tag matched again, and a holder passed the
  lock to a long-finished request: lost lock, hang. Fix: the server clears slot n % R when it
  assigns n (that slot belonged to n − R, already finished). `stress_server_*_handoff_wrap`
  hangs 6/6 without the fix.
- **Tests:** `stress_server_{emu,uc}_handoff{,_cap1,_scan,_window2,_wrap}`,
  `emu_detects_missing_writeback_server_handoff` (WILL_FAIL), `handoff_rejects_fastsrv`.
- **Stats:** path `global_handoff` = received a pass; the CSV's `missed_announce_per_acq` =
  `succ[]` read without a match, `announce_loads_per_acq` = `succ[]` loads, `chain_window` =
  R. The human-readable output adds server `succ_published`, `rel_grants`, `window_full` and
  client `passes_sent`, `rels`.

### Measurements (uc, 200 ns CS, 64 B footprint, servers `-Y sibling` at N=8, 3 interleaved reps)

Raw data: `results/server_chain_uc.csv` (Phase 1 vs chain, hint and always-read, R = 4N; the hint
rows appear three times per rep because a script bug ran the planned 2N/8N variants at the
default),
`results/server_chain_vs_tournament_uc.csv` (first sweep: R = 2N, plus Phase 2a server and
tournament Phase 1 / 2b references), `results/server_chain_window_uc.csv` (R = 2N/4N/8N).

**Capacity** (closed loop, Mops/s; fairness min/max 1.00 everywhere except tournament Phase 1):

| | server Phase 1 | chain (hint) | chain (always read) | tournament Phase 1 | tournament 2b |
|---|---|---|---|---|---|
| 4x1 | 1.44 | **1.58** | 1.56 | 1.58 | 0.92 |
| 8x1 | 1.15 | **1.55** | 1.48 | 1.68 (0.98) | 0.92 |
| 4x2 | 1.18 (2a: 1.80) | **2.46** | 2.46 | 1.42 (0.97) | 2.48 |

At 8x1, 97% of acquisitions are passes. The server still receives every REQ, but only
one REL per 32 grants, and it is off the handoff path.
The handoff cycle is one uncached load (`succ`) plus one store (grant), versus 2b's four
serialized uncached operations (0.92M for every gate). At 4x2 (98% local passes), the
chain beats server 2a by 37%. Each global transfer in 2a waits for the server; with the
chain it does not.

**Uncontended** (Poisson 100k; hold = sojourn p50 − acquire p50): with the hint, hold time
equals Phase 1 (N=8: 320 vs 384 ns, one histogram bucket); always-read adds ~130 ns (448-512
ns). Acquire p50 is unchanged (1.8 us at N=8, 2.3 us at N=4). That number is the idle
server's backoff (cap 1024 + `sched_yield`), the same for every server variant and much
slower than the tournament's 575 ns.

**Open loop, N=8** (acquire p50 / p99):

| offered | server Phase 1 | chain (hint) | tournament Phase 1 |
|---|---|---|---|
| 300k | 991 ns / 8.7 us | 1.1 / 8.7 us | 575 ns / 2.2 us |
| 600k | 1.2 / 7.4 us | 1.2 / 4.9 us | 607 ns / 3.2 us |
| 900k | 3.2 / 39 us | **1.5 / 6.7 us** | 895 ns / 5.4 us |
| 1.2M | sat (20% unserved) | **2.0 / 12 us** | 1.3 / 9.2 us |
| on/off 300k mean | 30 / 111 us | **2.3 / 35 us** | 1.3 / 9.7 us |

Below ~600k the two are the same (few waiters, so few passes). Near Phase 1's capacity,
and in bursts, the chain cuts p50 2-13x and p99 3-6x, and it serves 1.2M without
saturating.

**Window size** (hint; Mops/s closed loop, p50 / p99 at 1.2M offered):

| R | 2N | 4N (default) | 8N |
|---|---|---|---|
| 4x1 capacity | 1.53 | 1.61 | 1.65 |
| 8x1 capacity | 1.48 | 1.54 | 1.58 |
| 8x1, 1.2M | 2.3 / 17 us | 2.2 / 13 us | 1.9 / 10 us |

Each doubling halves the REL resyncs (1/R per grant) and gains 3-5% capacity. At 900k and
in bursts, the three are within noise. 4N is the default as a compromise with memory: R
lines per lock, i.e. 4 KB per lock at N=8 with R = 8N.

**Memory.** Per-lock uncached lines (64 B each) at N=8:

| | lines per lock | bytes |
|---|---|---|
| Lamport | N+2 = 10 | 640 B |
| tournament | 3(N−1) = 21 | 1.3 KB |
| hybrid | 34 | 2.2 KB |
| 2b gate + owner | base + N+1 | +576 B |
| server Phase 1 | ~2N queue slots (2N-4N with power-of-two rounding), spread over the N source queues | ~1-2 KB |
| server chain | the above + R (default 4N = 32) | +2 KB, ~3 KB total |

Every variant is O(N) lines per lock. The chain's ring is the largest single item (4 KB per
lock at N=16). That is irrelevant for the microbenchmark, ~30 MB at 10k locks, but ~3 GB
for 1M locks at N=8 (KV store scale). The server also keeps a 128-entry assignment array
(~0.8 KB node-local) per homed lock; it could be sized to R. Options, none implemented:

1. **Smaller window.** Any power of two ≥ 2 is correct (`stress_server_*_handoff_window2`).
   It trades memory for resyncs: 2N gives 1.48M vs 1.54M at 8x1. R = 2-4 is not measured.
2. **Rings on demand (preferred before the KV store).** Each home keeps a pool of rings.
   It assigns one to a lock when a second request arrives, carries the ring index in the
   grant word (bits 52-63 are free), and reclaims the ring when a REL leaves the lock free.
   At that point the server knows every assigned number has finished. Memory then scales
   with the number of concurrently contended locks, not with nlocks; uncontended locks get
   hint = 0 grants and never touch a ring. Reuse is safe because of clear-on-assign: holder
   n reads only slot n % R, which the server cleared when it assigned n, so entries left by
   the ring's previous lock are never visible. Pool exhaustion falls back to Phase 1 grants
   (no ring, hint = 0).
3. **Pack 8 entries per line.** The ring is uncached and has a single writer (the server),
   so it needs only what CXL 3.1 defines: `MemWrPtl` byte enables (Table 3-41) plus untorn
   8-byte words. Even a host-side read-modify-write (forced by host-side encryption)
   rewrites only the server's own words. It does not need the device to merge concurrent
   partial writes from different hosts atomically, which the spec leaves unspecified
   (design §2 rule 3). Confirm with the single-writer partial-write test (design §5). It
   would cut ring memory 8x and could combine with option 2.

**Summary.** For (c), cross-node handoff works much better than gate + owner did for
(a)/(b)/(d): no uncontended cost, +10% (4x1) to +35% (8x1) capacity, and much better tails
near saturation. It still trails the Phase 1 tournament at low load (the server's poll
latency) and at 8x1 capacity (1.55 vs 1.68M). The costs: R uncached lines per lock
(2 KB at N=8), grant slots with two possible writers, and a server that no longer sees each
hold boundary, so (e)'s yield rule and 2c migration would need rework.
