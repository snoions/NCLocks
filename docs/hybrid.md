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
