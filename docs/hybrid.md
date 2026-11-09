# Hybrid (d): fast-path reset rule

Status: **Proposed, implemented in `src/base.c`, NOT model-checked.** Runtime
stress passes. A mutant that resets the splitter without the `b[]` scan fails
the stress test immediately, but that only shows the scan is necessary, not
that the rule is sufficient. Model-check it before benchmarking (design §4d, §5).

## Variables (all hot metadata, one per line)

Participants `i = 0..P-1`, ids `me = i+1` (0 = empty).

- Splitter: `x`, `y`, `b[i]`
- Tournament: Peterson `flag[2]`, `turn` per internal tree node
- Top two-party Peterson: `sflag` (slow side), `turn`, and **per-participant
  `fflag[i]`** for the fast side. The fast side's "flag" is `OR_j fflag[j]`.

## Protocol

```
acquire(i):
    b[i]=1; ss; x=me; F
    if y != 0:            goto slow
    y=me; F
    if x != me:           goto slow
    # splitter won: b[i] stays 1 until release
    fflag[i]=1; ss; turn=FAST; F
    await !(sflag && turn==FAST)
    path=FAST
  slow:
    b[i]=0
    tournament_lock(i)
    sflag=1; ss; turn=SLOW; F
    await !(turn==SLOW && OR_j fflag[j])
    path=SLOW

release(i):                       # path read from node-local lock state
  FAST:  F; y=0; ss; b[i]=0; ss; fflag[i]=0
  SLOW:  F
         if y != 0:                       # possibly stale splitter
             F; if all b[j]==0: F; y=0    # reset; else skip
         ss; sflag=0; ss; tournament_unlock(i)   # root to leaf
```

## Why each choice

1. **Stale `y`.** A splitter round can end with nobody winning (`y != 0`, all
   contenders failed `x == me`). Until `y` returns to 0 every fast attempt fails.
   That is a liveness problem only.
2. **Fast winner clears `y` and `b[i]` *before* `fflag[i]`.** While `fflag[i]=1`
   it still holds the top lock, so no slow holder can run the reset at the same
   time. If `y=0` were written after leaving the top lock, a slow holder could
   read the old `y`, see `b[]` clear, and then blindly write `y=0` over a new
   winner's `y`. That allows two simultaneous fast winners.
3. **Per-participant `fflag`.** After the old winner writes `y=0`, a new fast
   winner may arrive at the top lock before the old winner clears its flag.
   With a single shared fast flag, the old winner's `flag=0` would erase the new
   winner's `flag=1`. Separate flags avoid that. The slow side waits on their
   OR, so Peterson's argument still holds: at most one fast-side process is ever
   trying or in the CS, because the old one is only clearing state.
4. **Reset only if every `b[j]==0`.** A fast winner keeps `b=1` until its
   release, and an in-flight attempt has `b=1` from before it writes `x` until it
   fails. So "all clear" means no winner exists and no attempt is between "read
   `y==0`" and "write `y`". While `y` is stale, no new attempt can read `y==0`,
   and the only other writers of `y=0` are fast releases (excluded by point 2)
   and other slow resets (excluded because they need the top lock). So the
   blind `y=0` store cannot overwrite a live value.
5. **Skip, don't wait.** If some `b[j]` is set, the reset is skipped and a later
   slow holder retries. Release never blocks on other nodes. Skips are counted
   (`reset_skips`). In stress runs about 97% of stale-`y` observations skip,
   because new attempts keep `b[j]` set briefly. If benchmarks show the fast
   path staying disabled, revisit this.

## Ordering assumption to verify

Point 4 needs the RC11 fence pattern below to be forbidden. A delayed attempt
`k` does `St b[k]=1; F; Ld y→0`. Later the reset sees the newer `y` in
`Ld y→stale; F; Ld b[k]`. It must not then read `b[k]→0`. SC fences forbid this
(psc includes `F; hb; fr; rf; hb; F`), and on x86 TSO it holds with `mfence`.
Confirm under GenMC.

## Cost

Uncontended fast path: three serialized store-fence-load round trips (two
splitter, one top Peterson). Lamport alone needs two. The slow path adds an
O(P) `fflag` scan in the top wait and, on release, the O(P) `b[]` scan.
