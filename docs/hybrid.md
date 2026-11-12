# Hybrid (d) and fast-path server (e): implementation notes

The protocols are specified in `design.md` §4d and §4e. That is the source of
truth, and this file only records implementation choices and measurements.
**Neither protocol is model-checked yet** (design §5).

## History

The first implementation of (d) (commit `ad9f252`..`4c6b0b6`) used a
Lamport-style `x`/`y`/`b[]` splitter, a per-node `fflag[]` on the top lock's
fast side, and a home-grown reset rule. On 2026-11-09 the design replaced it
with the race/door/present splitter and the Aspnes §18.5.2 reset rule, and the
code now follows the design. The old code and its measurements remain in git
history. The main lesson carries over: in the old version, O(P) uncached scans
on the slow path, especially one done while holding the lock, cost up to 50% of
capacity at N=16.

## Implementation choices

- **Shared primitives** (`include/ncl/base.h`): `ncl_splitter_enter`,
  `ncl_splitter_open`, `ncl_splitter_reset_if_quiet`, `ncl_present_clear` and
  the top Peterson lock split into `announce` / `may_enter` / `unlock`. (d) and
  (e) use the same code.
- **Encoding:** `race` is 0 (none) or node+1. `door` is OPEN = 0, so the zeroed
  metadata is the initial state, and CLOSED = 1. Each variable is on its own
  uncached line (design §2 rule 3; dense packing awaits the partial-write test).
- **Reset check:** `door` is read, then a fence, then `present[]`. A node that
  read `door == OPEN` and is about to close it set `present[i]` before its
  fence (1), so the check sees it. The scan starts after the caller's index and
  stops at the first set flag. Reading order does not affect safety.
- **(d) release order** follows the design pseudocode exactly. Fast path:
  top unlock, `door = OPEN`, `present = 0`. Slow path: top unlock, reset
  check, tournament unlock, `present = 0`. *Open:* confirm the order against
  Aspnes and model-check it.
- **(e) server's SLOW side** (`src/server.c`, `advance()`): the server
  announces when a lock's queue becomes non-empty, then checks the Peterson
  condition once per server round instead of spinning inline. That is the same
  protocol, but a fast-path holder of one lock never stalls the server's other
  locks. The server grants only while it holds the SLOW side. When a release
  finds the queue empty, the server unlocks, fences, and runs the reset check
  over all `present[]`, since the releaser cleared its own before sending REL.

## Measurements (4 nodes x 1 thread, `uc`, 200 ns CS, Poisson, 3 reps)

Raw data: `results/load_uc_n4_all.csv`.

| | uncontended p50 (100k) | capacity | notes |
|---|---|---|---|
| Lamport | 383 ns | ~1.15M | huge tail near saturation |
| Tournament | 431 ns | ~1.54M | best capacity |
| Hybrid (d) | 543 ns | ~0.90M | fast path 0% at saturation |
| Server (c) | 2.3 us (backoff 1024) | ~1.35M | p50 depends on server backoff |
| Fast-server (e) | 543 ns | ~1.23M | p50 independent of server backoff |

- **(d) under contention** is slower than the tournament alone (0.90M vs
  1.54M). Once contended, the door stays CLOSED (by design) and every
  acquisition pays tournament + top lock + reset check. The reset check (a door
  load, a fence, and at least one `present[]` load) runs *between* the top
  unlock and the tournament unlock, so it lands on every slow-to-slow handoff.
  Moving it after `tournament_unlock` would take it off the critical path, but
  that is exactly the ordering the design marks Open, so it needs the model
  check first.
- **(e) vs (c), server idle backoff** (`-S`, 100k req/s):

  | cap | (c) p50 / p99 | (e) p50 / p99 | (c) polls/s | (e) polls/s |
  |---|---|---|---|---|
  | 1 | 607 ns / 1.9 us | 543 ns / 1.7 us | 34.3M | 34.3M |
  | 128 | 1.6 us / 5.4 us | 543 ns / 4.9 us | 6.3M | 3.8M |
  | 1024 | 2.3 us / 30 us | 543 ns / 29 us | 3.4M | 0.76M |
  | 8192 | 2.3 us / 31 us | 543 ns / 197 us | 3.1M | 0.37M |

  This confirms the design's claim: (e)'s median does not depend on the
  server's backoff, so its server can back off hard (4.5x less poll traffic at
  the default cap). The cost lands on the ~7% of deflected acquisitions, which
  meet a backed-off server, and shows up in p99.
- **(e) at moderate contention** (1M req/s): the fast path is down to 8%, and
  p99 is 590 us versus 12 us for (c). The queue drains and refills often, so
  the server repeatedly releases and reacquires the SLOW side, and grantees
  wait out fast-path holders. Under contention (e) is worse than plain (c) here.

## Rejected: moving (d)'s reset check after `tournament_unlock`

It looks attractive because it would take the check off the slow-to-slow
handoff path, but it is **unsafe**. The tournament is what serializes
resetters: with the design's order, a resetter's `door` read cannot be stale
relative to another resetter's blind `door = OPEN` store. Moving the check
after the unlock lets two checks overlap:

1. s (slow releaser, tournament already released) reads `door == CLOSED`,
   scans `present[]` and finds all 0, then stalls before `door = OPEN`.
2. s2 sets `present[s2]` after s's scan has passed index s2, is deflected
   (door still CLOSED), takes the tournament and top SLOW side, runs its CS,
   releases, and starts its own check. It reads `door == CLOSED` (stale), and
   its scan reads `present[f] == 0`.
3. s writes `door = OPEN`, then clears `present[s]`.
4. f sets `present[f]` after s2's scan has passed index f, sees OPEN, closes
   the door, and wins the splitter.
5. s2 reads `present[s] == 0`, finds all 0, and writes `door = OPEN` while f is
   a fast winner.
6. g sees OPEN, closes the door, and wins the splitter. f and g are now both on
   the FAST side, which the top Peterson lock does not arbitrate.

Stress testing is unlikely to hit this (it needs two well-placed delays); a
model check of any reordering is mandatory.

## Optimization: unfenced pre-check before the reset check (implemented)

`ncl_splitter_reset_if_quiet` first reads one `present[]` slot (the one last
seen set, kept in node-local `struct ncl_base::reset_hint`) with no fence. If it
is set, the check is skipped. Skipping a reset is always safe: it only leaves
the fast path closed longer. Otherwise the full check runs unchanged (door
load, fence, every `present[j]` re-read), so the safety argument is untouched,
and the check stays before `tournament_unlock`.

A/B against `85e4956` (4 nodes x 1 thread, `uc`, 200 ns CS, interleaved runs):

| | baseline | pre-check |
|---|---|---|
| closed-loop capacity (median of 5) | 0.899M [0.896-0.906] | **1.065M** [1.052-1.116] (+18%) |
| 750k p50 / p99 / p99.9 | 1.2 / 41 / 139 us | 0.9 / **15** / **53** us |
| 850k p50 / p99 | 5.6 / 147 us | **1.6 / 45** us |
| 300k, 600k | unchanged | unchanged (p99 at 600k 12.8 -> 8.7 us) |

Pre-check hit rate (checks it ends): ~100% saturated, 74% at 850k, 44% at
300k. Fast-path share at 850k rose from 17% to 35%: handoffs are faster, so
contention episodes end, and the door reopens, more often.

**Tail at 800-900k (8 interleaved reps each; `results/precheck_ab_uc_n4.csv`):**

| offered | version | p50 | p99 median [range] | p99.9 median [range] | max (median) | fast |
|---|---|---|---|---|---|---|
| 800k | baseline | 2.0 us | 70 us [61-102] | 172 us [139-229] | 284 us | 31% |
| 800k | pre-check | 1.2 us | 22 us [22-31] | 242 us [61-557] | 587 us | 42% |
| 850k | baseline | 7.0 us | 217 us [156-721] | 434 us [262-950] | 608 us | 14% |
| 850k | pre-check | 1.5 us | 29 us [24-43] | 281 us [74-557] | 574 us | 36% |
| 900k | baseline | 688 us | 57 ms (2.8% unserved: saturated) | 58 ms | 58 ms | 0% |
| 900k | pre-check | 2.0 us | 44 us [37-55] | 356 us [90-655] | 647 us | 29% |

p50 and p99 improve 2-5x in every run, and at 900k the baseline is saturated
while the pre-check is not. p99.9 is **not** systematically worse (an earlier
3-run sample suggested it was). It is better at 850k and 900k and overlaps at
800k. What is real: the pre-check version has rare stalls of about 0.6 ms
(max is about 0.6 ms in nearly every run, versus about 0.28 ms for the
baseline at 800k), which makes its p99.9 bimodal across runs (60-90 us or
500-650 us). Cause unknown. Candidates are the more frequent fast/slow
alternation through the top Peterson lock (the fast share is 29-42% versus
0-31%), or OS noise that now stands out against shorter queues. Next step if
needed: per-path latency histograms in `ncl_bench`.

## Fairness fixes (implemented; design §4d/§4e updated; not model-checked)

The re-baseline with per-node fairness (`results/rebaseline_uc_n4.csv`) found:

1. **(e) starvation.** The server held the top lock's SLOW side until its queue
   emptied, which never happens under sustained contention, so a node that had
   won the splitter waited forever (closed loop: one node with 0
   completions). Fix (`src/server.c`, `advance()`): between grants, if
   `flag[FAST]` is set and at least one grant was made in this hold, unlock
   and re-announce the SLOW side (turn = SLOW, so the fast winner goes first).
   Exercised in open loop: 247 yields at 500k and 553 at 900k, with max
   latency 51-66 us (before: p99 590 us at 1M).
2. **(d)/(e) fast-path monopoly.** The fast winner reopened the door
   unconditionally, so its own node re-won while everyone else sat on the slow
   path. Top-Peterson alternation gave that node about 50%, and its absence
   from the tournament gave its leaf sibling 25%: completions split 4:2:1:1.
   Fix: `ncl_splitter_open_unless_others` reopens only if no other
   `present[j]` is set. After both fixes, fairness is 1.000 for (d) and (e) in
   both modes.

Cost of fix 2 (uc, 4 nodes, versus `45a3225`): acquire p50 is unchanged
(543 ns). Hold time (sojourn p50) is +250-320 ns from the N−1 = 3 extra loads on
a lone fast release. The fast-path share falls (hybrid 94 -> 91% at 100k and
82 -> 73% at 300k; (e) 76 -> 49% at 300k, so its p50 rises from 575 ns to
1.0 us). A cheaper variant would probe only the remembered slot (1 load), at
the cost of weaker fairness.

## Scaling to N=8 (uc, Phase 1, 8 x 1, 200 ns CS, `-B 16`, 3 reps)

Raw data: `results/scale_closed_uc_n4n8.csv` (closed loop, N=4 and N=8) and
`results/load_uc_n8_all.csv` (Poisson sweep, N=8). With 10 physical cores, the 8
workers get one core each, but at least six of the eight (c)/(e) server threads must
share a core. The sweep therefore uses `-Y sibling`, which puts each node's server on
the SMT sibling of its own worker so that all nodes are placed alike. At N=8,
sibling and next-CPU placement give the same capacity. At N=4, sibling placement
costs (c) 7% (1.40 -> 1.30M) and (e) 3%.

Closed-loop capacity (Mops/s, median; fairness = min/max node completions):

| | N=4 | N=8 | fairness N=8 |
|---|---|---|---|
| Tournament | 1.54 | **1.70** | 0.98 |
| Server (c) | 1.40 (next) / 1.30 (sibling) | 1.13 / 1.16 | 1.00 |
| Hybrid (d) | 1.13 | 1.11 | 1.00 |
| Fast-server (e) | 1.09 / 1.06 | 1.03 / 1.04 | 1.00 |
| Lamport | 0.83 | **0.50** (4.2 retries/acq) | 0.86 |

Open loop at N=8 (acquire p50 / p99; "sat" = offered load not sustained):

| offered | tournament | hybrid (d) | fast-server (e) | server (c) | Lamport |
|---|---|---|---|---|---|
| 100k | 575 ns / 1.5 us | **543 ns** / 2.6 us | 543 ns / 33 us | 1.8 / 29 us | 383 ns / 2.0 us |
| 300k | 607 ns / 2.2 us | 575 ns / 4.6 us | 1.2 / 30 us | 1.0 / 8.2 us | 383 ns / 4.4 us |
| 600k | 607 ns / 3.3 us | 1.5 / 11 us | 1.9 / 13 us | 1.2 / 6.9 us | sat (20% unserved) |
| 900k | 0.9 / 5.4 us | 4.6 / 33 us | sat | 3.1 / 35 us | sat |
| 1.2M | 1.3 / 11 us | sat | sat | sat | sat |
| 1.5M | 2.8 / 28 us | sat | sat | sat | sat |

- **The tournament improves with N.** Under saturation, the next winner already holds
  the lower levels of its subtree, so handoffs get shorter. Its uncontended p50 rises
  415 -> 575 ns (one more level).
- **Lamport collapses**: below 600k open-loop capacity, with seconds-long tails.
- **The (d) fast path now beats the tournament on acquire** (543 vs 575 ns at 100k):
  its cost does not depend on N.
- **(d)'s release gives that back and more.** Hold time (sojourn p50 - acquire p50)
  at 100k is about 1024 ns for (d) versus about 416 ns for the tournament. The extra
  ~600 ns matches the measured 6.56 `present[]` loads per acquisition x ~82 ns: the
  N-1 = 7-slot scan in `ncl_splitter_open_unless_others` (fairness rule) on a lone
  fast release. Because it lengthens the hold, it also brings saturation earlier
  (between 900k and 1.2M, versus > 1.5M for the tournament).
- When saturated, the scan disappears (0 scan loads per acquisition in closed loop),
  because the reset pre-check decides with one load. Scans cost the most at light and
  moderate load, which is exactly where the hybrid is meant to win.

Candidates to cut the release scan:
1. Probe only the remembered slot on a fast release: 1 load, weaker fairness, no
   layout change.
2. Pack `present[]` densely so that one load reads several slots. This requires
   sub-line writes (design §2 rule 3, Open; §5 partial-write test). Byte packing
   would also break the one-8-byte-store invariant and introduce mixed-size accesses.
   Expected saving at N=8: about 500 ns per hold.

## Fast-release check: hint (default) vs scan

The fairness rule's scan of every other `present[]` slot is the ~600 ns hold-time cost
found at N=8. The default (`-F hint`, `ncl_fast_release = NCL_FASTREL_HINT`) now
probes only the slot last seen set (`reset_hint`, kept current by reset checks and by
this rule). If that slot is set the door stays CLOSED; otherwise it reopens. A miss
can reopen while some other node waits, which is exactly the design's original
unconditional reopen, so safety is unaffected. Only fairness is weaker in principle.
`-F scan` keeps the strict rule. The ctests `stress_{hybrid,fastsrv}_{emu,uc}_fastrel_scan`
cover it.

A/B (uc, 200 ns CS, `-B 16`, 3 interleaved reps; `results/fastrel_ab_uc.csv`).
Hold time is sojourn p50 minus acquire p50:

| | scan: hold, loads/acq | hint: hold, loads/acq |
|---|---|---|
| (d) N=8, 100k | ~990 ns, 6.57 | **~545 ns**, 1.09 |
| (d) N=8, 300k | ~1025 ns, 5.66 | **~545 ns**, 1.35 |
| (d) N=4, 100k | ~670 ns, 2.90 | ~545 ns, 1.02 |
| (e) N=8, 100k | ~990 ns, 5.84 | **~545 ns**, 1.13 |

With hint, (d)'s hold time at N=8 is close to the tournament's (~416 ns). Shorter
holds end contention episodes sooner, so the fast path is used more:

| N=8 | scan: p50, fast share | hint: p50, fast share |
|---|---|---|
| (d) 300k | 575 ns, 62% | 543 ns, 75% |
| (d) 600k | 1.47 us, 26% | **927 ns**, 44% |
| (e) 300k | 1.22 us, 44% | **575 ns**, 62% |
| (e) 600k | 1.86 us, 18% | 1.66 us, 33% |

- **Capacity unchanged** (closed loop, scan vs hint): (d) 4x1 1.150 / 1.128M, 8x1
  1.121 / 1.126M, 4x2 local 2.459 / 2.438M; (e) 4x1 1.105 / 1.105M, 8x1 0.998 /
  1.018M, 4x2 local 1.781 / 1.780M. When saturated the fast path is almost never used.
- **Fairness 1.00 in every closed-loop configuration** (minimum across reps 1.00),
  including 4x2 local, where the unconditional reopen gave 4:2:1:1. Under contention
  the remembered node is almost always still present.
- **Tails inconclusive.** At N=8, 600k, the median p99.9 is worse with hint ((d) 18 ->
  31 us, (e) 25 -> 221 us). At N=4, 1M, it is better ((d) 295 -> 147 us). Both
  variants show the bimodal ~0.85 ms stalls, and the spread within a variant (e.g.
  (e) 600k p99.9 per rep: scan 25/172/25 us, hint 557/31/221 us) exceeds the
  difference between variants. In open loop, weaker fairness would show up as tail
  latency rather than as uneven completions, so a real cost could be hiding here.
  Settling it needs more reps or per-path latency histograms.

## Anderson-Kim fast path (`-a ak`; not model-checked)

Anderson & Kim, "A new fast-path mechanism for mutual exclusion", Distrib. Comput. 14
(2001), Figure 4 (bounded version; https://cs.unc.edu/~anderson/papers/dc00.pdf), in
`src/base.c` with the paper's statement numbers. Slow path = our tournament, ENTRY 2 = our
top Peterson lock. Instead of the splitter and `present[]` it uses `X`, `Y = (free, indx)`
(one word), `Reset`, `Infast`, `Name_Taken[N]` and `Obstacle[N]`, all single-word
metadata. The reopen check reads one `Name_Taken` and one `Obstacle` entry instead of
scanning `present[]`. The cost is one more store-fence-load step on the fast path
(`Name_Taken` vs `Reset`) and several more stores.

- **As published (`-F always`) it is unfair:** the fast winner always reopens, and the
  quickest node keeps winning (closed loop fairness 0.36 at N=8, 0.25 at N=16), as our
  hybrid did before its fairness fix.
- **Default = fairness variant (ours):** the fast winner reopens only if the top lock's
  SLOW flag is clear (one load), and every slow-path releaser runs the reset (statements
  29-37), not only SLOW 2 processes; otherwise the fast path could stay closed. The
  reset's safety argument (Name_Taken/Reset and Obstacle/Reset Dekker pairs, `Infast`,
  statements 29-30) does not depend on which critical-section holder runs it, but this
  is an argument, not a proof.

**uc, 200 ns CS, `-B 16`, 3 interleaved reps, medians** (`results/ak_vs_hybrid_uc.csv`).
Closed loop: Mops/s (fairness). Open loop: sojourn p50 (us); onoff = `-r 200k -A
onoff:200:0.25` (bursts at 800k). hybrid-scan = hybrid `-F scan` (full `present[]` scan at
every fast release).

| N, -L | load | hybrid | hybrid-scan | ak (fair) | ak as published |
|---|---|---|---|---|---|
| 8, 0 | closed | 1.12 (1.00) | 1.12 (1.00) | 0.93 (0.97) | 1.31 (0.36) |
| 8, 0 | 100k | **1.09** | 1.54 | 1.54 | 1.41 |
| 8, 0 | 600k | 1.79 | 2.56 | 2.18 | 1.60 |
| 8, 0 | onoff | 2.69 | 3.46 | 3.20 | 1.92 |
| 16, 0 | closed | 1.06 (1.00) | 1.06 (1.00) | 0.93 (0.91) | 1.32 (0.25) |
| 16, 0 | 100k | **1.09** | 2.18 | 1.54 | 1.47 |
| 16, 0 | 600k | 3.46 | 4.86 | **2.30** | 1.66 |
| 16, 0 | onoff | 4.35 | 5.63 | **3.20** | 1.92 |
| 16, 300 | closed | 0.26 (1.00) | 0.26 (1.00) | 0.21 (0.93) | 0.30 (0.14) |
| 16, 300 | 100k | **10.8** | 15.9 | 12.3 | 9.2 |
| 16, 300 | onoff | **74** | 74 | 377 | 57 |

(600k with `-L 300` saturates every variant: 56-73% unserved.)

- **Uncontended the hybrid wins** by about 450 ns (N=8 or 16, no injected latency): AK's
  extra fenced step plus its extra stores (Obstacle, Name_Taken, Infast, Reset, Y).
- **The O(N) scan is real:** hybrid-scan is 450 ns slower than hybrid at N=8 and 1.1 us
  at N=16 uncontended. The default hybrid avoids most of it with the hinted pre-check.
- **At N=16 under moderate or bursty load the fair AK wins** (600k: 2.3 vs 3.5 us; onoff:
  3.2 vs 4.4 us). It keeps the fast path in use (fast fraction 0.41 vs 0.26 at 600k):
  the hybrid's fairness rule keeps the door closed while anyone is present and reopens
  only when quiet, AK's reopens after every slow release. Part of the gain is that
  policy, not only the O(1) reset.
- **The fair AK pays in capacity** (0.93 vs 1.06-1.12): every slow release runs the reset
  inside the critical section (two more round trips of hold time). With 300 ns device
  latency that lower capacity makes bursts queue much longer (onoff p50 377 vs 74 us).
- **AK as published is the fastest in every open-loop row and in capacity, but closed-loop
  fairness 0.14-0.36** rules it out under saturation.
- Net: no variant dominates. For N <= 8 the hybrid stays the better choice; at N=16 with
  moderate load the fair AK is better, unless the device is slow or the load saturates.
