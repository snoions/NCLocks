# Phase 2b: gate + owner cross-node handoff — implementation notes

The protocol is design.md §3.3 (Decided); this file records implementation choices and
measurements. Mode `NCL_MODE_HANDOFF` (`-M handoff`) = Phase 2a local cohort passing
(`docs/local_handoff.md`) plus cross-node handoff through a gate and an owner field.
Variants (a) Lamport, (b) tournament, (d) hybrid serve as the gate; (c)/(e) are rejected
(they hand off through the server queue; use `-M local`), as is the no-submutex ablation.

## Implementation (`src/lock.c`: `handoff_acquire`, `handoff_release`)

- **Layout:** per lock, after the base-algorithm lines: one uncached `owner` line
  (0 = FREE, node+1 = held) and `announce[N]`, one line per node. Epoch bits in the
  owner word (§3.5 Open) are not implemented.
- **Acquire** (caller holds the submutex): gate = `ncl_base_lock`; announce; fence;
  read `owner`; if FREE or me, store me (one 8-byte store), fence, `ncl_base_unlock`.
  Otherwise leave the gate and poll `owner`: me = handoff received, FREE = missed
  announcement (back through the gate). On success: fence, `announce = 0`.
- **Release** (never enters the gate): data writeback as before; scan `announce[]`
  round-robin from me+1; fence; store the first waiter (handoff) or FREE; then the
  submutex unlock. Local cohort passing happens first, exactly as in 2a.
- **Announce placement (§3.5 Open):** `-G inside` (default, the spec's baseline: announce
  after entering the gate) or `-G before` (announce, fence, then the gate). Both are safe
  by the "owner == me means handoff" rule.
- **Safety sketch:** `owner` is written only by the holder (at release) or by a gate user
  that read FREE (or its own ID); gate users are mutually excluded, and a FREE owner has no
  holder, so no two writers race. A releaser that misses an announcement stores FREE after
  that waiter's owner read returned "held", so the waiter's later poll sees FREE and it
  retries. Not model-checked (design §5).
- **Owner poll: backoff + spin-then-yield.** A handoff commits the lock to one specific
  node; if its waiting thread is descheduled, the lock idles while other spinners hold the
  CPUs. Four concurrent stress runs (48 threads, 20 CPUs) took 14 s without the yield vs
  0.14 s in Phase 1, and 0.77 s with it. Same convention as the submutex and server waits.
- **Stats** (`struct ncl_handoff_stats`, CSV columns `global_handoff_frac`,
  `local_handoff_frac`, `missed_announce_per_acq`, `owner_polls_per_acq`,
  `gate_entries_per_acq`, `announce_loads_per_acq`); path `NCL_PATH_GLOBAL_HANDOFF`.
- **Tests:** `stress_{lamport,tournament,hybrid}_{emu,uc}_handoff[_before_cap1]`
  (`-K 1` makes almost every release a cross-node handoff), emu negative tests with
  `--skip-wb` / `--skip-inv` in handoff mode, and `handoff_rejects_server`.

## Measurements (uc, 200 ns CS, 64 B footprint, `-B 16`, 3 interleaved reps)

Raw data: `results/handoff_uc.csv`.

**Uncontended** (Poisson 100k; acquire p50; hold = sojourn p50 − acquire p50):

| | Phase 1 | handoff (inside) | handoff (before) |
|---|---|---|---|
| tournament N=4 | 415 ns | 799 ns | 831 ns |
| tournament N=8 | 575 ns | 991 ns | 1023 ns |
| Lamport N=8 | 383 ns | 735 ns | 767 ns |
| hybrid N=8 | 543 ns | 1087 ns | 1087 ns |

The design expected about +1 round trip (the owner read). Measured: about +4, i.e.
announce store + fence, owner load, owner store + fence, announce clear. The release adds
the `announce[]` scan, N−1 loads when nobody waits (6.7 loads/acq at N=8): tournament hold
time at N=8 grows ~416 -> ~930 ns.

**Capacity** (closed loop, Mops/s; fairness min/max):

| | Phase 1 / 2a | handoff inside | handoff before |
|---|---|---|---|
| tournament 4x1 | 1.57 | 0.93 | 0.94 |
| tournament 8x1 | 1.66 (0.98) | 0.92 (1.00) | 0.92 (1.00) |
| hybrid 4x1 / 8x1 | 1.11 / 1.13 | 0.90 / 0.90 | 0.88 / 0.90 |
| Lamport 4x1 | 0.84 (0.75) | 0.94 (1.00) | 0.93 (1.00) |
| Lamport 8x1 | 0.50 (0.84) | **0.93 (1.00)** | 0.92 (1.00) |
| 4x2 (vs 2a local) | 2.43-2.46 | 2.43-2.46 | 2.45-2.47 |

Saturated, nearly every acquisition is a global handoff (the gate is idle), so capacity is
the handoff cycle, ~0.92M/s for every gate. A diagnostic run (tournament 8x1) rules out
poll backoff (`-B 1`: 0.85M, `-B 4`: 0.95M, `-B 64`: 0.54M) and data movement (no footprint:
1.03M vs Phase 1 2.16M; no CS: 1.02M vs 2.29M). The cycle is about 1 us with four
serialized uncached operations (releaser's owner store, waiter's poll, waiter's
`announce = 0`, which the next UC load waits for, and the releaser's `announce[]` scan load)
plus two `mfence`s. A saturated tournament instead pipelines its lower levels and pays about
one store plus one load per handoff. At 4x2, 98% of acquisitions are local handoffs, so 2b
equals 2a.

**Open loop, N=8** (acquire p50 / p99):

| offered | tournament P1 | tournament handoff | Lamport P1 | Lamport handoff | hybrid P1 | hybrid handoff |
|---|---|---|---|---|---|---|
| 300k | 607 ns / 2.2 us | 1.0 / 4.6 us | 383 ns / 4.4 us | 799 ns / 6.1 us | 543 ns / 4.0 us | 1.2 / 6.1 us |
| 600k | 607 ns / 3.2 us | 1.8 / 10 us | sat (19%) | 2.0 / 23 us | 927 ns / 10 us | 2.4 / 13 us |
| 900k | 895 ns / 5.4 us | 27 / 279 us | sat (50%) | 111 / 688 us | 4.1 / 32 us | 82 / 819 us |
| 1.2M | 1.3 / 10 us | sat (26%) | sat | sat (25%) | sat (6%) | sat (27%) |

On/off bursts (1 ms period, 25% on, so 4x the mean rate while on), p50: at 300k mean,
tournament 1.3 us (P1) vs 35 us (handoff), Lamport 156 us vs 45-57 us; at 600k mean, the
tournament 49 us vs 205 us. The bursts exceed handoff's capacity.

**Announce placement:** `before` cuts missed announcements ~100x (tournament 600k: 0.051
-> 0.0002 per acquisition) and raises the handoff share (65 -> 73%); latency and capacity
are otherwise about the same. Recoveries are rare in either case (at most 7.5% of
acquisitions, at moderate load).

**Summary.** Gate + owner pays off where the base algorithm is expensive to hold across the
CS (Lamport: 0.50 -> 0.93M at N=8 and starvation gone). With the tournament or hybrid as the
gate it costs 20-45% capacity and about +400 ns uncontended on this platform. Candidate
optimizations: announce lazily (only after reading a held owner) and a hinted `pick_waiter`.
