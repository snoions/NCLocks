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
  once (`gate_entries_per_acq` = 1.0) to read "held". A shorter handoff brings these gate
  passes closer together, so they overlap and contend, and on CXL a contended Lamport
  gate spins on `x`/`y`/`b[]`. The tournament's gate degrades gracefully; Lamport's
  collapses. The dev box shows the same effect on a smaller scale (Lamport retries
  0 -> 0.6 with `grant,clear`).
  This led to `--ho skip` (read `owner` before the gate, adaptively; docs/handoff.md),
  which takes the gate off every acquisition at saturation; not yet measured on CXL.
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
  candidate is that 8-byte UC stores reach the device as partial writes (`MemWrPtl`) that
  it serves slowly or serially; untested (it would need device-side counters or a
  memlat run with concurrent UC streams).
- **With the ceiling gone, uc beats cf on CXL everywhere** (tournament 7.1M vs 3.4M,
  Lamport 15.9M vs 7.1M at 32x1 L=256), and the many-locks ranking is the same as on CXL
  cf: Lamport, then the server, at L=256; the server at L=16 (5.8M / 6.5M).
- At L=16 Lamport is still the worst at 32 nodes (827k, p99 tails), as on CXL.
- **Server unfairness persists but is smaller:** 0.94 at 16x1 and 0.80 at 32x1, L=256
  (CXL 0.78 / 0.64). It grows with load on the servers, so it is a server-side effect, not
  a CXL one.
