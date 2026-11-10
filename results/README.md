# Results

Raw CSV from `scripts/sweep_load.sh`; summarize with `scripts/summarize.py <file>`.
All runs: one host (i9-10900K, single socket), `uc` backend (PTE UC-, ~81 ns metadata
load), 8 nodes x 1 thread pinned, 1 lock, 200 ns critical section, 64 B footprint, no
injected latency. Hybrid in both files predates the release-scan reordering (see
docs/hybrid.md), so its capacity there is ~0.92M rather than ~1.04M.

- `load_uc_n8_poisson.csv`: Poisson, 250k-1.75M req/s, 5 interleaved reps, 2 s (0.3 s warmup)
- `burst_uc_n8.csv`: mean 250k/500k; poisson vs onoff 1 ms period, FRAC 0.5/0.25/0.1; 3 reps
- `load_uc_n4_all.csv`: 4 nodes, all five algorithms (new (d) per design §4d, (c), (e));
  Poisson 100k-2.5M req/s, 3 reps, 1.5 s (0.3 s warmup). Servers pinned to CPUs 4-7.
- `precheck_ab_uc_n4.csv`: hybrid (d) baseline (`85e4956`, ver=old) vs reset pre-check
  (ver=new), 4 nodes, Poisson 800k/850k/900k, 8 interleaved reps, 2 s (0.3 s warmup).
- `local_uc_n4t2.csv`: Phase 1 vs Phase 2a local handoff (cap 64), all five algorithms,
  4 nodes x 2 threads, closed loop + Poisson 1M/2M, 3 reps, default spin backoff (1024).
  Predates the fairness column. Lamport numbers here are unfair (see docs/local_handoff.md).
- `rebaseline_uc_n4.csv`: all five algorithms with fairness (`node_min_max_ratio`), spin
  backoff 16 vs 1024; Phase 1 at 4x1 and local (cap 64) at 4x2; closed loop + Poisson
  100k/1M; 3 reps. Fairness is meaningful only for saturated (closed-loop) rows. (e)
  shows fairness 0.00 (fast-path winner starved by the server holding the SLOW side),
  and (d) in local mode shows 0.25 (fast-path monopoly). Both are fixed afterwards (fairness
  1.000); see docs/hybrid.md "Fairness fixes". The (d)/(e) rows predate those fixes.
- `scale_closed_uc_n4n8.csv`: all five algorithms, closed loop, 4 and 8 nodes x 1 thread,
  spin backoff 16, 3 interleaved reps, 2 s (0.2 s warmup). (c)/(e) with both server
  placements (`server_place`: `next` = CPU N+n, `sibling` = SMT sibling of node n's
  worker). Adds `scan_loads_per_acq` and `lamport_retries_per_acq`.
- `load_uc_n8_all.csv`: all five algorithms at 8 nodes x 1 thread, Poisson 100k-1.5M
  req/s, spin backoff 16, servers on SMT siblings, 3 interleaved reps. Workers use CPUs
  0-7 (one per physical core); every server shares a core with its node's worker.
