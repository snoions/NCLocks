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
