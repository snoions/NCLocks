# KV store (`ncl_kv`)

A YCSB-style key-value store on the non-coherent data region (design §6.1, §6.3).
Source: `bench/kv.c`. Sweep: `scripts/cxl_sweep.sh kv`; summary: `scripts/cxl_summary.py
DIR kv`.

## Design

- **Table.** K records (key, version, value of `-V` bytes; 64-byte aligned, so 128 B at
  the default 100 B) striped over L locks: stripe(k) = hash(k) mod L. Each stripe's
  records are contiguous and form the lock's registered data range.
- **Index.** YCSB A/B/C/F never insert, so the key -> record map is static. The parent
  builds it before the nodes fork; each node keeps its own copy (read-mostly metadata,
  design §2). Inserts would need a shared, versioned index.
- **Operations.** read (copy the value), update (new value, version + 1), read-modify-
  write. Workloads A (50/50 read/update), B (95/5), C (read only), F (50/50 read/RMW).
- **Keys.** Uniform or YCSB's scrambled Zipfian (`-z`, 0.99 default): ranks are hashed,
  so hot keys spread over the stripes. Affinity `-F f`: with probability f a request
  picks a key from its node's own partition (keys whose stripe s has s mod N == node),
  with the same distribution inside it. Zipf makes some locks hot but shared by all
  nodes; affinity makes locks owned by one node.
- **Locks.** Any algorithm and mode of the library (`-a`, `-M`, `-K`). `--algmap top:H`
  gives the H stripes with the highest expected load (computed from the key
  distribution) the tournament and the rest Lamport (`cfg.alg_per_lock`,
  `ncl_lock_set_alg`). H=0 is all Lamport, H=L all tournament; the best H bounds what
  any per-lock choice, including a reactive lock, can gain.
- **Open or closed loop** (`-r RATE`: Poisson, latency from the scheduled time).

## Precise data maintenance (`cfg.precise_data`, data layer `include/ncl/data.h`)

A stripe holds many records, so invalidating the whole stripe whenever the lock arrives
at a node (the conservative mode, `--full`) costs thousands of lines per transfer.
Precise mode extends the dirty-range hints of design §2 to reads:

- `ncl_acquire_ranges(t, lock, ranges, n)` names everything the holder will read or
  write (here: the record's lines). When the lock arrives at the node, only those lines
  are invalidated. On a local handoff, lines already invalidated in this node's current
  run of ownership are current (coherent within the node) and are kept; others are
  invalidated, after writing back any part the run dirtied (in hardware `clflushopt`
  does this; the emulator must be told). The node remembers up to 32 such ranges per
  run.
- Release must pass dirty hints (empty if nothing was written). If the run's dirty list
  overflows, it is written back early: precise mode never falls back to writing back the
  whole range, since lines the run never fetched may be stale and would overwrite newer
  data.
- Writes must stay inside acquired ranges: a line written without being invalidated
  first would write stale neighbors back.

**Verification.** Value words are a function of (key, version), so each read checks its
record. At the end the parent reads the device copy, checks every record, and checks
that the versions sum to the number of updates. A stale read of a whole record is
self-consistent (an old version), so it shows up as lost updates in the version sum.
ctests `kv_*` run every mode on emu (and uc if loaded). With `--debug skip-inv`,
`skip-wb`, `no-local-refresh` or `no-dirty-wb` (the last needs long local runs: `-K 256`,
few records per stripe) the emulator must FAIL, and does (`kv_emu_detects_*`).

## Dev box (4x3, cf, 2a, 100k keys, workload A, Zipf 0.99, 1-1.5 s, single runs)

| | L=64 | L=1024 |
|---|---|---|
| all Lamport | 9.0-9.2M, lock p99 5.6 us | 10.2M, 4.4 us |
| tournament on the 1 hottest stripe | 9.9-10.2M, 3.2 us | 11.1M, 2.4 us |
| tournament on the 16 hottest | 10.5M, 2.4 us | 11.3M, 1.9 us |
| all tournament | 10.7M, 2.2 us | 11.1M, 1.8 us |
| server chain | 7.7-7.9M, 3.1 us | 7.8M, 2.9 us |
| all tournament, `--full` | 0.40M (26x slower) | 6.4M |

The dev box's memory is fast, which compresses the differences; the CXL run is the
real test. Uniform keys: all variants within 2% except the server.

## Reactive lock (`-a reactive`, design §4f)

`ncl_kv -a reactive [--react U:D]`: each stripe's lock starts on Lamport and switches to
the tournament when at least U of a node's last 16 global acquisitions met another node,
back when at most D of the last 64 did (default 8:4). CSV columns `tournament_acq_frac` (share of global
acquisitions on the tournament) and `mode_switches`.

First version (consecutive-event trigger, `mode` read on every acquisition, 2:64). Dev box
(4x3, cf, 2a, 1.5 s): the mode choice follows the oracle (Zipf 0.99, L=1024: 26%
of global acquisitions on the tournament vs 28% for the 16 hottest; uniform 0.3%;
affinity 0.9: 1%), but throughput is 6-14% below the best fixed choice everywhere: the
extra `mode` load per global acquisition and, near the threshold, thrashing (up to 43k
switches in 1.5 s at L=64 with affinity; 4:512 cuts that). On the dev box the fixed
choices differ by only a few percent, so this overhead dominates; on CXL they differ by
31% to 5x. Sweep phase `kvreact`.

Current version (INVALID marker in Lamport's `y`, rate trigger, 8:4 / 12:4), same dev-box
setup: 3-7% below the best fixed choice (uniform L=1024: 11.6M vs 12.0M; affinity:
10.9M vs 11.2M; Zipf L=1024: 10.6M vs 11.0M; Zipf L=64: 9.9M vs 10.7M); uniform keys never
promote; 12:4 switches ~100 times per run, 8:4 up to a few thousand, 4:4 thrashes
(50-150k). On Zipf keys it promotes 10-14% of global acquisitions vs the oracle's 28%.

### Isolation: mechanism vs policy (`kviso`)

`-a reactive --algmap top:H --react off` starts the H hottest stripes' locks in tournament
mode (mode word 1, Lamport's `y` = INVALID; `ncl_lock_set_reactive_mode`) and never switches,
so it has exactly the fixed map's assignment and differs from it only by the reactive
mechanism. Pinned all-Lamport (`top:0`) and all-tournament (`top:L`) give the per-path cost.
Dev box (4x3, cf, L=1024): Lamport path -1.5% (uniform keys; the fast path has no `mode`
read), tournament path -8 to -9% (the `mode` read after every tournament acquisition);
pinned to the 16-hottest map under Zipf keys -2 to -6% against the fixed map at the same
tournament share.

After folding the tournament's `mode` read into the root's entry check (one batched round
trip with the root's flag and turn on cached-flush), same dev-box setup: pinned
all-tournament 3-5% below the fixed one (was 8-9%), pinned to the 16-hottest map 0.5%
below the fixed map (was 2-6%).

## Coherent baselines (`kvbase`)

`-b coherent` runs the same KV store with this host's own cache coherence on the CXL
memory: no data writeback or invalidate, metadata as plain cached memory. It is the ideal
case (a coherent CXL device used by one host), an upper bound, not what several hosts
sharing a non-coherent device get. Variants: a `pthread` mutex per stripe without the
node-local submutex (`-m`: every thread contends directly, what a program would do with
coherent memory and atomics) and with it; the tournament and the reactive lock in 2a on
coherent memory (isolates the cost of non-coherence for the same algorithms); and the
non-coherent reactive lock and 16-hottest map on cf.

Dev box (4x3, single short runs): coherent 21-43M ops/s, non-coherent cf 10-12M (2.2-3.6x).
