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
- `-Q batch|serial` (bench and stress; default `batch`) for A/B; CSV column `meta_batch`.
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
  that other cores keep modifying costs coherence traffic here. A possible refinement is
  to probe the hinted slot alone first and batch the rest only on a miss. Not done.
- **Everything else favors uc:** single-word polls, stores, and store-fence-load steps.
  Lock acquisition is 110-200 ns slower on cached-flush for every algorithm.
- **Best of both would be per-region memory types:**
  - uncached for words that are polled or handed off;
  - cached + batched flush for arrays that are scanned (`present[]`, `announce[]`, `b[]`,
    server queues).

  This would bring in the cached-metadata requirements of design §1: no clean writebacks,
  and padding to the host's writeback granularity. Not implemented.
- **Not done yet:** overlapping the data writeback with the release-path loads that do not
  depend on it (2b `pick_waiter`, the chain's `succ` load). That also applies to `uc`,
  where data is cached.
