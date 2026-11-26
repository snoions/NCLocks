# Results on the CXL host (sweep of 2026-11-12)

Host: 2 x 60 cores, CXL 2.0 memory expander as NUMA node 2 attached to socket 0
(`docs/cxl_host.md`). Every run: `numactl --cpunodebind=0 --membind=0`, shared regions
bound to node 2 (`--mem-node 2`), uc pool from `ncl_uc.ko nid=2` (UC-, checked per page),
200 ns CS, `-B 16`, 3 s, 3 interleaved reps, medians; servers `-Y next` (own cores).
Raw CSVs: `results/cxl_20261112_2329/`; tables: `scripts/cxl_summary.py <dir>`. All 1128
runs completed. The spread across reps is mostly below 5%; the largest is 23% (aksrv 4x1 cf).

Configurations:
- `uc_split_clwb`: `-b uc -U split --wb clwb`;
- `cf_clwb`: `-b cached-flush --wb clwb`.

Fairness is per-node min/max completions. All throughputs are k acquisitions/s. Every
phase except "Many locks" uses one lock (`-l 1`), i.e. maximum contention.

The host is still hardware-coherent. CXL contributes latency and device behaviour, while
non-coherence is emulated as before (uc = really uncached, cf = cost stand-in).

## Per-operation cost (`ncl_memlat`, uc pool on node 2)

| | ns |
|---|---|
| 1 UC load | 474-523 |
| UC store + mfence | 610-655 |
| Dekker step (store, fence, load) | ~1000 |
| 7 UC stores, then mfence | 4513 (each ~645: **UC stores serialize**) |
| 7 cached stores + clflushopt, mfence | 1454 |
| 7 cached stores + clwb, mfence | **401** |
| re-read after clwb / after clflushopt | 333 / 711 |
| 7-line scan, split batched / UC serial | 559 / 3030 |

On this platform every per-operation cost is about 5x the dev box, and the writeback
instruction matters: `clwb` keeps the line and its writebacks overlap.

## Phase 1 (closed loop)

Throughput (fairness in parentheses where below 0.99):

| alg | 4x1 cf / uc | 8x1 cf / uc | 16x1 cf / uc | 32x1 cf / uc | 16x3 cf / uc |
|---|---|---|---|---|---|
| tournament | **511 / 457** | **503 / 468** | **503 / 438** | **486 / 359** | **473 / 419** |
| server | 316 / 357 | 326 / 339 | 344 / 331 | 266 / 257 | 309 / 295 |
| aksrv | 263 / 260 | 290 / 290 | 289 / 284 | 244 / 233 | 260 / 265 |
| fastsrv | 263 / 264 | 277 / 270 | 256 / 255 | 230 / 216 | 241 / 232 |
| hybrid | 255 / 217 | 254 / 213 | 246 / 202 | 248 / 189 | 230 / 193 |
| ak | 204 (0.83) / 161 (0.84) | 190 (0.87) / 155 (0.81) | 164 (0.96) / 126 (0.86) | 155 / 118 (0.97) | 157 (0.92) / 118 (0.88) |
| lamport | 120 (0.91) / 106 (0.72) | 70 (0.68) / 54 (0.83) | 38 (0.79) / 27 (0.92) | 19 (0.81) / 13 (0.85) | 36 (0.84) / 27 (0.90) |

- **The tournament wins at every N, and is nearly flat in N.** It loses only 5% from 4 to
  32 nodes on cf, and 21% on uc.
- **The fast paths do not pay under saturation.** Hybrid, fastsrv and aksrv take the
  fast path 0% of the time, so they pay the fast-path cost and then the slow path. AK
  takes it 0-33% of the time and is unfair.
- **cf beats uc by 10-45%** for all but the servers. This is the cost of each serialized
  UC access versus a cached access with a `clwb`.
- **Lamport collapses** as O(N) serialized ~500 ns loads, 13k/s at 32 nodes.

**Ablation** (layout and writeback, 8x1 / 16x1, uc):

| alg | uniform + clflushopt | split + clflushopt | split + clwb |
|---|---|---|---|
| server | 206 / 118 | 300 / 279 | **339 / 331** |
| fastsrv | 186 / 113 | 226 / 208 | 270 / 255 |
| aksrv | 187 / 113 | 269 / 246 | 290 / 284 |
| tournament | 431 / 418 | 442 / 408 | 468 / 438 |
| hybrid | 201 / 192 | 194 / 189 | 213 / 202 |
| ak / lamport | 155 / 127, 49 / 24 | 155 / 127, 54 / 27 | 155 / 126, 54 / 27 |

**The split layout reverses the dev-box verdict for the servers**, which gain +46% at 8
nodes and 2.4x at 16 from split alone (batched polls of the queue rings), and 1.65x /
2.8x with `clwb`. `clwb` adds 6-23% to the other algorithms; AK and Lamport are unchanged.

**Coherent baselines** (`base`, cached memory, real coherence): at T=1, tournament 1.66-1.96 M/s and
pthread 1.29-1.47 M/s; pthread fairness is 0.82-0.97. Node 2 and node 0 give the same results
within noise, because the hot lines stay in the caches. So the non-coherent tournament on
CXL pays 3.4-4.6x versus coherent locking on the same memory.

## Phase 2a (local handoff, `-M local -K 64`, T=3)

Every algorithm reaches 1.2-1.6 M/s, with 98.5% local handoffs, and the tournament is
again first (1.46-1.58 M). Fairness is 1.0 except for the unfair global algorithms, where
the local cap turns their unfairness into node starvation: AK 0.5-0.68, Lamport 0.19-0.91.

## Phase 2b (cross-node handoff, `-M handoff`)

With T=3 the results look like 2a (1.1-1.5 M/s, 98.5% local). With T=1 every handoff is
global:

| alg | 4x1 cf / uc | 8x1 cf / uc | 16x1 cf / uc | 32x1 cf / uc | owner polls/acq at 32 (cf / uc) |
|---|---|---|---|---|---|
| server chain | **397 / 373** | **393 / 361** | **386 / 351** | **356 / 320** | 0 (polls own grant slot) |
| tournament-2b | 325 / 229 | 234 / 145 | 135 / 75 | 67 / 38 | 38 / 64 |
| hybrid-2b | 227 / 178 | 233 / 193 | 183 / 78 | 92 / 28 | 26 / 89 |
| ak-2b | 163 / 123 | 163 / 128 | 124 / 75 | 75 / 39 | 33 / 62 |
| lamport-2b | 200 / 202 | 112 / 108 | 72 / 65 | 39 / 35 | 67 / 70 |

Fairness is 1.0 throughout. Compared with Phase 1 at the same N (tournament uc: 457 /
468 / 438 / 359), **the gate algorithms lose throughput roughly as 1/N in 2b**, down to
about 10% of Phase 1 at 32 nodes, while the server chain is flat. On the dev box at N=8,
tournament-2b reached 55% of Phase 1; here it reaches 31%.

**Hypothesis (to verify): the shared owner field is a hot spot on the device.** The
`-M handoff` waiters all poll one uncached `owner` word, with backoff capped at `-B 16`
pauses (`src/lock.c`, the wait loop after `ncl_base_unlock`). Polls per acquisition grow
as ~2N. At 32 nodes on uc, one handoff takes ~26 us, which means each waiter completes
only about 2 polls per handoff, or ~13 us per poll against a 500 ns unloaded load. That
fits same-address accesses being served one at a time (~400 ns each) at the device or
home agent, with the releaser's handoff store and the new owner's load queued behind the
other waiters' polls. Phase 1 does not see this, because each tournament node has two
contenders and they spin on different lines. The server chain does not see it either,
because every waiter polls its own grant slot (the MCS local-spinning argument, now on a
memory device).

**Backoff check** (tournament, uc split clwb, T=1, 3 reps, spread below 2%):

| -B | 2b 16x1 k/s (owner polls/acq) | 2b 32x1 | Phase 1 16x1 / 32x1 |
|---|---|---|---|
| 16 | 74.8 (31.0) | 38.2 (64.0) | 443 / 362 |
| 64 | 78.1 (29.5) | 40.1 (60.8) | 443 / 369 |
| 256 | 89.6 (25.4) | 44.8 (54.0) | 444 / 377 |
| 1024 | **103.5** (21.0) | **61.5** (38.4) | 442 / 380 |
| 4096 | 57.8 (18.8) | 56.8 (25.5) | 439 / 383 |

The check supports the hypothesis. Up to `-B 1024`, the handoff interval (1/throughput)
equals owner polls per acquisition × 420-460 ns at both N. The owner line serves one
access at a time, and every handoff waits behind all queued polls. Slower polling helps,
by +38% at 16 nodes and +61% at 32. At 4096 pauses the polls no longer saturate the line
(~900 ns per poll), but the new owner notices its handoff late, so throughput drops again.
That is the classic global-spinning trade-off. Even at its best, tournament-2b stays at
23% of Phase 1 at 16 nodes and 16% at 32, and far below the server chain (351k / 320k).
**Backoff tuning cannot fix this. The waiters must stop sharing a polled line.**

Fix, `--wait own` (`docs/handoff.md`, "Own-line wait"; now the default): the releaser
writes `owner = j`, then a grant word in j's own announce line. Waiters poll that line.
In the version measured below they re-read `owner` every 16th poll. The interval now
doubles after each re-read (16, 32, 64, ...) because of the Lamport result below.

**`--wait own` vs `owner`** (`results/cxl_20261113_0334/own.csv`, 3 reps, medians;
k acq/s, cf / uc split):

| alg | 4x1 | 8x1 | 16x1 | 32x1 | 16x3 |
|---|---|---|---|---|---|
| tournament owner | 331 / 227 | 241 / 142 | 137 / 75 | 69 / 39 | 1269 / 1156 |
| tournament **own** | 314 / 261 | 315 / 250 | **302 / 233** | **246 / 185** | 1411 / 1392 |
| hybrid owner | 219 / 181 | 221 / 194 | 173 / 80 | 91 / 28 | 1275 / 1163 |
| hybrid **own** | 223 / 182 | 274 / 209 | 266 / 191 | **243 / 174** | 1424 / 1404 |
| ak owner | 167 / 128 | 167 / 128 | 123 / 80 | 77 / 40 | 1288 / 1148 |
| ak **own** | 181 / 125 | 201 / 141 | 196 / 134 | 183 / 127 | 1419 / 1396 |
| lamport owner | 215 / 209 | 121 / 111 | 71 / 65 | 40 / 37 | 1266 / 1147 |
| lamport **own** | 322 / 274 | 303 / 242 | 155 / 90 | 44 / 32 | 1423 / 1404 |

Fairness is 1.0 everywhere, there are no missed announces, and the rep spread is mostly
below 5%. The worst spread is lamport own cf at 16x1, 39%.
- **The 1/N collapse is gone for tournament, hybrid and AK.** At 32 nodes they run
  2.4-6x faster than with `owner`, and tournament keeps 78% (cf: 80%) of its 8-node
  throughput. Owner polls per acquisition fall from ~2N to 3-9, and lock p99 at 32x1
  drops from 0.6-1.4 ms to 0.15-0.33 ms.
- **With local handoff (16x3), `own` adds 10-22%.** It reaches 1.39-1.42 M/s, about the
  same as 2a without cross-node handoff (tournament 1.46 M).
- **Lamport at 32 nodes does not improve.** Its waits are long (406-523 grant polls per
  acquisition), so the every-16th-poll rechecks still put 25-32 owner loads on each
  acquisition, ~13 us of owner-line time per handoff. The fixed recheck interval
  reintroduces the hot spot whenever waits are long. The doubling interval (now the
  default) bounds this to O(log wait); see below.
- **Doubling re-read** (`results/cxl_20261113_0423/own.csv`, same sweep; `owner` mode as
  the control matches the run above within ±6.5%, mean −0.2%). Owner polls per
  acquisition with `own` fall to 2-4.6 at 32 nodes, from 3.6-32 with the fixed interval:

  | `own`, cf / uc | fixed every 16th | doubling |
  |---|---|---|
  | lamport 16x1 | 155 / 90 | **210 / 124** (+36-38%) |
  | lamport 32x1 | 44 / 32 | **65 / 46** (+42-47%) |
  | tournament 32x1 | 246 / 185 | **290 / 222** (+18-20%; lock p99 147 → 115-147 us) |
  | hybrid / ak 32x1 | 243 / 174, 183 / 127 | 254 / 180, 188 / 131 (+3-4%) |

  Everything else is unchanged within noise (±5%), fairness is 1.0, and there are no
  missed announces. Lamport at 32 nodes is still slow (45-65k). Its owner traffic is now
  small (4.6 loads per acquisition), so the remaining cost is its gate and ~420 grant
  polls per wait, not the owner line.
- **Still below the alternatives at T=1.** Tournament-2b with `own` (uc 240k / 222k at
  16 / 32 nodes, doubling) stays below both Phase 1 (438k / 359k) and the server chain (351k /
  320k). The gate, announce and grant cost more than they save when every handoff
  crosses nodes.

### Shorter handoff path (`ho` phase, `--ho`)

`results/cxl_20261114_0711/ho.csv`: cf (`cf_clwb`) only, 3 reps, 336 runs, spread
<=10% except Lamport at L=1 (up to 28%). `errors.log` holds only the uc preflight's
`/dev/ncl_uc` "Permission denied"; no run failed. Options: `none`, `grant,clear`, and
`all`, which in this run was `grant,clear,prepick` (`prepick` has since been removed; see
below). k/s:

| T=1, L=1 | 4x1 | 16x1 | 32x1 | Lamport gate retries/acq at 16 / 32 |
|---|---|---|---|---|
| Phase 1 tournament | 510 | 504 | 499 | |
| server chain | 403 | 388 | 365 | |
| tournament-2b none -> grant,clear | 314 -> **429** | 304 -> **455** | 292 -> **462** | |
| hybrid-2b | 223 -> 238 | 273 -> 265 | 259 -> 258 | |
| ak-2b | 183 -> 185 | 202 -> 188 | 191 -> 183 | |
| lamport-2b | 333 -> 204 | 185 -> **65** | 71 -> **38** | 1.0 -> 5.8 / 4.3 -> 9.3 |

- **`grant,clear` helps only the tournament** (1.37-1.58x, now 85-93% of Phase 1 and
  above the server chain). Hybrid and AK do not move, and **Lamport gets 1.6-2.8x
  slower**, with its gate retrying 5.8-9.3 times per acquisition.
- **The gate is the bottleneck.** At saturation every acquisition still passes the gate
  once (`gate_entries_per_acq` = 1.0) to read "held". Explained, with the `skip` run,
  in the next subsection.
  This led to `--ho skip` (read `owner` before the gate, adaptively; docs/handoff.md),
  which takes the gate off every acquisition at saturation; measured below.
- **`prepick` did nothing on CXL either:** it found the next waiter in its snapshot on
  every handoff at L=1, but `all` was 0.96-1.0x of `grant,clear`. It was removed.
- **T=3 (98.5% local handoffs), L=1:** every 2b variant reaches 1.43-1.47M, whatever
  the options; the server chain gets 1.40M and Phase 1 tournament 480k.
- **L=256:** `grant,clear` gives 1.02-1.07x for all four gate algorithms (16x1:
  tournament 1548 -> 1600, Lamport 2443 -> 2592; 32x1: Lamport 4485 -> 4804). Lamport-2b
  beats Phase 1 tournament (2076 / 3351) but not the server chain (3031 / 3352 at 16x1 /
  32x1). At 16x3: Lamport-2b 6494, server chain 6882, Phase 1 tournament 5722.
- **Fairness:** 1.0 at L=1 for every option; 0.93-0.99 at L=256 (server chain 0.80 at
  32x1). p99 at L=1: 2b tournament 10-74 us with `grant,clear` vs 11-115 us with `none`;
  Lamport `grant,clear` 0.4-1.5 ms.

#### With `skip` (`results/cxl_20261114_0836/ho.csv`)

Same phase after `4134c29`, `all` = `grant,clear,skip`; cf, 3 reps, 336 runs, no
errors. k/s, T=1, L=1 (`none` / `grant,clear` / **`grant,clear,skip`**):

| | 4x1 | 16x1 | 32x1 |
|---|---|---|---|
| Phase 1 tournament | 509 | 503 | 506 |
| server chain | 394 | 382 | 367 |
| tournament-2b | 326 / 421 / **444** | 301 / 460 / **473** | 298 / 465 / **503** |
| hybrid-2b | 229 / 240 / **483** | 262 / 269 / **475** | 257 / 256 / **505** |
| ak-2b | 182 / 179 / **466** | 201 / 193 / **471** | 186 / 175 / **480** |
| lamport-2b | 330 / 184 / **505** | 211 / 67 / **471** | 69 / 39 / **486** |

- **`skip` fixes the gate collapse.** Gate entries per acquisition go from 1.0 to 0 at
  saturation, and all four gate algorithms land at 444-505k: 0.87-1.0x Phase 1
  tournament, 1.2-1.4x the server chain. Lamport goes from 39k to 486k at 32 nodes.
  p50 at 32 nodes is 62-65 us for every gate, against 61 us for Phase 1. Fairness
  1.0; no missed announces.
- **2b at T=1 now matches Phase 1** (at 16-32 nodes within 6%, at 4 nodes 0.87-0.99x),
  and the gate algorithm no longer matters at saturation. It still does not beat it:
  the handoff floor (one store seen by one waiter) is the same.
- **Where the gain came from: the gate was a second mutex in series.** Without `skip`,
  every acquisition passes the gate once, and the gate is itself a mutual-exclusion
  lock, so 2b throughput can be no higher than the gate's own contended throughput.
  It isn't on the handoff path (the node in the gate is not the next to be granted),
  but it caps the rate. The numbers match: 2b `grant,clear` runs at about the
  algorithm's own Phase 1 throughput (16x1 / 32x1: hybrid 269 / 256 vs Phase 1 242 /
  240, AK 193 / 175 vs 166 / 151, Lamport 67 / 39 vs 38 / 19, tournament 460 / 465 vs
  516 / 505). It is flat in N, as a single serial resource would be. 2b is slightly above
  Phase 1 because the gate's critical section is one owner load instead of the
  workload's CS and data writeback. `grant` made handoffs faster, so gate arrivals
  came denser. That hurts only Lamport, whose throughput collapses with contention
  (Phase 1: 120k at 4 nodes, 19k at 32); with the slower `none` handoff its gate saw
  less contention (211k at 16 nodes). With `skip`, the only serial resource left is
  the grant chain, whose rate is set by the handoff (~2 us on CXL) and not by the
  gate, so all four reach the same ~470-505k. The tournament gains least, because
  its gate already ran at ~500k. On the dev box (uc split) the gate's ~1.7M exceeded
  the ~1.05M handoff rate, so `skip` changed nothing there.
- **L=256 and T=3 are unchanged** (adaptive: the early read never fires at L=256):
  e.g. 16x1 L=256 Lamport-2b 2578, tournament-2b 1593, server chain 3055, Phase 1
  tournament 2067 (Phase 1 Lamport 3890 in the locks phase); T=3 L=1 all 1.44-1.45M.

## Many locks (Phase 1, `-l L`, T=1)

`results/cxl_20261113_0923/locks.csv` (`scripts/cxl_sweep.sh locks`): each acquisition
picks one of L locks uniformly, so contention per lock falls as L grows. Throughput k/s,
cf / uc:

| alg | 16x1 L=1 | 16x1 L=16 | 16x1 L=256 | 32x1 L=1 | 32x1 L=16 | 32x1 L=256 |
|---|---|---|---|---|---|---|
| tournament | 516 / 441 | 1881 / 930 | 2084 / 939 | 505 / 368 | 2782 / 737 | 3364 / 757 |
| server | 338 / 314 | **2309 / 2103** | 3095 / 2593 | 280 / 252 | **2301 / 1914** | 2453 / 1997 |
| lamport | 38 / 27 | 750 / 492 | **3890 / 2699** | 19 / 13 | 319 / 216 | **7100 / 2458** |
| hybrid | 242 / 205 | 1290 / 787 | 2234 / 1008 | 240 / 195 | 1571 / 543 | 3574 / 835 |
| ak | 166 / 125 | 1179 / 608 | 1653 / 671 | 151 / 119 | 1688 / 483 | 3069 / 636 |
| fastsrv | 255 / 252 | 1333 / 1032 | 2206 / 1162 | 226 / 215 | 1497 / 974 | 2249 / 884 |
| aksrv | 287 / 276 | 1309 / 740 | 1649 / 617 | 250 / 236 | 1627 / 728 | 2223 / 544 |

Fast-path fraction (hybrid, ak, fastsrv, aksrv) is 0-6% at L=1, 17-52% at L=16 and
87-95% at L=256. Lamport's is 73-75% already at L=4 but with starvation-level tails
(p99 1.4-7.9 ms up to L=16 at 32 nodes; fairness 0.82-0.87 at 32 nodes for L<=64). All
runs completed, no errors; spread mostly <5% (max 16%: server uc 32x1 L=64).

- **The winner depends on contention.** L=1: tournament (as Phase 1). L=4-64: the server
  on uc (2.1-2.6M at 16 nodes, ~1.9-2.0M at 32), the tournament or server on cf.
  L=256 (nearly uncontended): Lamport, whose fast path is the shortest (cf 3.9M / 7.1M;
  uc 2.7M / 2.5M). Lamport is unusable at L<=16.
- **uc stops scaling with L or N; cf does not.** At L=256, time per acquisition per
  thread (N / throughput) on uc roughly doubles from 16 to 32 nodes for every gate
  algorithm (tournament 17 -> 42 us, hybrid 16 -> 38, ak 24 -> 50, Lamport 6 -> 13),
  while cf stays nearly flat (tournament 8 -> 10, hybrid 7 -> 9, Lamport 4 -> 4).
  Uncontended, the tournament at 32 nodes is 5 Peterson levels, ~15 us at the
  single-thread costs above, so per-access UC latency rises with concurrent UC traffic:
  there seems to be an aggregate ceiling on UC metadata accesses to the CXL node
  (tournament uc *falls* from 939k to 757k as N doubles). The `dram` phase would show
  whether it is UC or CXL. Data handling is the same in both configs and 64 B per lock.
- **The fast paths pay for their reopen scan on uc.** At L=256 hybrid / ak / fastsrv /
  aksrv take the fast path ~90-95% of the time, yet on uc reach only 0.5-1.2M, below
  Lamport (2.5-2.7M) and the server: the strict `present[]` scan before reopening (hybrid,
  fastsrv) and AK's longer fast path add UC accesses under that ceiling. On cf, hybrid
  and ak at 32x1 L=256 (3.6M / 3.1M) beat the tournament (3.4M) but not Lamport.
- **The server saturates and turns unfair.** Server throughput is flat from L=64 to
  L=256 (cf 3.1M / 2.5M, uc 2.6M / 2.0M at 16 / 32 nodes) and fairness drops to
  0.78-0.82 (16 nodes) and 0.64-0.66 (32 nodes) at L=256, on both configs. Not yet
  explained: the per-server load is uniform (home = hash % N), and at 16 nodes the 16
  servers + 16 workers have their own cores, so CPU placement alone does not explain it.

## Open loop (Poisson, N=16, T=1)

Sojourn p50 / p99 in us. The capacity column is the highest offered load served with
less than 0.1% unserved:

| alg | 25k cf / uc | 200k cf / uc | capacity cf / uc |
|---|---|---|---|
| server | 6.1 / 6.4, p99 16 / 16 | 5.1 / 5.9, p99 20 / 24 | 300k / 300k (400k: 17-22% unserved) |
| tournament | 7.2 / 9.7, p99 12 / 16 | 8.7 / 11.8, p99 26 / 39 | **>=400k / >=400k** |
| hybrid | 6.7 / 7.7 | 37 / 205 | 250k / 200k |
| fastsrv | 6.9 / 7.9 | 12 / 13 | 250k / 250k |
| aksrv | 9.7 / 12.3 | 12 / 13 | 250k / 250k |
| ak | 9.2 / 12.3 | saturated | 150k / 100k |

- **At light load the server has the lowest median, and the tournament has the lowest
  p99.** Uncontended, the hybrid fast path saves 0.5 us on cf and 2 us on uc
  (p50 6.7 / 7.7 vs 7.2 / 9.7).
- The tournament is the only algorithm unsaturated at 400k.

## UC metadata on DRAM (`dram` phase)

`results/cxl_20261113_2050/dram.csv`: `uc_split_clwb` at 8x1 and 16x1, one lock, with
the uc pool and the data on node 0 (`ncl_uc.ko nid=0`; every row has `uc_pool_node=0`,
`mem_node=0`, `UC-`). All 42 runs completed. `errors.log` has one failed preflight
(`/dev/ncl_uc` permission) from an earlier attempt. Compared with the same config on CXL
(Phase 1) and with cf on CXL, k/s:

| alg | uc DRAM 8 / 16 | uc CXL 8 / 16 | DRAM / CXL | cf CXL 8 / 16 |
|---|---|---|---|---|
| tournament | **1132 / 1141** | 468 / 438 | 2.4-2.6x | 503 / 503 |
| server | 826 / 794 | 339 / 331 | 2.4x | 326 / 344 |
| aksrv | 731 / 685 | 290 / 284 | 2.4-2.5x | 290 / 289 |
| fastsrv | 656 / 606 | 270 / 255 | 2.4x | 277 / 256 |
| hybrid | 531 / 552 | 213 / 202 | 2.5-2.7x | 254 / 246 |
| ak | 417 / 340 | 155 / 126 | 2.7x | 190 / 164 |
| lamport | 172 / 91 | 54 / 27 | 3.2-3.4x | 70 / 38 |

Fairness is unchanged in kind: 1.0 for tournament, server and the server variants; AK
0.69 / 0.80 and Lamport 0.59 / 0.75, a little *less* fair than on CXL (0.81-0.92).
p99 drops by the same ~2.5x.

- **CXL costs a uniform ~2.4-2.7x at one lock** (Lamport 3.2-3.4x). The algorithm
  ranking does not change, so the CXL latency multiplies every algorithm's
  critical path about equally.
- **uc on DRAM beats cf on CXL by 2.2-2.4x.** So on this host most of uc's
  disadvantage against cf comes from UC accesses having to go to CXL: each UC
  access pays the full CXL round trip, while cf's clwb keeps lines and overlaps
  writebacks (per-operation table above).

### Many locks with UC metadata on DRAM

`results/cxl_20261113_2101/locks.csv`: the locks phase with `MEMNODE=0
CFGS=uc_split_clwb LOCKS="16 256"` (pool on node 0, checked per row), 84 runs, no errors,
spread <=3%. k/s, then time per acquisition per thread (N / throughput) in us:

| alg | DRAM uc 16x1 L=256 | DRAM uc 32x1 L=256 | CXL uc 16 / 32 | CXL cf 16 / 32 |
|---|---|---|---|---|
| tournament | 4355, 3.7 us | 7084, 4.5 us | 939 / 757 (17 / 42 us) | 2084 / 3364 (8 / 10 us) |
| lamport | 8585, 1.9 | **15946, 2.0** | 2699 / 2458 (6 / 13) | 3890 / 7100 (4 / 4) |
| hybrid | 4798, 3.3 | 8252, 3.9 | 1008 / 835 (16 / 38) | 2234 / 3574 (7 / 9) |
| ak | 3465, 4.6 | 6543, 4.9 | 671 / 636 (24 / 50) | 1653 / 3069 (10 / 10) |
| fastsrv | 4649, 3.4 | 7777, 4.1 | 1162 / 884 (14 / 36) | 2206 / 2249 (7 / 14) |
| aksrv | 3440, 4.7 | 6329, 5.1 | 617 / 544 (26 / 59) | 1649 / 2223 (10 / 14) |
| server | 8099, 2.0 | 12842, 2.5 | 2593 / 1997 (6 / 16) | 3095 / 2453 (5 / 13) |

- **The uc ceiling is CXL-specific.** On DRAM, uc throughput nearly doubles from 16 to 32
  nodes and the per-thread time stays flat (tournament 3.7 -> 4.5 us, Lamport 1.9 -> 2.0).
  On CXL it doubles or triples instead. Since cf on CXL scales too, the limit is not
  CXL bandwidth in general but **uncached accesses to the CXL device under concurrency**:
  at 32 nodes, L=256, DRAM is 6-12x faster for uc versus 2.4-2.7x at one lock. A
  candidate (confirmed below, "Device concurrency") is that 8-byte UC stores reach the device as partial writes (`MemWrPtl`) that
  it serves slowly or serially; untested (it would need device-side counters or a
  memlat run with concurrent UC streams).
- **With the ceiling gone, uc beats cf on CXL everywhere** (tournament 7.1M vs 3.4M,
  Lamport 15.9M vs 7.1M at 32x1 L=256), and the many-locks ranking is the same as on CXL
  cf: Lamport, then the server, at L=256; the server at L=16 (5.8M / 6.5M).
- At L=16 Lamport is still the worst at 32 nodes (827k, p99 tails), as on CXL.
- **Server unfairness persists but is smaller:** 0.94 at 16x1 and 0.80 at 32x1, L=256
  (CXL 0.78 / 0.64). It grows with load on the servers, so it is a server-side effect, not
  a CXL one.

## Device concurrency (`conc` phase, `ncl_memconc`)

`results/cxl_20261117_0804/conc.csv`: 3 reps, T = 1-60 threads on socket 0 (one per
core), no errors. Configs: `uc` (metadata ops on the kmod pool, node 2 = CXL), `cf_cxl`
(everything on node 2), `cf_dram` (node 0). The `uc` config's data ops (`wb64`, `nt*`,
`*batch`) ran on DRAM (the sweep did not bind its data region; fixed since) and match
`cf_dram`. Aggregate Mops/s; `line GB/s` = Mops x 64 B:

| op | T=1 | T=8 | T=16 | T=32 | T=60 | at T=60 |
|---|---|---|---|---|---|---|
| uc `load` (CXL) | 2.6 | 19.4 | 38.1 | 74.8 | **137.6** | 436 ns/op, flat |
| uc `store` (CXL) | 1.9 | 9.2 | 9.0 | 9.0 | **9.0** | 6.7 us/op |
| uc `storef` (CXL) | 1.8 | 9.2 | 9.0 | 9.0 | **9.0** | |
| uc `dekker` (CXL) | 1.0 | 4.7 | 6.4 | 7.3 | 6.0 | |
| `nt8` (CXL) | 2.9 | 8.5 | 8.4 | 8.4 | **8.4** | 7.2 us/op |
| `nt64` (CXL) | 2.9 | 25.0 | 50.5 | 100.1 | **179.8** | 11.5 GB/s |
| `wb64`, cf `store` (CXL, full line + clwb) | 2.4 | 21.1 | 42.7 | 84.6 | **153-174** | ~10-11 GB/s |
| `rdbatch` (CXL) | 40 | 307 | **374** | 275 | 263 | peak 24 GB/s |
| `wbbatch` (CXL) | 69 | **411** | 252 | 176 | 200 | peak 26 GB/s |
| `nt8` (DRAM) | 4.7 | 38.1 | 76.6 | 154.0 | 287.3 | scales |
| `rdbatch` / `wbbatch` (DRAM) | 86 / 90 | 670 / 715 | 1325 / 1444 | 2490 / 2407 | 4099 / 3060 | 262 / 196 GB/s |
| `shload` uc / cf CXL / cf DRAM | 2.3 / 2.0 / 4.6 | 2.6 / 2.8 / 6.7 | | | 2.6 / 2.8 / 6.9 | one line, serialized |

- **The uc ceiling is a cap on partial writes, not device bandwidth.** Every 8-byte
  write to the CXL node, UC store or 8-byte movnti from write-back memory alike, stops at
  **8.4-9.0M/s in total from T=8 on**, about 0.55 GB/s even counting a full line each:
  1/45 of the 24-26 GB/s the device sustains for full-line traffic. Full-line writes
  through the same path (`nt64`, `wb64`, cf stores) scale linearly to 150-180M/s at
  T=60, and so do UC loads (138M/s, latency flat at 436 ns). On DRAM, `nt8` scales like
  `nt64`. So the device (or the host's CXL path) serves partial writes (`MemWrPtl`) at a
  fixed aggregate rate. The memory type does not matter: `nt8` and the UC store hit
  the same cap.
- **It accounts for the lock measurements.** uc locks write a few metadata words per
  acquisition (tournament: flag and turn per level plus the release; Lamport: x, y,
  b[], release), and the uc rates at the "Many locks" plateau (tournament 757k, Lamport
  2.5M at 32x1 L=256) times those counts land at the ~9M/s cap. cf writes whole lines
  (store + clwb) and is not capped, which is why cf beat uc in every phase.
- **Device bandwidth itself:** 24 GB/s read (T=16) and 26 GB/s write (T=8), against
  262 / 196 GB/s for local DRAM; both fall by a third or more with more threads, with
  large rep spread (oversubscribed device queues).
- **Same-line loads are served one at a time** on every backend (2.6-2.8M/s on CXL,
  6.9M/s on DRAM), which is why waiters poll their own line (`--wait own`).

## Open loop with 2b (`open2b` phase, N=16, T=1, cf)

`results/cxl_20261117_0804/open2b.csv`: 3 reps, no errors. Lock latency p50 / p99 in
us (from the scheduled arrival, no coordinated omission); 2b = `--ho all`
(`grant,clear,skip`):

| L=1, offered | 25k | 100k | 200k | 300k | 400k | 450k |
|---|---|---|---|---|---|---|
| Phase 1 tournament | **5.4 / 10** | **5.4 / 14** | 6.1 / 25 | 8.2 / 41 | **12.8 / 98** | **25 / 254** |
| Phase 1 Lamport | 2.2 / 9 | saturated (37k) | | | | |
| server chain | 5.4 / 15 | **3.7 / 15** | **4.4 / 16** | **6.7 / 31** | 2% unserved | 18% unserved |
| 2b tournament `-X adaptive` | 7.9 / 29 | 8.7 / 51 | 12.3 / 61 | 18.4 / 98 | 74 / 459 | 279 / 1638 |
| 2b tournament `-X scan` | 7.9 / 15 | 8.7 / 25 | 9.2 / 39 | 15.4 / 94 | 78 / 459 | 426 / 2621 |
| 2b tournament `--ho none` | 8.7 / 31 | 9.2 / 57 | 24 / 127 | 393 / 2490 | 30% unserved | |
| 2b Lamport `-X adaptive` | 4.4 / 18 | 4.9 / 82 | 16.4 / 197 | 25 / 221 | 78 / 475 | 328 / 2032 |
| 2b Lamport `-X scan` | 4.1 / 12 | 4.6 / 41 | 7.9 / 82 | 16.4 / 111 | 74 / 442 | 279 / 1770 |
| 2b hybrid `-X scan` | 7.2 / 25 | 7.9 / 45 | 12.3 / 61 | 18.4 / 102 | 82 / 492 | 246 / 1573 |

| L=256, offered | 250k | 1M | 1.5M | 2M | 2.5M | 3M |
|---|---|---|---|---|---|---|
| Phase 1 Lamport | **2.3 / 6** | **2.3 / 9** | **2.4 / 12** | **2.7 / 16** | **4.4 / 25** | **6.1 / 39** |
| Phase 1 tournament | 5.1 / 13 | 5.6 / 29 | 11 / 59 | 94 / 721 | 21% unserved | |
| server chain | 6.4 / 22 | 5.1 / 27 | 5.9 / 31 | 7.4 / 41 | 12 / 74 | 127 / 88k |
| 2b Lamport `-X adaptive` | 4.4 / 10 | 4.6 / 21 | 6.4 / 32 | 11 / 64 | 74 / 590 | 16% unserved |
| 2b tournament / hybrid | 8 / 21 | 12 / 60 | 60-130 / 0.4-11k | ~24% unserved (cap ~1.6M) | | |

Closed loop, L=256 (`-X adaptive` -> `scan`, k/s): tournament 1582 -> 1532 (16x1), 2550 ->
2410 (32x1); Lamport 2583 -> 2311, 4769 -> 3894. Fairness 0.89-0.99 either way.

- **2b costs latency whenever the lock is not saturated.** At L=1 its median is
  2.5-3.3 us above Phase 1 tournament (7.9-8.7 vs 5.4 us at 25-100k, +50%), its p99 2-4x
  higher, and near saturation (400-450k) its queueing is far worse (p50 74-426 vs 13-25
  us), because its capacity (~470k closed loop) is slightly below Phase 1's (~500k).
  There is no load at which 2b tournament beats Phase 1 tournament on CXL. (At 4-16
  nodes Phase 1 tournament is also fair, 0.999.)
- **The adaptive pick misses 6-14% of announces at L=1** below saturation (a holder that
  got the lock uncontended probes one slot); a missed waiter waits for its next owner
  re-read. `-X scan` removes the misses and halves p99 (2b tournament 29 -> 15 us at
  25k, 51 -> 25 at 100k) at the same median. But on CXL the scan costs closed-loop
  throughput when locks are mostly uncontended: 3-6% for the tournament and 11-18% for
  Lamport at L=256 (16 / 32 slots). Neither policy is right everywhere; the default stays
  `adaptive`.
- **The server chain has the best median and tail from 50k to 300k at L=1** (3.7-6.7 /
  15-31 us) but saturates below 400k. At L=256 it is second to Phase 1 Lamport and serves
  ~3M.
- **Phase 1 Lamport is unusable at one lock** (saturates at 37k) and **best at 256
  locks** at every load (p50 2.3-6.1 us up to 3M/s): the open-loop picture matches the
  closed-loop many-locks results.
- `--ho none` (the old 2b default) saturates at ~300k: the `--ho` default is confirmed in
  open loop too.

## Multi-threaded nodes (`mt` phase, T>1, cf)

`results/cxl_20261117_2215/mt.csv`: 3 reps, 564 runs, no errors, rep spread <=7.6%
(median 0.8%). 16x3 and `-K 64` unless noted; 2b = `--ho all`. Nodes run several threads
in the deployment, so this is the main result set; the T=1 sections above are worst
cases.

**Local-handoff cap** (L=1; k/s, lock p99 in us):

| `-K` | 8 | 16 | 64 | 256 |
|---|---|---|---|---|
| tournament-2a | 1177, 123 | 1317, 205 | 1454, 688 | 1496, 2621 |
| tournament-2b / Lamport-2b | 1157, 123 | 1286-1288, 205 | 1444-1450, 688 | 1495-1504, 2621 |
| server chain | 1064, 139 | 1212, 229 | 1390, 721 | 1453, 2753 |
| Lamport-2a (fairness 0.81-0.83) | 274, 1901 | 442, 1770 | 924, 1704 | 1316, 410 |
| local handoff fraction | 0.889 | 0.941 | 0.985 | 0.996 |

- **The cap sets a protocol-independent trade-off:** K=8 -> 256 gives +27-29% throughput
  for 21x the p99 (123 us -> 2.6 ms). Every protocol except Lamport-2a sits on the same
  curve, with fairness 1.0: once ~90% of handoffs are local, the global algorithm hardly
  matters.
- **Lamport-2a fails on a hot lock** at every cap (274-1316k, fairness 0.81-0.83, p99
  up to 1.9 ms).

**Number of locks** (k/s; fairness in parentheses where below 0.93):

| | L=1 | 4 | 16 | 64 | 256 |
|---|---|---|---|---|---|
| tournament-2a | **1444** | **2374** | **4304** | 5517 | 5797 |
| tournament Phase 1 | 481 | 1621 | 3841 | 5314 | 5808 |
| tournament-2b | 1443 | 2046 | 2813 | 3358 | 4081 |
| Lamport-2a | 922 (0.87) | **489** | 1534 | 5197 | **9645** |
| Lamport Phase 1 | 36 (0.85) | 143 (0.83) | 587 (0.82) | 2386 (0.82) | **9591** |
| Lamport-2b | 1455 | 2003 | 2257 | 4577 | 6418 |
| server chain | 1392 | 2017 | 4145 (0.88) | 5777 (0.89) | 6913 (0.91) |
| server Phase 1 | 305 | 1127 | 2651 | **5830** (0.90) | 6832 (0.90) |

- **Hot to warm locks (L <= 16): tournament-2a** is first or tied, with the lowest p99 from
  L=4 on (13.8-47 us).
- **Cold locks (L=256): Lamport** (2a = Phase 1, nobody local to pass to): 9.6M, **1.66x**
  tournament-2a, fairness 0.95. The crossover is between L=64 (tournament-2a, server and
  Lamport-2a within 12%) and L=256.
- **Lamport is unusable while a lock is hot**, even with 2a: 489k at L=4 (tournament-2a
  2374k, 4.9x), 922k at L=1.
- **Gate-based 2b never beats 2a**: tournament-2b loses 14-35% from L=4 on. Lamport-2b
  fixes Lamport's hot-lock collapse (1455k at L=1, fair) but trails the best choice at
  every L (tournament-2a when hot, Lamport-2a when cold: 6418 vs 9645 at L=256).
- The server leads only at L=64 (5.8M, tied with tournament-2a within 6%) and gets
  unfair (0.88-0.91) from L=16 on.

**Threads per node** (K=64): at L=1 every cohort variant gives 1.40-1.58M at 4x3, 16x2,
16x3, 16x4 and 32x2 (Phase 1 tournament 415-486k). At L=256, tournament-2a = Phase 1
(4.0-7.3M) and Lamport-2b (4.7-7.9M) or the server chain (5.2-8.0M) lead; Lamport-2a was
not in this part.

**Open loop** (lock p50 / p99 in us):

| L=1, offered | 100k | 250k | 500k | 1M | 1.25M |
|---|---|---|---|---|---|
| tournament-2a | 5.6 / **13.8** | 6.9 / 24.6 | **13.3 / 55** | **47 / 172** | **94 / 311** |
| tournament Phase 1 | 5.6 / 15.4 | 7.4 / 34.8 | 5% unserved | saturated (~480k) | |
| server chain | **4.1** / 20.5 | **5.6 / 18.4** | 22.5 / 74 | 78 / 246 | 156 / 475 |
| tournament-2b | 9.2 / 53 | 12.3 / 57 | 24.6 / 90 | 57 / 197 | 111 / 360 |
| Lamport-2b | 5.4 / 74 | 15.9 / 180 | 25.6 / 94 | 55 / 197 | 115 / 360 |
| Lamport Phase 1 | saturated (36k) | | | | |

| L=256, offered | 500k | 1M | 2M | 3M | 4M | 5M | 6M |
|---|---|---|---|---|---|---|---|
| Lamport Phase 1 | **2.4 / 5.9** | **2.4 / 6.9** | **2.4 / 9.2** | **2.6 / 11.8** | **2.7 / 14.8** | **2.8 / 20.5** | **4.4 / 27.6** |
| server chain | 5.4 / 21.5 | 4.4 / 20.5 | 4.1 / 19.5 | 4.6 / 20.5 | 5.1 / 24.6 | 7.2 / 36.9 | 14.9 / 90 |
| tournament-2a (= Phase 1) | 5.6 / 12.8 | 5.6 / 15.4 | 5.9 / 22.5 | 6.4 / 34.8 | 11.3 / 55 | 22.5 / 148 | 4.5% unserved |
| Lamport-2b | 4.6 / 10.8 | 4.6 / 13.3 | 4.9 / 19.5 | 5.1 / 27.6 | 7.2 / 41 | 11.3 / 70 | 37 / 238 |
| tournament-2b | 8.7 / 20.5 | 8.7 / 28.7 | 9.2 / 47 | 17.4 / 98 | saturated at ~4.06M | | |

- **Hot lock: tournament-2a** has the best tail at every load and the best median from
  500k on; the server chain has the best median at light load (4.1-5.6 us up to 250k).
- **Cold locks: Lamport** has 2.2-2.6x lower median and 2-3x lower p99 than the
  tournament up to 5M/s, and serves 6M without loss.

**Summary for T>1:** tournament-2a for hot and warm locks, Lamport (2a or Phase 1) for cold
locks, the local-handoff cap as the latency/throughput knob. The wrong choice costs
1.66x on cold locks (tournament) and up to 4.9x plus unfairness on hot ones (Lamport).
That gap is what a per-lock reactive Lamport/tournament lock would recover on mixed
workloads (Zipfian KV). Gate-based 2b is dominated in every regime measured with T>1.

## KV store (`kv` phase, 16x3, cf, 2a, precise data)

`results/cxl_20261118_2256/kv.csv`: 3 reps, 222 runs, no errors, KV checks clean in every
run, rep spread median 1.3% (max 8.3%). 100k keys x 100 B. Algorithm map `top:H`: the H
stripes with the highest expected load on the tournament, the rest Lamport (`docs/kv.md`).
k ops/s, lock p99 in us, fairness.

**Per-lock map, Zipf 0.99:**

| | A, L=64 | A, L=1024 | B, L=64 | B, L=1024 |
|---|---|---|---|---|
| all Lamport (H=0) | 1349, 852 us, 0.84 | 1485, 819, 0.87 | 1331, 852, 0.84 | 1505, 819, 0.86 |
| H=1 | 2180, 590 | 2601, 492 | 2172, 590 | 2759, 475 |
| H=4 | 3532, 262 | 5624, 55 | 3536, 279 | 5518, 82 |
| **H=16** | **5316**, 41, 0.95 | **6894**, 39, 0.94 | **5605**, 37, 0.96 | **7492**, 32, 0.97 |
| H=64 | (= all) | 6566, 37 | (= all) | 7030, 30 |
| all tournament (H=L) | 4903, 30, 0.94 | 5249, 27, 0.94 | 5067, 27, 0.95 | 5498, 23, 0.95 |
| server chain | 4957, 41 | 5481, 43 | 5242, 35 | 5973, 37 |

**Key skew x node affinity** (A, L=1024):

| | all Lamport | H=16 | all tournament | server chain |
|---|---|---|---|---|
| uniform | **11158** | 10901 | 5804 | 6925 |
| uniform, affinity 0.9 | **11674** | 11324 | 5870 | 6938 |
| Zipf 0.9 | 2428 (p99 557 us) | **8570** | 5587 | 6467 |
| Zipf 0.9, affinity 0.9 | **11588** | 10820 | 5912 | 7023 |
| Zipf 0.99 | 1494 (p99 786 us) | **6964** | 5240 | 5521 |
| Zipf 0.99, affinity 0.9 | **11282** | 10595 | 5994 | 7130 |

**Data maintenance** (all tournament, A, Zipf 0.99): precise 4944 / 5283 vs `--full` 136
(p99 2.4 ms) / 1980 at L=64 / 1024: **36x** / 2.7x.

**Open loop** (A, L=1024, Zipf 0.99; lock p50 / p99 in us):

| offered | 1M | 2M | 4M | 6M | 8M |
|---|---|---|---|---|---|
| H=16 | **2.7 / 12.3** | **2.8 / 18.4** | 5.9 / 74 | 17% unserved | 43% |
| server chain | 4.6 / 20.5 | 4.6 / 20.5 | **7.4 / 47** | **10%** (5.5M served) | 39% |
| all tournament | 5.9 / 17.4 | 6.1 / 26.6 | 18.4 / 156 | 27% | 51% |
| all Lamport | saturated at ~0.53M | | | | |

- **A per-lock choice pays a lot on skewed, shared data.** With Zipf keys and no
  affinity, H=16 beats all-tournament by 8-11% at L=64 and **31-36% at L=1024**, and
  all-Lamport by 4-5x. The hot stripes need the tournament (all-Lamport: p99 0.8 ms,
  fairness 0.84); the cold tail is faster on Lamport.
- **Too few tournament locks is worse than none of the per-lock benefit**: H=1-4 at L=64
  leaves hot stripes on Lamport (2.2-3.5M).
- **Without shared hot locks, Lamport wins** (uniform keys, or affinity 0.9 so hot locks
  are owned by one node and stay there through cohort passing): all-Lamport 11.2-11.7M,
  ~2x all-tournament. H=16 stays within 2-7% of it, so a map that only moves truly
  shared hot locks to the tournament is near-best in every row.
- The server chain is between all-tournament and H=16 in closed loop, but has the highest
  open-loop capacity (5.5M served at 6M offered) and the best tail at 4M.
- The oracle map uses the known key distribution; a reactive lock would have to find the
  hot, shared set online. Its upper bound here is the H=16 row.

## Reactive lock in the KV store (`kvreact` phase, 16x3, cf, 2a)

`results/cxl_20261119_0332/kvreact.csv`: 3 reps, 156 runs, no errors, KV checks clean,
rep spread median 1.1% (max 8.3%). k ops/s (fairness where below 0.85); "tour" = share of
global acquisitions on the tournament.

| keys, L | all Lamport | top-16 (oracle) | all tournament | server chain | reactive 2:64 | reactive 4:512 |
|---|---|---|---|---|---|---|
| Zipf 0.99, 64 | 1292 (0.82) | **5342** | 4880 | 4888 | 4609 (tour 1.00) | 4486 (1.00) |
| Zipf 0.99, 1024 | 1476 (0.83) | **6917** | 5232 | 5584 | 5882 (0.38) | 5940 (0.34) |
| uniform, 64 | 5212 | 5248 | 5394 | **5699** | 5014 (1.00) | 5089 (1.00) |
| uniform, 1024 | **11144** | 10899 | 5774 | 6839 | 8514 (0.14, 55k switches) | 9150 (0.04) |
| Zipf + affinity 0.9, 64 | **9094 (0.48)** | 9012 (0.78) | 6758 | 7864 | 6783 (0.77, 80k sw.) | 6376 (1.00) |
| Zipf + affinity 0.9, 1024 | **11325** | 10552 | 5958 | 7120 | 9435 (0.05) | 9529 (0.05) |

Open loop (Zipf 0.99, L=1024; lock p50 / p99 us): reactive 4:512 3.1 / 12.3 at 1M, 3.2 /
18.4 at 2M, 6.7 / 53 at 4M, 1.1% unserved at 6M; top-16 2.6 / 11.8, 2.8 / 16.4, 5.6 / 37,
and serves 6M (p99 156 us); server chain 4.6 / 21.5, 4.6 / 21.5, 8.2 / 53, 12% unserved at 6M.

- **Reactive is the most robust online choice but leaves 14-30% to the oracle.** Its worst
  case against the best fixed choice is -18% (uniform, 1024) to -30% (affinity, 64), against
  -48% for all-tournament (uniform, 1024) and -79% for all-Lamport (Zipf, 1024). On Zipf
  keys at L=1024 it beats all-tournament by 12-14% and all-Lamport by 4x, but trails the
  oracle by 14%.
- **The extra `mode` load is the main cost on cold locks:** at uniform L=1024 with 4:512
  only 4% of global acquisitions use the tournament, yet reactive is 18% below all-Lamport.
  That is one more CXL round trip on a ~2-round-trip Lamport acquisition.
- **The "any cross-node contention" trigger over-promotes:** at L=64 every lock ends on the
  tournament (tour 1.00), while the oracle keeps the 48 coldest on Lamport (+16%). Warm locks
  meet occasional contention but are still faster on Lamport. 2:64 also thrashes (55-80k
  switches per run); 4:512 mostly stops it without closing the gap.

### Reactive lock, second version (`kvreact`, 2026-11-19 04:59)

`results/cxl_20261119_0459/kvreact.csv`: same setup, after `9ddd81f` (INVALID marker in
Lamport's `y`, so no `mode` read on a Lamport fast-path acquisition; rate trigger). 3 reps,
156 runs, no errors, KV checks clean, rep spread median 1.0% (max 5.0%). k ops/s; in
brackets the change against the best fixed lock choice in the same run (all Lamport, top-16
map, all tournament); the server chain is listed separately.

| keys, L | best fixed lock | server chain | first version (best of 2:64, 4:512) | **12:4** | 8:4 |
|---|---|---|---|---|---|
| Zipf 0.99, 64 | 5291 (top-16) | 4880 | 4609 (-13%) | **4884 (-8%)** | 4661 (-12%) |
| Zipf 0.99, 1024 | 6890 (top-16) | 5478 | 5940 (-14%) | **6456 (-6%)** | 6347 (-8%) |
| uniform, 64 | 5373 (all tournament) | **5660** | 5089 (-5%) | 5045 (-6%) | 5119 (-5%) |
| uniform, 1024 | 11068 (all Lamport) | 6848 | 9150 (-18%) | **10398 (-6%)** | 10283 (-7%) |
| Zipf + affinity, 64 | 9190 (all Lamport, fairness 0.52) | 7820 | 6783 (-25%) | **9367 (+2%, fairness 0.81)** | 8746 (-5%) |
| Zipf + affinity, 1024 | 11279 (all Lamport) | 7029 | 9529 (-16%) | **10758 (-5%)** | 10639 (-6%) |

Open loop (Zipf 0.99, L=1024; lock p50 / p99 us): 12:4 2.6 / 11.8 at 1M, 2.7 / 18.4 at 2M,
5.1 / 49 at 4M, and it serves 6M (p50 51, p99 410); top-16 2.6 / 11.8, 2.8 / 16.4, 5.6 / 37,
19 / 156 at 6M; server chain 12% unserved at 6M. The first version dropped 1% at 6M.

- **The gap to the best fixed choice fell from 5-25% to 2-8%** (12:4); in the affinity case
  at L=64 it now beats every fixed choice, with fairness 0.81 against all-Lamport's 0.52.
  Against the best uniform choice it gains 24% (Zipf L=1024 vs all-tournament) and 4.3x
  (vs all-Lamport), while losing at most 6-8% where one uniform choice is right.
- **Uniform keys no longer promote at L=1024** (tournament share 0.000 vs 0.04-0.14), so the
  INVALID marker shows its effect directly: 10398 vs all-Lamport's 11068 (-6%), against -18%
  for the first version.
- **12:4 beats 8:4 in five of six cases**; 8:4 still thrashes where contention hovers near
  the threshold (141k switches at L=64 with affinity, against 35k for 12:4).
- **Remaining gaps:** at L=64 with uniform keys all locks are warm and the lock promotes
  nearly all of them (0.95), like all-tournament minus 6%; the server chain is best there.
  At Zipf L=64 it is 8% below the top-16 map at a similar tournament share (0.55 vs 0.44).

### Reactive lock: demotion dwell (`kvreact`, 2026-11-19 05:47)

`results/cxl_20261119_0547/kvreact.csv`: after `7e545d0` (`--react U:D:W`, history reset on a
mode change); 3 reps, 216 runs, no errors, spread median 1.1% (max 5.5%). Eager promotion
with a long dwell (6:4:256, 4:4:256, 4:2:1024) against 12:4:64. (A run at 05:23,
`results/cxl_20261119_0523`, used the previous build by mistake and repeated the 04:59 run
within ~10%.)

| | 16-hottest map | 12:4:64 | 6:4:256 | 4:4:256 | 4:2:1024 |
|---|---|---|---|---|---|
| open 6M: p50 / p99 (us) | 20 / 172 | 53 / 442 | 115 / 1901 | 2.9% unserved | 6.3% unserved |
| open 6M: tournament share | 0.27 | 0.19 | 0.31 | 0.45 | 0.52 |
| closed Zipf L=1024 (k/s) | 6874 | **6434** | 6176 | 5863 | 5699 |
| closed uniform L=1024 | 10917 | **10337** | 10132 | 7755 | 6611 (share 0.62) |
| closed Zipf L=64 | 5270 | **4881** | 4482 | 4427 | 4546 |
| closed affinity L=64 | 9008 | **9395** | 8255 | 7567 | 6967 |

- **Promoting more locks made it worse everywhere.** 6:4:256 matches the map's tournament
  share (0.31 vs 0.27) yet is slower, so the gap is not the number of promoted locks. With
  three threads on 16 nodes many warm locks meet another node now and then; the binary
  contended/uncontended signal cannot tell them from hot ones, and they are slower on the
  tournament.
- 12:4:64 remains the best setting; whether the rest of the gap at 6M is the policy (which
  locks) or the mechanism (e.g. the `mode` read on every tournament acquisition) is the
  question of the isolation experiment (`kviso`).

### Reactive lock: mechanism vs policy (`kviso`, 2026-11-19 06:13)

`results/cxl_20261119_0613/kviso.csv`: after `e80ca43`; 3 reps, 72 runs, no errors, spread
median 0.2% (max 3.6%). "Pinned" = the reactive lock started in the fixed map's assignment
with switching off (`--react off`), so it differs from the fixed map only by the reactive
mechanism. k ops/s; lock p50 ns.

| | fixed | reactive pinned | diff | reactive 12:4:64 |
|---|---|---|---|---|
| Zipf 0.99, L=1024, 16-hottest map | 6943 | 6295 | **-9.3%** | 6412 |
| Zipf 0.99, L=64, 16-hottest map | 5464 | 4923 | **-9.9%** | 4877 |
| Zipf 0.99, L=1024, all tournament | 5204 | 4827 | -7.2% | |
| uniform, L=1024, all tournament | 5810 | 5411 (p50 6.4 vs 5.9 us) | -6.9% | |
| uniform, L=1024, all Lamport | 11085 | 10388 (p50 2.69 vs 2.56 us) | **-6.3%** | |

Open loop, Zipf 0.99, L=1024 (lock p50 / p99 us): at 6M fixed map 20 / 172, reactive pinned
**70 / 590**, reactive 12:4:64 45 / 377; at 4M 5.6 / 37, 6.1 / 49, 5.1 / 49; at 1-2M all
within ~10%.

- **The gap is the mechanism, not the policy.** With exactly the map's assignment and no
  switching, the reactive lock is 9-10% below the fixed map in closed loop and worse than
  reactive 12:4:64 at 6M; the online policy is as good as the oracle within the mechanism.
- **Both paths cost ~6-7%.** The tournament path: the `mode` read after each acquisition.
  The Lamport path is unexpected: its fast path has no extra metadata access (dev box:
  -1.5%), yet on CXL it is 6.3% slower (+130 ns p50, about a quarter of a round trip).
  Candidates: the layout (the reactive block is [mode][x][y][tree], 48 lines; the fixed
  per-lock layout puts x, y in lines 0-1 of a 45-line block) and slow-path `mode` reads.
- Near saturation the ~9% capacity loss becomes 3.5x the p50 at 6M.

### Reactive lock after folding the tournament's `mode` read (`kviso`, 2026-11-19 20:28)

`results/cxl_20261119_2028/kviso.csv`: after `c181d9b` (`mode` read in the root's entry
check, one batched round trip with the root's flag and turn); 3 reps, 72 runs, no errors,
spread median 0.0% (max 3.5%). k ops/s; in brackets the change against the fixed lock, and
the 06:13 run (separate `mode` read) for comparison.

| | fixed | reactive pinned | 06:13 pinned | reactive 12:4:64 | 06:13 12:4:64 |
|---|---|---|---|---|---|
| Zipf, L=1024, 16-hottest map | 6876 | 6842 (-0.5%) | -9.3% | **6928 (+0.8%)** | 6412 |
| Zipf, L=64, 16-hottest map | 5380 | 5275 (-2.0%) | -9.9% | **5343 (-0.7%)** | 4877 |
| Zipf, L=1024, all tournament | 5216 | 5138 (-1.5%) | -7.2% | | |
| uniform, L=1024, all tournament | 5807 | 5754 (-0.9%) | -6.9% | | |
| uniform, L=1024, all Lamport | 11055 | 10321 (**-6.6%**) | -6.3% | | |

Open loop, Zipf 0.99, L=1024 (lock p50 / p99 / p99.9 us):

| offered | fixed 16-hottest map | reactive 12:4:64 | 06:13 reactive 12:4:64 |
|---|---|---|---|
| 1M | 2.6 / 11.8 / 17.4 | 2.6 / 11.8 / 41.0 | 2.6 / 11.8 / 28.7 |
| 2M | 2.8 / 16.4 / 24.6 | 2.7 / 17.4 / 47.1 | 2.6 / 18.4 / 43.0 |
| 4M | 5.6 / 38.9 / 59.4 | **4.4 / 36.9** / 69.6 | 5.1 / 49.2 / 81.9 |
| 6M | 18.4 / 156 / 246 | **19.5 / 172** / 279 | 45.1 / 377 / 623 |

- **The open-loop gap is closed:** at 6M the reactive lock is within 6% of the oracle map
  at p50 and 10% at p99 (it was 2.4x / 2.2x), and at 4M it is ahead at p50 and p99.
- **The tournament path now costs 1-2%** against the fixed tournament (was 7%), and with
  Zipf keys the reactive lock matches the oracle map in closed loop (+0.8% at L=1024,
  -0.7% at L=64).
- **The Lamport path still costs 6.6% on CXL** (uniform keys, all Lamport; dev box 1.5%).
  Its fast path has no extra metadata access, so the cause is still open (layout and
  slow-path `mode` reads are the candidates).
- **p99.9 at light load is 2x the map's** (41-47 vs 17-25 us at 1-2M): at low load few
  locks show enough contention to be promoted (5-10% of global acquisitions on the
  tournament vs 29%), so occasional bursts on a hot lock meet Lamport's slow path.

## KV store: coherent baselines (`kvbase`, 16x3, all on the CXL node)

`results/cxl_20261119_2143/kvbase.csv`: after `cf34f59`; 3 reps, 144 runs, no errors, KV
checks clean, spread median 0.5% (max 3.2%). "Coherent" = this host's own cache coherence
on the CXL memory (no writeback or invalidate; `pthread` = futex, coherent atomics): an
upper bound, what a coherent device used by one host would give, not several hosts sharing
a non-coherent one. k ops/s; lock p50.

| | uniform L=1024 | Zipf 0.99 L=1024 | Zipf 0.99 L=64 |
|---|---|---|---|
| coherent pthread, no submutex | **97244** (p50 0.29 us) | 16140 | 14111 |
| coherent pthread + submutex | 94148 | **16557** | **14409** |
| coherent reactive 2a | 66597 (0.54 us) | 15748 | 13434 |
| coherent tournament 2a | 44669 (0.86 us) | **16609** | 13856 |
| non-coherent reactive 12:4:64 (cf) | 10380 (2.69 us) | 6888 | 5237 |
| non-coherent 16-hottest map (cf) | 10862 | 6852 | 5413 |

Open loop, Zipf 0.99, L=1024 (lock p50 / p99 us): coherent pthread 0.35 / 0.9 at 2M,
0.35 / 1.3 at 6M, 0.37 / 22 at 15M, saturates at ~16M; non-coherent reactive 2.7 / 18 at
2M, 18 / 164 at 6M, saturates at ~6.9M (37% unserved at 10M).

- **The non-coherent locks reach 1/9 of coherent locking on uncontended locks and 0.37-0.42
  of it on contended ones** (uniform: 10.4M vs 97M; Zipf: 6.9M vs 16.6M at L=1024, 5.2-5.4M
  vs 14.4M at L=64). Under contention all locks are bound by the hot keys' serialization,
  so the gap narrows.
- **Most of the gap is non-coherence, not the algorithm.** The same reactive lock on
  coherent memory gives 66.6M (uniform) and 15.7M (Zipf): non-coherence costs 6.4x and
  2.3x. The rest is algorithm: the futex mutex beats the reactive lock by 1.46x uncontended
  (one atomic vs Lamport's store-fence-load round trips) and only 5% under Zipf.
  Non-coherence costs twice over: every metadata access goes to the device, and each
  arriving acquisition invalidates its record and fetches it from CXL, where coherent
  caches keep hot records.
- **Latency:** at 2-6M the coherent mutex's median is 8-50x lower (0.35 vs 2.7-18 us), and
  its capacity 2.4x higher (~16M vs ~6.9M).
- The non-coherent reactive lock again matches the oracle map (6888 vs 6852 at Zipf
  L=1024).

## Metadata layout of the reactive lock (`klayout`, 2026-11-20 02:53)

`results/cxl_20261120_0253/klayout.csv`: after `34eb4ed`; 3 reps, 33 runs, no errors.
16x3, cf, 2a, L=1024. k ops/s (median); block = metadata lines per lock (stride = 64 B x
block).

| uniform keys | block (stride) | x, y | k ops/s |
|---|---|---|---|
| fixed per-lock layout | 45 (2880 B) | every other block mid-pair | 11097 |
| fixed, padded by 1 | 46 (2944 B) | one 128 B pair | 11072 |
| plain Lamport | 2 (128 B) | one pair | 11021 |
| **fixed, padded by 3** | **48 (3072 B)** | one pair | **10618 (-4.3%)** |
| reactive pinned to Lamport, [mode][x][y] | 48 (3072 B) | split | 10362 (-6.6%) |
| reactive pinned to Lamport, [x][y][mode] | 48 (3072 B) | one pair | 10567 (-4.8%) |

| Zipf 0.99 | k ops/s | open loop 6M p50 / p99 (us) |
|---|---|---|
| fixed 16-hottest map (45-line blocks) | 6913 | 20.5 / 164 |
| reactive 12:4:64, [mode][x][y] | 6888 | |
| reactive 12:4:64, [x][y][mode] | **6944** | **17.4 / 164** |

- **On CXL it is the stride, not the pair alignment** (the opposite of the dev box): 45-,
  46- and 2-line blocks are within 0.7%, while 48-line blocks (3072 B, a multiple of 1 KB)
  cost 4.3% even with x and y in one pair. The reactive block is 48 lines, so most of its
  Lamport-path cost is this stride; the [x][y][mode] order recovers another 2 points.
  Likely the device's interleaving: with a 3 KB stride every lock's hot lines fall on the
  same offsets modulo 1 KB.
- Slow-path `mode` reads stay negligible (1.6% of global acquisitions).
- With Zipf keys the reordered reactive lock now slightly beats the oracle map, also in open
  loop at 6M (p50 17.4 vs 20.5 us).

## Lock-block stride (`kstride`, 2026-11-20 03:07)

`results/cxl_20261120_0307/kstride.csv`: after `56d822a`; 3 reps, 69 runs, no errors.
Uniform keys, L=1024, 16x3, cf, 2a. k ops/s (median) by meta lines per lock (stride = 64 B
x lines), via `--pad-lines`:

| stride | multiple of 512 B? | fixed layout | reactive (xy-first) | plain Lamport |
|---|---|---|---|---|
| 128 B, 192 B | no | | | 11142, 11136 |
| 512 B | **yes** | | | **10457** |
| 1024 B, 1088 B | yes, no | | | **10636**, 11054 |
| 2048 B | **yes** | | | **10634** |
| 2880-3008 B (45-47 lines) | no | 11131, 11106, 11053 | | |
| **3072 B (48 lines)** | **yes** | **10418** | **10466** | **10412** |
| 3136-3264 B (49-51 lines) | no | 11027, 11104, 11065 | 11066, 10996, 10988 | 11053 (3136) |
| 3584 B (56 lines) | **yes** | **10453** | | |
| 4096 B (64 lines) | **yes** | **10405** | **10428** | **10438** |

- **Every stride that is a multiple of 512 B costs 4-6%; every other stride is within ~1% of
  the best**, for all three lock types and the same code. So with such a stride every lock's
  hot metadata lines fall on the same offsets modulo 512 B, which on this device (or its
  interleaving) costs throughput. Pair alignment does not matter here (odd and even line
  counts are equal), unlike on the dev box.
- **The reactive lock's remaining Lamport-path cost was this:** at 49 lines it gets 11066,
  0.6% below the best fixed layout (11131), against 10466 at its natural 48.
- The scan region (Lamport's `b[]`, announce lines) has P = 16 lines per lock, a 1 KB
  stride, in every configuration measured so far; it may carry the same penalty for all of
  them.

## Flat merged bakery (`bakery`, `kvbakery`, 2026-11-23 19:49)

`results/cxl_20261123_1949/`: `-a bakery` (design §4g g5/g6) against the tournament,
Lamport and reactive 12:4:64; cf + clwb, CXL node 2, 3 reps, medians, 516 + 132 runs, no
errors, spread mostly below 5% (max 11%). k acquisitions/s, fairness = per-node min/max.

**Nearly uncontended (L=256), closed loop:**

| N x T | tournament | Lamport | reactive | bakery | bakery line reads/s |
|---|---|---|---|---|---|
| 4x3 (2a) | 2714 | 2982 | 2962 | **3550** | 32M |
| 8x3 (2a) | 3922 | 5660 | 5586 | **6119** | 86M |
| 16x2 (2a) | 4008 | 7167 | 7021 | **7227** | 217M |
| 16x3 (2a) | 5779 | **9638** | 9467 | 7692 | 231M |
| 32x2 (2a) | 6136 | **10938** | 10825 | 4349 (fair 0.83) | 270M |
| 16x1 (P1) | 2077 | 3894 | | **4247** | 127M |
| 32x1 (P1) | 3365 | **7084** | | 4421 | 274M |

- **Per acquisition, the bakery is the fastest lock measured.** Open loop at 16x3 L=256, its
  lock p50 / p99 is the lowest at every rate (0.5M: 3.6 / 6.9 us; Lamport 4.0 / 7.4,
  tournament 7.7 / 14.8) and it stays unsaturated at 6M, where the tournament saturates.
  At 16x1 its lock p50 equals Lamport's (2.4 us) with a lower p99.
- **In aggregate it hits the device's read bandwidth.** Each acquisition reads the other
  P-1 slots twice (rounds 2 and 4). Its plateau is 230-275M line reads/s (last column),
  the device's measured ceiling (`rdbatch` 263-275M at T=32-60, "Device concurrency"). Below
  that (N <= 8, 16x1) it beats Lamport; at 16x3 it is 20% below, at 32 nodes 60% below and
  less fair. The §4g cost model ("width is nearly free") holds for latency, not for
  bandwidth: reads per acquisition grow as 2(N-1) against O(1) for Lamport's fast path.

**Contended (L=1):**

| N x T | Phase 1: tournament / bakery | 2a: tournament / bakery |
|---|---|---|
| 4x3 | 417 / 210 | 1530 / 1447 |
| 8x3 | 478 / 178 | 1525 / 1448 |
| 16x3 | 486 / 146 | 1441 / 1318 |
| 32x2 | 478 / 108 | 1388 / 1219 |
| 16x1 / 32x1 | 507 / 142, 507 / 104 | |

- **A cross-node handoff costs 3-4.5x the tournament's** (16x3: 6.8 us vs 2.1 us), and the
  gap grows with N. Cohort passing (2a, `-K 64`) hides most of it (-5 to -12%). Fairness
  is 1.00 throughout (Lamport: 0.15-0.87).
- Open loop 16x3 L=1: lowest p50 at 100k (3.7 us; tournament 7.9), 4.5x the tournament's
  from 500k on (70 vs 15 us); no rate up to 1.25M saturates it.
- Middle contention (16x3, 2a): L=4 bakery 1399 vs tournament 2397; L=16 3911 vs 4227;
  L=64 **7145** vs 5332 (Lamport 5134).
- Likely cause (not measured on CXL; the CSV has no bakery counters): the strict wait rule
  (`wait while slot[j].choosing ...`) makes the successor wait for the releasing node's next
  doorway. In Phase 1 the node's next thread (or, at T=1, the releaser itself) starts its
  doorway right after the release, so the successor's re-read often finds it choosing and
  waits two more rounds. On the dev box 26-37% of contended waits saw a choosing blocker.
  Classic bakery does not wait there: once j was seen with choosing = 0 after our ticket
  was published, only j's ticket matters, and {choosing, 0} has ticket 0.

**Predecessor polling (g6)** vs re-reading every blocker on each poll: Phase 1 L=1
1.4-1.8x faster (16x3 146k vs 103k; 32x2 109k vs 59k); 2a L=1 +6-15%; 2a L=16 mixed (16x3:
3872k vs 4428k, poll-all 14% faster; 32x2 equal).

**KV store (16x3, 2a):**

| keys, L | Lamport map | top:16 | tournament map | reactive | bakery | server chain |
|---|---|---|---|---|---|---|
| uniform, 1024 | **11.14M** | 10.87M | 5.78M | 10.97M | 7.72M | 7.00M |
| Zipf 0.99, 1024 | 1.50M | **6.91M** | 5.22M | 6.88M | 3.53M | 5.57M |
| Zipf 0.99, 64 | 1.31M | 5.31M | 4.99M | **5.32M** | 3.10M | 5.07M |
| Zipf 0.99 + F 0.9, 1024 | **11.32M** | 10.48M | 5.96M | 11.29M | 8.07M | 7.15M |

- Uniform: the read-bandwidth plateau again (-31% vs Lamport). Zipf: the hot stripes are
  contended, so the handoff cost dominates (-49% vs reactive). Fairness 0.96-0.98 (others
  0.84-0.95).
- Open loop, Zipf L=1024: the bakery has the lowest op p50 / p99 at 1M (3.7 / 8.7 us vs
  reactive 4.4 / 13.8) and saturates at 3.5M; the map and reactive reach 6M.

## k-ary bakery tree (`bktree`, `kvbktree`, 2026-11-23 21:07)

`results/cxl_20261123_2107/`: `-a bakery --bakery-arity K` (design §4g g5b; flat = K 0)
against the tournament and Lamport; cf + clwb, CXL node 2, 3 reps, medians, 684 + 63 runs,
no failed runs. Median spread is 0.9%; the largest is 16% (flat 16x2 L=256 P1) and the next 10%.
Arities >= N are not run. `preflight` did not run (typo `preflisht` in `errors.log`).
k acquisitions/s.

**Contended (L=1, Phase 1):**

| N x T | tournament | flat | K2 | K4 | K8 | Lamport |
|---|---|---|---|---|---|---|
| 4x3 | **419** | 213 | 294 | | | 125 |
| 8x3 | **493** | 176 | 387 | 271 | | 69 |
| 16x2 | **486** | 142 | 387 | 313 | 258 | 36 |
| 16x3 | **483** | 146 | 383 | 299 | 255 | 37 |
| 32x2 | **473** | 109 | 379 | 336 | 325 | 18 |
| 16x1 | **505** | 143 | 390 | 350 | 248 | 36 |
| 32x1 | **501** | 104 | 390 | 345 | 366 | 18 |

- **K2 recovers most of the gap and stays flat in N**: 78-81% of the tournament from 8
  to 32 nodes (flat: 21-36%). A cross-node handoff costs 2.6 us against the tournament's
  2.1 us; the flat bakery's 6.8 us (16x3) was its width, not the doorway alone. Fairness
  0.98-1.00.
- **2a, L=1:** the tree matches the tournament (16x3: K2-K8 1408-1431 vs 1441; 32x2
  1378-1382 vs 1392; flat 1312 / 1229).
- Open loop 16x3 L=1: the flat bakery has the lowest p50 at 100k (2.6 us; tournament 5.6,
  K2 7.7). From 500k the tournament leads (p50 12.8 us; K2 23.6, K4 34.8, flat 69.6).
  Nothing saturates up to 1.25M except Lamport.

**Nearly uncontended (L=256), closed loop:**

| N x T | tournament | Lamport | flat | K2 | K4 | K8 |
|---|---|---|---|---|---|---|
| 4x3 (2a) | 2696 | 2959 | **3465** | 2190 | | |
| 8x3 (2a) | 3882 | 5637 | **5997** | 3080 | 4151 | |
| 16x2 (2a) | 4010 | **7140** | 7131 | 3130 | 5286 | 5171 |
| 16x3 (2a) | 5767 | **9610** | 7716 | 4462 | 7310 | 7060 |
| 32x2 (2a) | 6039 | **11036** | 4516 (fair 0.84) | 4771 | 6790 | 7532 |
| 16x1 (P1) | 2069 | 3890 | **4195** | 1655 | 2841 | 2814 |
| 32x1 (P1) | 3359 | **7089** | 4614 | 2600 | 3940 | 5136 |

- **Each level costs about 1.9 us uncontended.** Open loop 16x3 L=256 at 0.5M, lock p50:
  flat 2.4 us (= Lamport), K4 / K8 (2 levels) 4.1, K2 (4 levels) 7.7, tournament 5.6.
  Below the bandwidth ceiling (N <= 16), fewer levels win, and the flat bakery is fastest.
- **At 32 nodes the tree lifts the ceiling:** K8 reads 20 lines per acquisition instead of
  62. 32x2 K8 7.5M = 150M line reads/s, below the device's 230-275M; flat 4.5M = 280M.
  K8 beats the tournament by 25% but is 32% below Lamport, which reads O(1) lines.
- Open loop 16x3 L=256: K2 saturates at 5M (4.46M served) and the tournament at 6M
  (5.73M). Flat, K4, K8 and Lamport serve 6M; flat and Lamport have the lowest p99 (21-27
  us at 6M; K4 70, K8 86).
- Middle contention (16x3, 2a): L=4 tournament **2342**, K2 1994, K4 1756, flat 1390; L=16
  tournament **4264**, K4 4017, flat 3840; L=64 flat **7343**, K4 6327, tournament 5351,
  Lamport 5177.

**KV store (16x3, 2a, L=1024), M ops/s:**

| keys | Lamport map | top:16 | reactive | flat | K2 | K4 | K8 |
|---|---|---|---|---|---|---|---|
| uniform | **11.11** | 10.89 | 10.95 | 7.73 | 4.54 | 7.60 | 7.29 |
| Zipf 0.99 | 1.49 | **6.89** | **6.89** | 3.52 | 4.09 | 5.29 | 4.41 |
| Zipf 0.99 + F 0.9 | **11.25** | 10.50 | 11.19 | 8.07 | 4.65 | 7.75 | 7.58 |

- K4 is the best bakery under Zipf (+50% over flat) but 23% below top:16 and reactive.
  Uniform: the flat bakery and K4 tie, 30% below the Lamport map. All runs: 0 errors.

**Summary.** No single arity is best: K2 is best under contention and flat (N <= 16) or K8
(32 nodes) when uncontended, and each is beaten by the tournament or Lamport in the same
regime. The bakery wins only at middle contention with many locks (L=64, flat +37% over
the tournament). In the KV store, it never wins. Per level, a 2-way bakery node handoff
(2.6 us) costs more than a Peterson node (2.1 us). Whether the strict wait rule causes this
(previous section) is still unmeasured. An adaptive root (§4g g7) can at best pick the better
of the two root algorithms per regime; with these numbers it would not beat
reactive Lamport/tournament.

## Bakery wait rule (`bkwait`, `kvbkwait`, 2026-11-23 22:52)

`results/cxl_20261123_2252/`: `--bakery-wait strict` (default, the spec's pseudocode) vs
`classic` (design §4g g3) at flat, K=2 and K=4, with the tournament as reference; cf + clwb,
CXL node 2, 3 reps, medians, 438 + 54 runs, no errors. Spread is mostly below 5% (max 10%).
k acquisitions/s, classic / strict.

**Contended (L=1, Phase 1):**

| N x T | tournament | flat | K2 | K4 |
|---|---|---|---|---|
| 4x3 | **420** | 291 / 201 | 375 / 297 | |
| 8x3 | **496** | 246 / 179 | 384 / 373 | 334 / 283 |
| 16x2 | **490** | 228 / 142 | 386 / 387 | 332 / 311 |
| 16x3 | **485** | 220 / 147 | 380 / 382 | 330 / 299 |
| 32x2 | **473** | 205 / 110 | 374 / 374 | 362 / 335 |
| 16x1 | **507** | 232 / 142 | 388 / 394 | 341 / 366 |
| 32x1 | **510** | 216 / 105 | 377 / 380 | 370 / 334 |

- **The flat bakery gains 1.4-2.1x**, more at larger N: the strict rule was most of its
  contended cost. K4 gains 0-11% (16x1: -7%, within its 8% spread). K2 is unchanged at 8 nodes and above.
  At 4x3, where the tree has a single 2-way root, it gains 26%.
- **The tournament still leads by 22-27%** over the best bakery (K2 at 8+ nodes). The
  strict rule does not explain the K2 tree's slower handoff.
- Likely reason (hypothesis; the CSV has no bakery counters): the choosing case shows up
  in the batched re-read of the *other* blockers after the predecessor changes. A
  2-way node has no other blockers, so the predecessor poll sees its slot change to 0
  before the re-entry.

**L=16:**

| N x T | 2a: tournament | flat | K4 | P1: tournament | flat | K4 |
|---|---|---|---|---|---|---|
| 4x3 | 2502 | **2719** / 2646 | | 2285 | **2422** / 2286 | |
| 8x3 | 3277 | **3689** / 3436 | 3112 / 3051 | 2972 | **3232** / 2835 | 2757 / 2681 |
| 16x2 | 3276 | **3668** / 3431 | 3402 / 3352 | 3074 | **3476** / 2849 | 3253 / 3152 |
| 16x3 | **4241** | 4154 / 3885 | 4149 / 4043 | **3808** | 3316 / 2212 | 3652 / 3570 |
| 32x2 | **4195** | 3621 / 3276 | 3952 / 3879 | **3923** | 3244 / 1670 | 3664 / 3667 |

- With classic, the flat bakery beats the tournament at L=16 up to 16x2 (+6-13%). At 16x3
  and 32x2 the tournament stays ahead. Phase 1 flat 32x2 nearly doubles (1670 → 3244),
  and its p99 drops from 623 to 119 us.
- 2a L=1: flat +4-10%, the trees ±2%; all within 3% of the tournament.

**KV store (16x3, 2a, L=1024):** classic vs strict within ±2.5% everywhere (Zipf: flat
3.60 / 3.53M, K4 5.46 / 5.33M; uniform flat 7.52 / 7.68M). Cohort passing and the many locks
hide the doorway.

**Summary.** Classic is never worse beyond noise and fixes most of the flat bakery's
contended handoff. It does not change the overall picture: under contention the
tournament still wins at every N, by about 22% over K2.

## Prefix-retention go/no-go (`g1bound`, 2026-11-24 21:00)

`results/cxl_20261124_2100/`: design §4g g1's ceiling. Only a subset of the N nodes
acquires (`ncl_bench --active`), L=1, closed loop with think time 0 / 2 / 10 us; the bound
is the tournament over just the levels g1 would still climb (`-n 2` with node 0 alone for
one node, `-n 2` for {0, N/2}, `-n 4` for {0, N/4, N/2, 3N/4}). cf + clwb, CXL node 2,
2 reps, medians, 648 runs, no failed runs. Median spread 3.8%; 41 of 324 cells spread more
than 15%, mostly the leaf-meeting subsets (near2, near4) and reactive there (it flips
modes between runs). The bound runs use other CPUs than the subset runs (nodes 0..n-1
are adjacent), which is not controlled. The bound ignores g1's own costs (release check,
sweeper).

**Bound / best existing lock** (the best of tournament, Lamport and reactive at the same
subset), by think time 0 / 2 / 10 us:

| N x T, mode | one {0} | far2 {0, N/2} | far4 {0, N/4, N/2, 3N/4} |
|---|---|---|---|
| 16x1 P1 | 1.55 / 1.28 / 1.10 | 2.08 / 1.15 / 1.05 | 1.05 / 1.33 / 1.20 |
| 16x1 2a | 1.41 / 1.21 / 1.08 | 1.62 / 1.15 / 1.05 | 1.08 / 1.21 / 1.24 |
| 16x3 P1 | 1.43 / 1.32 / 1.09 | 1.49 / 1.48 / 1.83 | 0.93 / 0.96 / 0.91 |
| 16x3 2a | 1.04 / 1.33 / 1.08 | 1.09 / 1.52 / 1.08 | 1.00 / 1.08 / 1.17 |
| 32x2 P1 | 1.54 / 1.46 / 1.07 | 1.96 / 2.00 / 1.53 | 1.30 / 1.20 / 1.56 |
| 32x2 2a | 1.11 / 1.19 / 1.08 | 1.21 / 2.06 / 1.05 | 1.16 / 1.37 / 1.38 |

Against the plain tournament alone the bound is 1.2-3.7x for one node, 1.15-2.1x for
far2 and 0.9-1.7x for far4.

- **Go.** The bound beats every existing lock by >= 10% in 36 of 54 cells, by 8-106% (median 21%) in
  2a at think 2 us (the deployment case, moderate load). It does not help where cohort
  passing already hides the global path (2a, think 0: 0-21%), at light load for a single
  node (think 10 us: 7-10%), or for 4 contenders at 16x3 Phase 1 (0.91-0.96; the
  tournament with 2 unused lower levels is slightly faster than `-n 4`, possibly the CPU
  placement).
- **Per level.** Lock p50 for one node at 16x1 P1: tournament (4 levels) 5.2 us, bound
  (root only) 1.3 us, Lamport 2.2 us: ~1.3 us per uncontended Peterson level, and one
  level is cheaper than Lamport's fast path. g1's release check (one load round) comes out
  of that 0.9 us margin; the acquire check alone is free on cf.
- **Leaf meetings cost the tournament dearly.** {0,1} vs {0,8} at 16x1 P1 think 0: 155k vs
  249k; 16x3 2a think 2 us: 371k vs 578k. The waiter climbs the upper levels after every
  handoff, on the critical path. g1 keeps nothing there (g2's case).
- **Reactive mispicks near2.** It often runs the tournament where Lamport is faster (16x1
  P1 think 2 us: 239k vs 339k; 32x2 P1 think 10 us: 89k vs 239k), bimodal between runs.

## Prefix retention (`g1`, 2026-11-24 22:32)

`results/cxl_20261124_2232/`: `-a tournament --retain` (design §4g g1) with the default
adaptive policy (`retain`, penalty 16) and keeping at every release (`retain_always`,
penalty 0), against the plain tournament, reactive and the CPU-matched bounds (now on the
subset's own CPUs) at the g1bound subsets, L=1, think 0 / 2 / 10 us; plus every node at
L = 1, 16, 256. cf + clwb, CXL node 2, 2 reps (means), 792 runs, no failures. Median
spread 2.3%; 40 of 396 cells spread more than 15%.

**Retain (adaptive) at the subsets**, 18 cells each (3 N x T, Phase 1 and 2a, 3 think times):

| subset | / tournament | / reactive | / best of both | / bound |
|---|---|---|---|---|
| one {0} | 1.08-2.60, median 1.82 | 0.89-1.30, median 1.04 | 0.89-1.30, median 1.04 | 0.70-1.00, median 0.89 |
| far2 {0, N/2} | 1.16-2.62, median 1.74 | 0.99-2.70, median 1.48 | 0.99-2.62, median 1.48 | 0.85-1.30, median 0.95 |
| far4 {0, N/4, N/2, 3N/4} | 0.96-1.37, median 1.02 | 0.92-1.31, median 1.12 | 0.92-1.19, median 1.01 | 0.76-1.11, median 0.89 |
| near2 {0, 1} | 0.88-1.28, median 0.96 | 0.58-1.15, median 0.97 | 0.58-1.04, median 0.92 | (leaf meeting, nothing to keep) |

Selected cells, k acquisitions/s (tournament / reactive / retain / retain_always / bound):

| cell | one | far2 | far4 |
|---|---|---|---|
| 16x1 P1 think 0 | 126 / 248 / 324 / 313 / 396 | 249 / 231 / 512 / 508 / 451 | 490 / 422 / 492 / 476 / 513 |
| 16x1 P1 think 2 us | 107 / 177 / 195 / 197 / 216 | 201 / 266 / 393 / 381 / 426 | 401 / 300 / 393 / 457 / 506 |
| 16x3 2a think 2 us | 329 / 554 / 597 / 596 / 694 | 583 / 553 / 796 / 797 / 877 | 817 / 821 / 829 / 851 / 857 |
| 32x2 P1 think 2 us | 106 / 256 / 266 / 256 / 365 | 169 / 165 / 444 / 446 / 371 | 288 / 330 / 394 / 419 / 419 |
| 32x2 2a think 2 us | 192 / 409 / 424 / 371 / 484 | 322 / 274 / 688 / 684 / 709 | 495 / 529 / 624 / 623 / 695 |

**Every node** (think 0), retain / retain_always relative to the plain tournament:

| N x T, mode | L=1 | L=16 | L=256 |
|---|---|---|---|
| 16x1 P1 | -10% / -7% | -2% / -83% | -4% / -75% |
| 16x3 P1 | -4% / -3% | -1% / -71% | -1% / -79% |
| 32x2 P1 | -6% / -8% | -1% / -71% | -1% / -78% |
| 16x1 2a | -8% / -10% | -2% / -84% | -4% / -76% |
| 16x3 2a | -1% / -1% | -3% / -73% | -0% / -79% |
| 32x2 2a | -0% / +1% | -0% / -72% | -1% / -78% |

- **One and two far-apart contenders: retention delivers.** far2 is 1.16-2.62x the
  tournament and 1.48x reactive (median), and is the best lock in every far2 cell but one (16x3 2a think 10 us, 1% under reactive); one
  node is 1.82x the tournament and matches or beats reactive (median 1.04; 0.89 at 32x2 2a
  think 10 us). Fairness is unchanged or better (32x2 P1 far2 think 2 us: min/max 0.81 vs
  0.66). Counters: one hit per acquisition, depth-1 levels skipped, almost no drops.
- **Gap to the bound for one node (median 0.89, 0.70 at 32x2 P1 think 0).** 16x1 P1
  think 0: 3.1 vs 2.5 us per acquisition, ~0.6 us, one load round: the release check,
  which reads depth-1 flags and finds nothing. far2 meets or exceeds its bound (up to
  1.30x at 32x2; the `-n 2` bound has another lock layout, so read it as approximate).
- **far4: the policy is all-or-nothing, and that loses the gain.** The four nodes meet at
  level depth-1, so the right prefix is depth-2 levels. When the release check finds no
  waiter, the node keeps depth-1 levels; a node then arrives at level depth-1 and is
  freed by an acquire check or the sweeper, which charges the penalty, and the node stops
  keeping anything (16x1 P1 think 2 us: 99% policy skips, 393k = tournament, while
  retain_always reaches 457k; 32x2 P1 think 0: 56% skips). At think 10 us retain_always
  loses (-11 to -21%: waiters wait for the sweeper), so neither fixed setting is right.
- **near2 costs 4-12%** (median 0.96 of the tournament): nothing can be kept, but every
  release pays the release check, which finds the sibling at level 1 and is not charged.
- **Every node: the policy works where it must.** Keeping at every release collapses
  (-71 to -84% at L=16 and 256, as on the dev box); the adaptive policy skips 90-99% of
  keeps there and costs 0-4%. At L=1 it costs up to 10% (16x1 P1): again the release
  check, which finds a waiter at level 1 each time.
- **Next: learn the keep height instead of on/off.** Per lock, node-local: the lowest
  level at which a waiter forced a drop (acquire check, sweeper) or the release check found
  one caps the next keep below it, and the release check is skipped while the cap is 0
  (re-probe occasionally). Expected: far4 at retain_always's level without its idle-node
  losses, near2 and all-nodes L=1 at the tournament's cost. Separately, `retain_acq`
  (no release check) measures how much of the one-node gap the release check is.

## Prefix retention, learned keep height (`g1`, 2026-11-25 18:47)

> **Caveat for every `--active` run so far (g1bound, g1, both g1 sections):** inactive
> nodes spun in `ncl_node_detach`'s barrier for the whole run (uncached loads of a done
> line, no backoff; fixed 2026-11-25: spin, then 100 us sleeps). At N=16, `one` ran with
> 15 such pollers, the bounds (`-n 2`, `-n 4`) with 0-3, so the subsets competed with
> device reads and unpinned spinning processes that the bounds did not. Comparisons at one
> subset share the effect; subset-vs-bound ratios may be biased against the subsets. On
> the dev box (DRAM, 20 CPUs) `-n 8 --active 0` is unchanged (0.77M), while `-n 16
> --active 0-7` in 2a went from 0.43M to 2.33M (oversubscribed). Re-run g1 to check.

`results/cxl_20261125_1847/`: same grid as the previous section, with the learned keep
height (`--retain-probe 16`) replacing the on/off policy. Variants: `retain_both`
(the default then: acquire and release checks), `retain_acq` (acquire check only),
`retain_both_always` (keep depth-1 levels at every release); plus the plain tournament,
reactive and the CPU-matched bounds. 972 runs, 2 reps, no failures; median rep spread
1.5-3.8% (reactive the highest). Tables: `scripts/cxl_summary.py results/cxl_20261125_1847 g1`.

**Ratios over the 18 cells per subset (min / median / max):**

| subset | retain_both / tour | retain_acq / tour | retain_both / reactive | retain_acq / reactive | retain_acq / bound |
|---|---|---|---|---|---|
| one {0} | 1.09 / 1.83 / 2.43 | 1.08 / 2.03 / 3.19 | 0.99 / 1.03 / 1.21 | 1.01 / 1.19 / 1.40 | 0.83 / 0.97 / 1.02 |
| far2 {0, N/2} | 0.97 / 1.69 / 2.68 | 1.20 / 1.81 / 2.33 | 1.02 / 1.64 / 2.59 | 1.07 / 1.79 / 2.37 | 0.83 / 0.99 / 1.23 |
| far4 | 0.98 / 1.15 / 1.53 | 1.00 / 1.20 / 1.49 | 0.99 / 1.15 / 1.49 | 1.02 / 1.20 / 1.63 | 0.87 / 0.98 / 1.16 |
| near2 {0, 1} | 0.95 / 1.00 / 1.16 | 0.94 / 1.01 / 1.15 | 0.44 / 0.98 / 1.04 | 0.43 / 0.98 / 1.04 | (nothing to keep) |

The previous run's on/off policy: far4 1.02, near2 0.96 of the tournament (median).

Selected cells, k acq/s (tournament / reactive / retain_both / retain_acq / retain_both_always):

| cell | one | far2 | far4 |
|---|---|---|---|
| 16x1 P1 think 0 | 131 / 256 / 309 / 358 / 313 | 249 / 222 / 519 / 526 / 480 | 474 / 461 / 489 / 494 / 482 |
| 16x1 P1 think 2 us | 108 / 174 / 194 / 218 / 196 | 208 / 345 / 382 / 421 / 382 | 407 / 352 / 456 / 492 / 446 |
| 16x3 2a think 2 us | 329 / 543 / 620 / 708 / 615 | 592 / 546 / 866 / 908 / 854 | 837 / 821 / 866 / 837 / 849 |
| 32x2 P1 think 2 us | 106 / 252 / 258 / 339 / 265 | 213 / 184 / 439 / 427 / 483 | 293 / 315 / 448 / 436 / 449 |
| 32x2 2a think 2 us | 195 / 399 / 447 / 472 / 448 | 324 / 261 / 556 / 599 / 578 | 471 / 537 / 595 / 643 / 703 |

**Every node** (think 0), relative to the plain tournament: retain_both 0.96-1.02,
retain_acq 0.96-1.02, keep-always 0.17-0.29 at L = 16, 256 (0.95-1.00 at L=1). Reactive
was not run in this part.

- **The learned height fixes the on/off policy's losses.** far4 settles at depth-2 kept
  levels (2 of 4 skipped at N=16, 3 of 5 at N=32, a hit on every Phase 1 acquisition) and
  probes the meeting level about once per 4096 releases (cap lowerings 0.0003/acq): 1.15x
  the tournament (was 1.02). near2 and every node at L=1 settle at cap 0 and skip the
  release check: 1.00 (was 0.96, and up to -10% at L=1).
- **The release check costs more than it saves.** Without it (`retain_acq`) one node is
  2.03x the tournament (vs 1.83) and 0.97 of the bound (vs 0.90); far2 and far4 reach
  0.99 and 0.98 of their bounds. It loses nowhere measurable (every-node cells within
  noise of retain_both). **Default changed to `--retain-check acquire`** (2026-11-25).
  Caveat: with the acquire check alone a waiter on an idle node's kept levels depends on
  the sweeper; on an oversubscribed host with keep-always (`--retain-probe 0`) that is
  slow (dev box stress under ctest -j8: timeouts), the learned height avoids it.
- **Against reactive** (retain_acq): faster in every cell of one, far2 and far4 (54 of
  54, 16 / 18 / 16 of 18 by more than 5%), median 1.19x for one node (reactive runs
  Lamport's fast path; retention's single root level is cheaper), 1.79x for far2 (reactive
  climbs the full tree or retries Lamport's slow path), 1.20x for far4. Fairness is
  better too (min/max at far2: worst cell 0.95 vs reactive 0.71; far4 0.88 vs 0.74).
  near2 (leaf meeting): median 0.98, but reactive wins clearly where it picks Lamport for
  two light contenders: 16x1 think 2 us 335k vs 145k (P1), 246k vs 144k (2a); think 10 us
  144k vs 113k. Retention keeps nothing there; that is g2's case, or a reason to combine
  the two (reactive's Lamport mode for leaf-meeting pairs, retention in tournament mode).

## Hot-slot subtree go/no-go (`g2bound`, 2026-11-26 06:49)

`results/cxl_20261126_0649/`: design §4g g2's bounds, tournaments over just the levels a
g2 path climbs with the other side idle, on the CPUs of the subset they model (`hot1_k2`
`-n 4 --active 0`, `hot2_k2` `-n 4 --active 0,1`, `hot2_k4` `-n 8 --active 0,2`, `hot4_k4`
`-n 8 --active 0-3`), and the cold path's extra level (`cold1` `-n 2N --active 0` vs the
tournament's `one`; `coldall` `-n 2N --active 0..N-1` vs every node on the tournament).
References at the g1 subsets: tournament, reactive, g1 (`--retain`, acquire check). cf +
clwb, 2 reps, 792 runs, no failures; idle nodes now back off in the detach barrier. Tables:
`scripts/cxl_summary.py results/cxl_20261126_0649 g2bound`.

**Bound / best existing lock (tournament, reactive, g1), min / median / max over 18 cells:**

| case | ratio |
|---|---|
| near2 {0,1}: hot2_k2 | 0.94 / 1.71 / 2.33 (>= 1.2 in 12 of 18; 5 of 6 at think 2 us) |
| near2: hot2_k4 (placed) | 0.84 / 1.67 / 2.44 |
| near4 {0-3}: hot4_k4 | 1.03 / 1.39 / 1.85 |
| one {0}: hot1_k2 | 0.63 / 0.80 / 1.00 |
| far2: hot2_k4 | 0.46 / 0.63 / 1.00 |
| far4: hot4_k4 | 0.54 / 0.58 / 1.01 |
| **cold path, one node: cold1 / tournament** | 0.82 / 0.88 / 1.02 |
| **cold path, every node: coldall / tournament** | **0.53 / 0.59 / 0.98** |

Selected cells, k acq/s (near2 tournament / reactive / g1 / hot2_k2; all tournament /
coldall): 16x1 P1 think 0: 117 / 149 / 143 / 257; 474 / 264. 32x2 P1 think 2 us: 110 /
108 / 111 / 228; 445 / 264. 16x3 2a think 2 us: 347 / 350 / 367 / 591; 778 / 543.

- **The hot path delivers where g1 cannot.** Leaf-meeting pairs get 1.71x the best existing
  lock (median), four adjacent nodes 1.39x. The criterion (>= 1.2x in most near2 cells at
  think 2 us) is met. Not at 16x1 think 10 us (0.94; light load, reactive's Lamport).
  Placement (k=4, meeting at H's top) is no better than k=2 here (1.67 vs 1.71): one more
  level offsets the shorter post-handoff climb.
- **Where g1 works, g1 is better.** One node: hot1_k2 is 0.80 of the best (g1 climbs the
  root only, g2 an H level plus R); far2 0.63, far4 0.58. So g2 must not admit nodes that
  g1 already serves.
- **The structural cold level fails the criterion (<= ~10%).** Uncontended it costs 12%
  (one level of ~5); when every node contends, 41% (median; 2a with cohort passing at
  think 0 hides it: 0.97-0.98). Under contention the extra level is on the handoff's
  critical path: after the cold tree's top is handed over, the waiter still climbs R
  before entering (16x1 P1: 2.1 us per acquisition on the tournament, 3.8 us with the extra
  level). **g2 as specified: no-go.** It would slow every contended lock by ~40% to speed up
  leaf-meeting pairs.
- **What would make it go** (design §4g g2, revision): bypass R while no slot is held, so
  the cold level is paid only while hot nodes exist; and admit only nodes whose g1 cap shows
  contenders meeting low (g1 cannot help them). Then the remaining cost is the mixed case,
  hot nodes plus contending cold nodes, which the fairness trigger already returns to the
  plain shape.

**g1 rerun (same directory, `g1.csv`) after the barrier fix:** within noise of the 18:47 run
(median change per subset and variant -2.4% to +4.3%), so the spinning idle nodes did not
bias those results. Acquire check, learned height: one 1.98x the tournament (1.15x the best
existing lock, 0.98 of the bound), far2 1.77x (1.46x, 1.00), far4 1.14x (1.14x, 1.00), near2
1.00x; every node 0.93-1.00x. Keeping at every release with the acquire check alone is now
clearly wrong at far4 (0.63x the tournament: waiters at the meeting level wait for the
sweeper), which the learned height avoids.

## Cost of g2's root bypass (`g2bypass`, 2026-11-26 07:58)

`results/cxl_20261126_0758/`: `-a tournament --hot-probe` (one more line per lock,
`hot_mode`, never written, read in the root's entry-check batch after the root's flag and
turn: what a cold node pays in g2 while no hot slot is held, design §4g g2) vs the plain
tournament, back to back; every node at L = 1, 16, 256 and the subsets one {0} and far2
{0, N/2}; Phase 1 and 2a, think 0 and 2 us, 16x1 16x3 32x2. cf + clwb, 3 reps, 360 runs, no
failures. `scripts/cxl_summary.py results/cxl_20261126_0758 g2bypass`.

**probe / plain, min / median / max over 12 cells per case:** every node L=1 0.920 / 0.983 /
1.004; L=16 0.969 / 0.991 / 1.029; L=256 0.984 / 0.993 / 1.005; far2 0.948 / 1.003 / 1.060;
one 0.949 / 1.000 / 1.024. All 60 cells: median 0.993.

- **Small almost everywhere.** Median 0.7%; spread locks (L=16, 256) and the subsets are
  within rep spread (subset cells spread up to 14%, so their 5% swings both ways are noise).
- **One systematic loss: a single lock contended by every node, Phase 1, growing with N.**
  16x1 1.5%, 16x3 2.5%, 32x2 6-8% (think 0: 457k -> 418-434k, think 2 us: 471k ->
  436-447k, lower in all 3 reps; rep spread 3.8% / 2.6%). In 2a the same cells are within
  noise (0.98-1.00): cohort passing takes the root handoff off most acquisitions. The waiter
  at the root polls three lines instead of two, and the poll that sees the release is on
  the handoff's critical path; an ordered 3-line batch completes with the slowest load.
  Any fresh `hot_mode` read must be ordered after the loads that grant entry, so the
  alternative is a separate round (worse); folding it into the flag word does not work
  (a flag can be older than the latest `hot_mode` change from our own side).
- **Verdict: go for g2 with the bypass** (criterion: median <= 2%, no cell more than 5%
  beyond rep spread: the worst, 8.0% at spread 3.8%, is 4.2% beyond). The cost to locks
  without hot nodes is ~1%, except up to 8% on one lock hammered by 32 nodes in Phase 1,
  against 41% for an always-present R.

## Hot-slot subtree, first implementation (`g2`, 2026-11-26 08:47)

`results/cxl_20261126_0847/`: `-a tournament --retain --hot K` (design §4g g2: g1 in the
cold tree, K hot slots; `g2_k2`, `g2_k4`) vs g1 (`--retain`), the tournament and reactive
at the g1 subsets (Phase 1 and 2a, L=1, think 0 / 2 / 10 us), and every node at L = 1, 16,
256 (think 0). Defaults: window 1 ms, lease 1 ms, cold-share eviction at 8 of the last 16
hot acquisitions. cf + clwb, 2 reps, 1044 runs, no failures. Tables:
`scripts/cxl_summary.py results/cxl_20261126_0847 g2`.

**Ratio to g1, min / median / max over 18 cells:**

| subset | g2_k2 / g1 | g2_k4 / g1 | g2_k2 / best existing | g2_k4 / best existing |
|---|---|---|---|---|
| near2 {0,1} | 1.05 / **1.66** / 2.46 | 1.06 / **1.74** / 2.43 | 0.94 / 1.64 / 2.49 | 0.84 / 1.70 / 2.41 |
| near4 {0-3} | 0.99 / 1.04 / 1.75 | 1.05 / **1.39** / 1.88 | 0.96 / 1.04 / 1.71 | 1.04 / 1.40 / 1.84 |
| one {0} | 0.95 / 0.99 / 1.02 | 0.96 / 1.00 / 1.06 | | |
| far2 | 0.97 / 1.00 / 1.13 | 0.95 / 0.99 / 1.09 | | |
| far4 | 0.96 / 0.99 / 1.04 | 0.96 / 0.99 / 1.02 | | |

Every node, ratio to g1: L=1 0.91-1.02, L=16 0.91-0.98, L=256 **0.84-0.89**.

- **The target case works.** Leaf-meeting pairs 1.66x g1 (1.64x the best existing lock),
  fairness 1.00; four adjacent nodes need k=4 (1.39x; k=2 holds only two of them). Where g1
  already works (one, far2, far4) g2 is never admitted and costs nothing measurable.
- **Every node on one lock: unfair.** About half the acquisitions take the hot path and the
  hot nodes take most of the lock: min/max fairness 0.12-0.25 in most cells (g1 ~1.0).
  near4 with k=2 likewise (0.40-0.72). The cold-share trigger reads R's `turn`, the side
  of R's last *announcer*; under contention that is the hot node itself (the arriving H
  winner announces after the waiting cold node and is the victim), so a cold node waiting
  at R is never seen.
- **Many locks, little reuse (L=256): -11 to -16%.** 3-15% of releases claim a slot and
  63-87% of acquisitions climb R. A node acquires a given lock about once per ms, so the
  1 ms lease keeps freeing slots, the 1 ms window keeps re-admitting, lease frees do not
  back off admission, and every claim sets `hot_mode` again. L=16: -2 to -9%.
- Fixes (2026-11-26): count a cold flag set at the hot node's R entry check as cold seen;
  decide eviction from 4 hot acquisitions (at least half saw a cold node); back off
  admission after every free, lease included; default window 100 us.
