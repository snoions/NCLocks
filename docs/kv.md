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

## Precise data maintenance (`cfg.precise_data`)

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
