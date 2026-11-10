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
