# Metadata backends: uncached vs explicit flush, and batched reads

Two ways to keep hot metadata consistent across nodes (design §2):
- **`uc`**: really uncached (PAT UC-, `kmod/ncl_uc.ko`).
- **`cached-flush`**: ordinary cached memory with `clflushopt` after every store and
  `clflushopt; mfence` before every load.

On this host `cached-flush` is a timing stand-in only. The host is coherent, so it cannot
expose missing flushes, and a flush of a line another core has modified costs a coherence
writeback that real non-coherent hardware would not pay.

## Where a flush can go early, and where it cannot

- **Stores** are already flushed as early as possible (`store; clflushopt`). The flush is
  asynchronous and only waited on at the next fence. Almost every store is followed by the
  fence that orders it (store-fence-load), so there is no independent work to overlap.
- **Single loads** cost two serialized trips: invalidate, then miss.
- **Do not issue an invalidate before the fence that orders an earlier store.** An
  example is `store X; flush X; flush Y; mfence; load Y`. A hardware prefetcher can
  re-fetch Y between its flush and the fence, and fences do not order prefetches. That
  copy predates X becoming visible, which is the store-to-load reordering Dekker forbids.
  For the same reason, protected data must not be invalidated before the lock is acquired.
- **Multi-line reads can be batched:** `clflushopt` every line, one `mfence`, then all
  loads, which miss in parallel. That is about 2 trips instead of K. UC loads execute one
  at a time, so the `uc` backend cannot do this.
- **Loads within a batch are unordered.** x86 load-load ordering relies on coherence
  snoops, which other hosts never send. Where an algorithm needs one read before another
  (the splitter's `door` before `present[]`), separate calls with a fence are required.
  UC loads give that order for free.

## Implementation (`include/ncl/mem.h`)

- `ncl_uc_load_many(lines, n, out)` and `ncl_uc_scan_first(arr, n, start, count, skip,
  &loads)`: batched on `cached-flush`. On `uc`, `emu` and `coherent` they are serial with
  early exit at the first set slot, so behavior on those backends is unchanged. Injected
  device latency (`-L`) is charged once per batch.
- Sites:
  - the splitter scans (release reopen check with `-F scan`, `ncl_splitter_reset_if_quiet`);
  - Lamport's slow-path `b[]` scan (a batched first pass; only nonzero slots are polled);
  - the Peterson wait (`flag[other]` and `turn` together);
  - 2b `pick_waiter` with `-X scan`;
  - the lock server's poll round (all N queue slots).
- `-Q probe|batch|serial` (bench and stress; default `probe`, see below) for A/B; CSV column `meta_batch`.
- Server poll on serial backends keeps load-then-process per slot. A first version loaded
  all N slots before processing any; on `uc` at N=8 that delayed each grant by up to 7
  uncached loads (about 500 ns) and cost 35% capacity (1.21 -> 0.80M).
- Tests: `stress_*_cached-flush_batch_n8` (`-F scan`, N=8) and
  `stress_tournament_cached-flush_batch_handoff_scan`. On this coherent host these check
  the batch index logic, not flush or fence placement.

## Per-operation cost (`ncl_memlat`, one core)

| | uc | cached-flush |
|---|---|---|
| 1 load | 70 ns | 99-106 ns |
| store + fence | 49 ns | 114-128 ns |
| store; fence; load (one Dekker step) | 118 ns | 145-201 ns |
| 3-line scan, serial / batched | ~230 ns / - | 290 / 109 ns |
| 7-line scan, serial / batched | 531 ns / - | 620-659 / 112-116 ns |
| unsafe: invalidate before the fence | - | 118-145 ns |

The batch cost is nearly flat in K. With real CXL latencies L the gap grows: about K*L
serial versus about 2L batched.

## Locks (uc, N=8 x 1, 200 ns CS, `-B 16`, servers `-Y sibling`, 3 interleaved reps)

Raw data: `results/backends_batch_uc_cf.csv` (`variant` column; `cf` = cached-flush).

**Capacity** (closed loop, Mops/s, fairness min/max):

| | uc | cf batch | cf serial |
|---|---|---|---|
| Lamport | 0.50 (0.85) | 0.49 (0.68) | 0.46 (0.74) |
| tournament | 1.70 | 1.64 | 1.60 |
| hybrid `-F hint` | 1.12 | 0.93 | 0.94 |
| hybrid `-F scan` | 1.13 | 0.91 | 0.95 |
| server | 1.21 | **1.24** | 0.95 |
| fastsrv | 1.06 | 0.99 | 0.88 |
| tournament 2b `-X scan` | 0.92 | 0.97 | 1.02 |

**Uncontended** (Poisson 100k; acquire p50 / hold = sojourn p50 − acquire p50):

| | uc | cf batch | cf serial |
|---|---|---|---|
| tournament | 575 / 416 ns | 767 / 512 ns | 767 / 512 ns |
| Lamport | 383 / 384 ns | 495 / 496 ns | 543 / 480 ns |
| hybrid `-F scan` | 543 / 992 ns | 703 / **640** ns | 703 / 1152 ns |
| tournament 2b `-X scan` | 959 / 960 ns | 1215 / **512** ns | 1215 / 1088 ns |

**Moderate load** (Poisson 600k, acquire p50 / p99): server uc 1.2 / 7.4 us, cf batch
1.1 / 6.9 us, cf serial 1.5 / 10.8 us. Tournament 2b `-X scan`: uc 2.0 / 10.8 us, cf batch
1.6 / 7.2 us. Hybrid `-F scan`: uc 1.5 / 10.8 us, cf batch 2.7 / 18 us.

## Reading the results

- **Batching wins where a read touches every line:**
  - a release scan that finds nobody: the hybrid and 2b release scans are 350-450 ns
    faster than uc, and 500-575 ns faster than serial cached-flush;
  - the server poll: +30% capacity over serial cached-flush, slightly above uc.
- **It loses a little under saturation.** The serial scan stops at the first set slot,
  usually the first one read, while the batch reads all of them. Hybrid `-F scan` and 2b
  `-X scan` lose 4-5% capacity against serial. Part of this is the testbed: flushing lines
  that other cores keep modifying costs coherence traffic here. The probe-first refinement
  (next section) recovers this.
- **Everything else favors uc:** single-word polls, stores, and store-fence-load steps.
  Lock acquisition is 110-200 ns slower on cached-flush for every algorithm.
- **Best of both would be per-region memory types:**
  - uncached for words that are polled or handed off;
  - cached + batched flush for arrays that are scanned (`present[]`, `announce[]`, `b[]`,
    server queues).

  This would bring in the cached-metadata requirements of design §1: no clean writebacks,
  and padding to the host's writeback granularity. Not implemented.

## Probe first, then batch (`-Q probe`, the default)

`ncl_uc_scan_first` with `-Q probe` reads the first candidate alone (one `flushopt; mfence;
load`). If that slot is 0, it batches the rest. The first candidate is the hinted or next
slot (`reset_hint`, me+1). Under contention it usually decides, so a saturated scan costs
one load as in serial mode; an empty scan costs one load plus one batch. Only scans
change; `ncl_uc_load_many` (Peterson wait, server poll) still reads all lines.

cached-flush, N=8 x 1, 3 interleaved reps (`results/scan_probe_cf.csv`):

| | serial | batch | probe |
|---|---|---|---|
| tournament 2b `-X scan`, capacity | 1.061M | 1.008M | **1.059M** |
| tournament 2b `-X scan`, hold at 100k | 960 ns | 512 ns | **576 ns** |
| tournament 2b `-X scan`, 600k p50 / p99 | 2.2 / 9.2 us | 1.5 / 6.7 us | **1.6 / 6.9 us** |
| hybrid `-F scan`, capacity | 0.930M | 0.909M | **0.931M** |
| hybrid `-F scan`, hold at 100k | 1152 ns | 672 ns | **704 ns** |
| Lamport, capacity (fairness) | 0.441M (0.77) | 0.493M (0.70) | 0.484M (0.74) |

Probe matches serial at saturation (release loads per acquisition: 1.00 vs 7.00 for
batch) and stays within one histogram bucket of batch when the scan finds nothing. Hybrid
`-F hint` (scans only in `ncl_splitter_reset_if_quiet`, which already pre-checks the hinted
slot) is unchanged within noise: 0.94 / 0.91 / 0.90M.

## Does the protocol ranking change? (N=8, defaults, `results/ranking_uc_cf_n8.csv`)

Measured with lazy announce, the 2b default at the time; see below.

| capacity rank | uc | cached-flush |
|---|---|---|
| 1-3 | tournament 1.68, server chain 1.56, server 1.23 | tournament 1.62, server chain 1.51, server 1.22 |
| 4 | fastsrv 1.12 | tournament-2b 1.04 |
| 5 | hybrid 1.10 | hybrid-2b 0.99 (fairness 0.95) |
| 6 | Lamport-2b 0.95 | fastsrv 0.98 |
| 7 | tournament-2b 0.93 | hybrid 0.93 |
| 8 | hybrid-2b 0.92 | Lamport-2b 0.51 (fairness 0.73) |
| 9 | Lamport 0.49 (0.86) | Lamport 0.48 (0.70) |

- **The top three are the same on both backends.**
- **Hybrid (Phase 1) drops.** At 600k it goes from 2nd (uc: 0.9 / 9.7 us) to 6th (cf:
  2.7 / 19 us).
- **The tournament loses its uncontended lead.** Each of its log N levels pays a flush
  plus fence per load. Uncontended sojourn: uc Lamport 767 < tournament 991 < hybrid 1087
  ns; cf Lamport 991 < hybrid ≈ Lamport-2b ≈ tournament ≈ 1343 ns.
- **2b gates move up on cf** (tournament-2b 0.93 -> 1.04M), helped by batched scans and
  the shared fence.
- **Lamport-2b collapsed** because of lazy announce, not because of the backend. With
  `-G before`, now the default, it reaches 1.03M at fairness 1.00 on cf, and hybrid-2b
  1.06M (`docs/handoff.md`, "Announce placement revisited").

## Overlapping the data writeback with release-path loads (`-O on|off`, default on)

Only the store that hands the lock to another node has to wait for the data writeback.
Two release paths load metadata before that store: 2b (`pick_waiter`) and the (c) chain
(the `succ[]` load). With overlap, `ncl_release` issues the data flushes without the
trailing fence (`ncl_data_writeback_nofence`). The fence those paths already have before
the handoff store then waits for the flushes: `handoff_release` before the owner store,
`ncl_srv_chain_release` before the grant store, and `send()` before REL. The other release
paths (Lamport, tournament, hybrid, fastsrv, Phase 1 server) start with a store, so they
keep the fenced writeback. Emu's writeback is a synchronous copy, so emu cannot detect a
misplaced fence here; correctness rests on the fence placement listed above.

**`ncl_memlat`** (one metadata load next to K dirty data-line flushes; fence first vs load
first):

| K | uc: fence, load | uc: load, fence | cf: fence, load | cf: load, fence |
|---|---|---|---|---|
| 1 | 201 ns | 201 ns | 203 ns | 191 ns |
| 8 | 230 ns | 237 ns | 252 ns | 239 ns |
| 64 | 578 ns | 671 ns | 616 ns | 652 ns |

A UC load does not get ahead of outstanding `clflushopt`s: it waits for them, as if they
were older stores (UC is strongly ordered). So on `uc` there is nothing to overlap.

**Locks** (N=8 x 1, 2b and chain, 3 interleaved reps; `results/wb_overlap_uc_cf.csv`):

| | footprint | closed off -> on | hold at 100k off -> on |
|---|---|---|---|
| tournament 2b, uc | 64 B / 4 KB | 0.920 -> 0.920 / 0.582 -> 0.592M | 512 -> 512 / 960 -> 960 ns |
| tournament 2b `-X scan`, uc | 64 B / 4 KB | 0.917 -> 0.933 / 0.585 -> 0.597M | unchanged |
| chain (server), uc | 64 B / 4 KB | 1.535 -> 1.589 / 0.785 -> 0.793M | 384 -> 320 / 896 -> 896 ns |
| tournament 2b `-X scan`, cf batched | 64 B / 4 KB | **0.949 -> 1.007 / 0.610 -> 0.644M** | 512 -> 512 / **1152 -> 1024 ns** |

- **uc:** 0-3%, about the run-to-run noise; hold times are within one histogram bucket.
- **cached-flush:** +6% capacity, and 128 ns less hold at 4 KB. That gain is not real
  overlap: the data flushes and the batched `announce[]` flushes now share ONE `mfence`
  instead of two.
- A few runs (minimums 0.47-0.96M) were disturbed by a micro-benchmark I ran concurrently
  by mistake. The medians are unaffected.

The default stays on: neutral on `uc`, and on cached-flush it saves a fence.
