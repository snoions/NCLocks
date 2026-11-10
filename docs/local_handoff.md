# Phase 2a: local handoff (cohort passing)

Design §3.2 / §6.4 (2a). Enabled with `cfg.mode = NCL_MODE_LOCAL`
(`-M local`); cap `cfg.local_cap` (`-K`, default 64).

## Implementation (`src/lock.c`)

- **Release:** if the submutex has a waiter and the consecutive-handoff count
  is below the cap, fence and pass the submutex. The node keeps global
  ownership (`global_held`), with no global traffic and no data
  writeback/invalidate. Otherwise write back the data, release globally, and
  pass the submutex. A waiter that then finds `global_held == 0` acquires
  globally.
- **Thread-obliviousness:** the global path and participant index live in
  node-local lock state, so whichever local thread ends the run does the
  global release. For (c)/(e) the node keeps the grant and the server sees
  only the final REL. Grant slots are only used while waiting.
- **Precise dirty hints:** a local run's hints are accumulated, up to 16
  ranges, and all written back when the lock leaves the node. Overflow, or any
  release without hints, falls back to a full writeback. Writing back only the
  last holder's hints would lose earlier local holders' writes; a mutant doing
  that fails the emu stress test with about 48k checksum errors.
- The `global_held` check runs only in local mode. In the no-submutex ablation
  a thread reaches that point holding nothing.
- Tests: ctest covers all five variants on emu with hint accumulation
  (`-K 4 -H`), overflow fallback (`-H`) and cap-forced releases (`-K 1`),
  plus a `--skip-wb` negative test.

## Results (uc, 4 nodes x 2 threads, 200 ns CS; `results/local_uc_n4t2.csv`)

For (c)/(e) that is 8 workers + 4 servers on 10 physical cores, so two cores
run two hyperthreads.

| alg | capacity phase1 -> local | p99 at 1M phase1 -> local |
|---|---|---|
| Lamport | 1.05M -> 2.50M | 1 ms -> 102 us |
| Tournament | 1.42M -> 1.86M | 8.7 us -> 5.4 us |
| Hybrid (d) | 0.92M -> 1.66M | saturated -> 14 us |
| Server (c) | 1.17M -> 1.80M | 18 us -> 6.4 us |
| Fast-server (e) | 1.10M -> 1.78M | 1 ms -> 24 us |

In closed loop with 2 threads per node, 98.5% of acquisitions are local
handoffs (runs of 65 at the default cap), for every variant.

## Spin backoff dominates fair cross-node handoff (important for all results)

The base algorithms' spin-wait backoff (`ncl_backoff_cap`, 1024 pauses, about
38 us max; now settable with `ncl_bench -B`) decides how quickly a waiter
notices a release. In local mode, waiters on other nodes sit through an
entire 65-acquisition run and back off to the cap, so every fair cross-node
transfer pays up to about 38 us of detection delay.

| local mode, closed loop | B=1 | B=16 | B=128 | B=1024 (default) |
|---|---|---|---|---|
| Tournament throughput / fairness | 2.48M / 1.000 | 2.47M / 1.000 | 2.32M / 1.000 | 1.88M / 0.996 |
| Lamport throughput / fairness | 2.39M / 0.866 | 2.48M / 0.669 | 2.51M / 0.517 | 2.49M / 0.402 |

Fairness is the min/max ratio of per-node completions (`ncl_bench` now
reports it, CSV column `node_min_max_ratio`).

- Lamport's apparent local-mode advantage at the default backoff was
  **unfairness**. It lets the releasing node's next thread win the fast path
  again, so the lock rarely moves. With a small backoff the tournament matches
  its throughput and stays perfectly fair.
- **Phase 1 too:** at 4x1, B=1024, Lamport reached 1.24M at fairness **0.057**
  (one node nearly starved); at B=16 it gives 0.84M at 0.77. The tournament
  (1.52M -> 1.57M) and hybrid (1.09M -> 1.12M) barely change. Earlier Lamport
  capacity figures (~1.05-1.15M at 4 nodes, 1.07M at 8) were bought with
  severe starvation and should not be compared to the fair algorithms
  as-is.
- Smaller backoff means more uncached polling traffic, which matters on real
  CXL. This is the design's "poll backoff caps" sweep (§6.7) and should be
  reported with the fairness ratio for every result.
