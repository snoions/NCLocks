#!/usr/bin/env bash
# Evaluation sweep for a NUMA host with a CXL memory node (docs/cxl_host.md).
#
# Usage: scripts/cxl_sweep.sh [PHASE...]        (default: preflight p1 2a 2b base open)
#
# Defaults cover what still wins somewhere on CXL (docs/cxl_results.md): cf only,
# algorithms tournament, Lamport and the server (ALGS), and in 2b the server chain
# (GATE_ALGS). Hybrid, AK, fastsrv and aksrv lost to one of those three in every
# measured regime, and gate-based 2b never beat Phase 1 tournament; setting ALGS and
# GATE_ALGS to the full lists (ALL_VARIANTS, ALL_GATE_ALGS below) brings them back.
# Phases abl, own and ho answered their questions (results/cxl_2026111*); they stay
# for reruns but are not in the default.
#   preflight  check placement (CPUs, mem node, uc pool node) and stop on mismatch
#   p1         Phase 1, closed loop, ALGS, main configs, all (N,T)
#   abl        Phase 1 ablation: uc uniform + clflushopt (the dev-box default), N=8,16 (done)
#   2a         Phase 2a local handoff (-M local), T=3
#   2b         Phase 2b cross-node handoff (-M handoff, defaults --wait own, --ho all), GATE_ALGS
#   base       coherent baselines: tournament and pthread (+ no-submutex) on CXL and DRAM
#   open       open loop (Poisson) at N=16, T=1, offered load scaled to CXL capacity
#   locks      Phase 1 with L locks chosen uniformly per acquisition (-l), ALGS, N=16,32
#   ho         2b shorter handoff path (--ho none | grant,clear | all = grant,clear,skip), gate algorithms,
#              with Phase 1 tournament and the server chain as references, L=1 and 256 (done)
#   own        2b waiters: shared owner word vs own announce line (--wait), gate algorithms (done)
#   dram       main uc configs with metadata on DRAM: RELOAD THE DRIVER WITH nid=0 FIRST
#   mt         multi-threaded nodes (the deployment case): 2a / 2b / server chain vs Phase 1,
#              tournament and Lamport, at T>1. Four parts, tagged in the config column:
#              cap (local-handoff cap -K, 16x3, L=1), locks (L = 1..256, 16x3), open (open
#              loop, 16x3, L=1 and 256) and nt (threads per node: MT_NT, L=1 and 256)
#   kv         KV store (ncl_kv, docs/kv.md) at KV_NT (16x3), cf, 2a: per-lock algorithm map
#              (top:H hottest stripes on the tournament, rest Lamport; H=0 all Lamport, H=L
#              all tournament) vs the server chain; key skew x node affinity; precise vs
#              full data maintenance; open loop. Parts tagged in the config column.
#   kvreact    KV store with the reactive lock (-a reactive, --react REACT_THR) against the
#              fixed maps (all Lamport, 16 hottest on the tournament, all tournament) and the
#              server chain: Zipf 0.99, uniform, Zipf 0.99 + affinity 0.9; L=64, 1024; open loop
#   kviso      reactive lock: mechanism vs policy. The reactive lock pinned to the oracle map
#              (--algmap top:16 --react off) vs the fixed map and reactive 12:4:64; pinned
#              all-tournament / all-Lamport vs the fixed ones (per-path overhead). KV, Zipf
#              0.99 and uniform, L=1024 (+ L=64 Zipf), closed loop and open loop 1-6M
#   kvbase     KV store: coherent baselines vs the non-coherent locks, all on MEMNODE.
#              Coherent: pthread mutex per stripe (-m: no submutex; and with it), tournament
#              and reactive (2a); cf: reactive 12:4:64 and the 16-hottest map (2a). Zipf 0.99
#              L=1024 and 64, uniform L=1024; open loop up to KVBASE_RATES
#   klayout    KV, 16x3 cf: why the reactive lock's Lamport path costs ~6% on CXL. Uniform
#              L=1024: fixed Lamport (45-line blocks, padded to 46 and 48, plain 2-line), reactive
#              pinned to Lamport with [mode][x][y] vs [x][y][mode] blocks; Zipf L=1024:
#              16-hottest map vs reactive 12:4:64 in both layouts (closed, open 6M)
#   kstride    KV, uniform keys, L=1024, 16x3 cf 2a: throughput vs the stride between lock
#              metadata blocks (lines per lock x 64 B), via --pad-lines: fixed per-lock layout
#              (45 lines +0..6, 56, 64), reactive pinned to Lamport xy-first (48 +0..3, 64),
#              plain Lamport (2 lines padded to 3, 8, 16, 17, 32, 48, 49, 64)
#   conc       ncl_memconc: aggregate throughput of T threads issuing one access pattern
#              (uc load/store/fence/Dekker, same-line load, full-line clwb, 8- vs 64-byte
#              movnti) on uc (pool on MEMNODE), cf on MEMNODE and cf on CPUNODE (DRAM)
#   open2b     open loop at N=16, T=1: 2b (--ho all and none, -X adaptive vs scan) vs
#              Phase 1 and the server chain, L=1 and 256; plus closed-loop L=256 rows
#              for the cost of -X scan when uncontended
#
# One CSV per phase in $OUT (default results/cxl_<date>/), each row prefixed by
# rep and config name; failures and warnings go to $OUT/errors.log. Runs are
# sequential (one uc experiment at a time) and repetitions are interleaved.
#
# env: OUT, BENCH, REPS (3), SECS (3), WARMUP (0.5), CS (200), BACKOFF (16),
#      ALGS ("tournament lamport server"), GATE_ALGS ("server"; -M handoff),
#      OPEN_ALGS (= ALGS),
#      MEMNODE (2: CXL), CPUNODE (0: the socket the device hangs off),
#      NT ("4x1 8x1 16x1 32x1 4x3 8x3 16x3"), OWN_NT ("4x1 8x1 16x1 32x1 16x3"),
#      CFGS ("cf_clwb"; add uc_split_clwb for uc),
#      LOCKS ("1 4 16 64 256"), LOCKS_NT ("16x1 32x1"),
#      HO_NT ("4x1 16x1 32x1 16x3"), HO_LOCKS ("1 256"), HO_OPTS ("none grant,clear all"), RUN_TIMEOUT (120 s per run), WB (clwb),
#      MT_MAIN ("16x3"), MT_CAPS ("8 16 64 256"), MT_LOCKS ("1 4 16 64 256"), MT_NT ("4x3 16x2 16x3 16x4 32x2"),
#      MT_RATES1 / MT_RATES256 (mt open loop at L=1 / 256),
#      KVBASE_RATES ("2000k 6000k 10000k 15000k 20000k"), REACT_THR ("12:4 6:4:256 4:4:256 4:2:1024"), KV (build/ncl_kv), KV_NT ("16x3"), KV_RATES ("1000k 2000k 4000k 6000k 8000k"),
#      CONC_T ("1 2 4 8 16 24 32 48 60"), CONC_SECS (1), MEMCONC (build/ncl_memconc),
#      RATES1 / RATES256 (open2b offered loads at L=1 / L=256), OPEN2B_N (16)
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BENCH=${BENCH:-$ROOT/build/ncl_bench}
OUT=${OUT:-$ROOT/results/cxl_$(date +%Y%m%d_%H%M)}
REPS=${REPS:-3}
SECS=${SECS:-3}
WARMUP=${WARMUP:-0.5}
CS=${CS:-200}
BACKOFF=${BACKOFF:-16}
MEMNODE=${MEMNODE:-2}
CPUNODE=${CPUNODE:-0}
# Nodes run several threads (T>1) in the deployment; T=1 makes every handoff
# cross-node and is a worst case only (NT="... 16x1" to include it).
NT=${NT:-"4x3 8x3 16x2 16x3 32x2"}
OWN_NT=${OWN_NT:-"4x1 8x1 16x1 32x1 16x3"} # phase own
LOCKS=${LOCKS:-"1 4 16 64 256"}             # phase locks
LOCKS_NT=${LOCKS_NT:-"16x3 32x2"}
HO_NT=${HO_NT:-"4x1 16x1 32x1 16x3"}         # phase ho
HO_LOCKS=${HO_LOCKS:-"1 256"}
HO_OPTS=${HO_OPTS:-"none grant,clear all"}
RUN_TIMEOUT=${RUN_TIMEOUT:-120}
CONC_T=${CONC_T:-"1 2 4 8 16 24 32 48 60"} # phase conc
CONC_SECS=${CONC_SECS:-1}
MEMCONC=${MEMCONC:-$ROOT/build/ncl_memconc}
RATES1=${RATES1:-"25k 50k 100k 200k 300k 400k 450k"}             # phase open2b, L=1
RATES256=${RATES256:-"250k 500k 1000k 1500k 2000k 2500k 3000k"} # phase open2b, L=256
OPEN2B_N=${OPEN2B_N:-16} # open2b is the T=1 (worst-case) study; mt covers T>1
OPEN_T=${OPEN_T:-3}       # phase open: threads per node
KV=${KV:-$ROOT/build/ncl_kv}
KV_NT=${KV_NT:-16x3}
# U:D[:W]: up when >= U of the last 16 contended; down when <= D of the last 64, after
# >= W acquisitions on the tournament (default 64)
KVBASE_RATES=${KVBASE_RATES:-"2000k 6000k 10000k 15000k 20000k"}
REACT_THR=${REACT_THR:-"12:4 6:4:256 4:4:256 4:2:1024"}
KV_RATES=${KV_RATES:-"1000k 2000k 4000k 6000k 8000k"}
MT_MAIN=${MT_MAIN:-16x3} # mt parts cap, locks, open
MT_CAPS=${MT_CAPS:-"8 16 64 256"}
MT_LOCKS=${MT_LOCKS:-"1 4 16 64 256"}
MT_NT=${MT_NT:-"4x3 16x2 16x3 16x4 32x2"}
MT_RATES1=${MT_RATES1:-"100k 250k 500k 750k 1000k 1250k"}
MT_RATES256=${MT_RATES256:-"500k 1000k 2000k 3000k 4000k 5000k 6000k"}
WB=${WB:-clwb} # writeback instruction of the main configs (clflushopt for hosts without clwb)

NUMA=(numactl --cpunodebind="$CPUNODE" --membind="$CPUNODE")
COMMON=(-s "$SECS" -W "$WARMUP" -c "$CS" -B "$BACKOFF" -C)
ALL_VARIANTS="tournament hybrid ak server fastsrv aksrv lamport"
ALL_GATE_ALGS="lamport tournament hybrid ak server" # -M handoff (server = chain)
ALL_ALGS=${ALGS:-"tournament lamport server"}
GATE_ALGS=${GATE_ALGS:-"server"}
OPEN_ALGS=${OPEN_ALGS:-$ALL_ALGS}

# Main configurations: name -> ncl_bench options.
declare -A CFG=(
  [uc_split_clwb]="-b uc -U split --wb $WB --mem-node $MEMNODE"
  [cf_clwb]="-b cached-flush --wb $WB --mem-node $MEMNODE"
)
# Default cf only: uc (really uncached) was slower in every phase of the 2026-11-12/13
# sweeps and hits an aggregate ceiling at low contention (docs/cxl_results.md, "Many
# locks"). CFGS="uc_split_clwb cf_clwb" brings uc back; phase dram is uc regardless.
MAIN_CFGS=${CFGS:-cf_clwb}
for c in $MAIN_CFGS; do [ -n "${CFG[$c]+x}" ] || { echo "cxl_sweep: unknown config $c" >&2; exit 1; }; done

mkdir -p "$OUT"
ERR=$OUT/errors.log
die() { echo "cxl_sweep: $*" | tee -a "$ERR" >&2; exit 1; }

# One run: append "rep,cfg,<csv row>" to $1. Aborts the sweep on a placement
# warning (threads spanning NUMA nodes or sharing CPUs): the data would be wrong.
run() {
  local csv=$1 rep=$2 cfg=$3; shift 3
  local o e
  e=$(mktemp)
  o=$(timeout "$RUN_TIMEOUT" "${NUMA[@]}" "${RUNBIN:-$BENCH}" "$@" 2>"$e")
  local rc=$?
  if grep -q WARNING "$e"; then
    cat "$e" >>"$ERR"
    rm -f "$e"
    die "placement warning in: $* (see $ERR)"
  fi
  if [ $rc -ne 0 ] || [ -z "$o" ]; then
    { echo "FAILED rc=$rc: $*"; cat "$e"; } >>"$ERR"
  else
    echo "$rep,$cfg,$o" >>"$csv"
  fi
  rm -f "$e"
}

header() { [ -s "$1" ] || echo "rep,config,$("$BENCH" -H)" >"$1"; }

preflight() {
  command -v numactl >/dev/null || die "numactl not installed"
  [ -x "$BENCH" ] || die "$BENCH not built"
  local want o
  want=$(cat /sys/devices/system/node/node"$CPUNODE"/cpulist)
  o=$("${NUMA[@]}" "$BENCH" -a tournament -b uc --mem-node "$MEMNODE" -n 2 -s 0.5 -W 0.1 2>&1) ||
    die "uc preflight run failed: $o"
  echo "$o" | grep -E '^(mem|cpus):'
  echo "$o" | grep -q "^cpus: $want\$" || die "pinned CPUs are not node $CPUNODE's ($want)"
  echo "$o" | grep -q "mem_node=$MEMNODE(bound)" || die "shared regions not bound to node $MEMNODE"
  echo "$o" | grep -q "meta_memtype=UC" || die "uc metadata is not UC"
  if [ "${EXPECT_POOL:-$MEMNODE}" != any ]; then
    echo "$o" | grep -q "uc_pool_node=${EXPECT_POOL:-$MEMNODE}" ||
      die "uc pool is not on node ${EXPECT_POOL:-$MEMNODE} (insmod ncl_uc.ko nid=...)"
  fi
  echo "preflight OK -> $OUT"
}

nt_split() { N=${1%x*}; T=${1#*x}; }

p1() {
  local csv=$OUT/p1.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in $NT; do nt_split "$nt"
    for a in $ALL_ALGS; do for c in $MAIN_CFGS; do
      # cached-flush has no split layout; server placement: dedicated cores (-Y next)
      run "$csv" "$rep" "$c" -a "$a" -n "$N" -t "$T" ${CFG[$c]} "${COMMON[@]}"
    done; done
  done; done
}

abl() {
  local csv=$OUT/abl.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in 8x1 16x1; do nt_split "$nt"
    for a in $ALL_ALGS; do
      run "$csv" "$rep" uc_uniform_clflushopt -a "$a" -n "$N" -t "$T" -b uc -U uniform \
        --wb clflushopt --mem-node "$MEMNODE" "${COMMON[@]}"
      run "$csv" "$rep" uc_split_clflushopt -a "$a" -n "$N" -t "$T" -b uc -U split \
        --wb clflushopt --mem-node "$MEMNODE" "${COMMON[@]}"
    done
  done; done
}

p2a() {
  local csv=$OUT/2a.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in $NT; do nt_split "$nt"
    [ "$T" -gt 1 ] || continue
    for a in $ALL_ALGS; do for c in $MAIN_CFGS; do
      run "$csv" "$rep" "$c" -a "$a" -n "$N" -t "$T" -M local -K 64 ${CFG[$c]} "${COMMON[@]}"
    done; done
  done; done
}

p2b() {
  local csv=$OUT/2b.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in $NT; do nt_split "$nt"
    for a in $GATE_ALGS; do for c in $MAIN_CFGS; do
      run "$csv" "$rep" "$c" -a "$a" -n "$N" -t "$T" -M handoff ${CFG[$c]} "${COMMON[@]}"
    done; done
  done; done
}

base() {
  local csv=$OUT/base.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in $NT; do nt_split "$nt"
    for node in "$MEMNODE" "$CPUNODE"; do
      run "$csv" "$rep" "coherent_n$node" -a tournament -n "$N" -t "$T" -b coherent --mem-node "$node" "${COMMON[@]}"
      run "$csv" "$rep" "coherent_n$node" -a pthread -n "$N" -t "$T" -b coherent --mem-node "$node" "${COMMON[@]}"
      run "$csv" "$rep" "coherent_n${node}_nosub" -a pthread -n "$N" -t "$T" -m -b coherent --mem-node "$node" "${COMMON[@]}"
    done
  done; done
}

p2b_own() {
  # --wait owner (design §3.3) vs own (grant in the waiter's announce line),
  # interleaved; T=1 makes every handoff cross-node (docs/handoff.md).
  local csv=$OUT/own.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in $OWN_NT; do nt_split "$nt"
    for a in lamport tournament hybrid ak; do for c in $MAIN_CFGS; do for w in owner own; do
      run "$csv" "$rep" "$c" -a "$a" -n "$N" -t "$T" -M handoff --wait "$w" ${CFG[$c]} "${COMMON[@]}"
    done; done; done
  done; done
}

many_locks() {
  # Phase 1 with L independent locks (each its own metadata lines, data block and,
  # for servers, home = hash % N); contention per lock falls as L grows, so the
  # uncontended paths (Lamport/hybrid/AK fast path) and per-lock footprint show.
  local csv=$OUT/locks.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in $LOCKS_NT; do nt_split "$nt"
    for l in $LOCKS; do for a in $ALL_ALGS; do for c in $MAIN_CFGS; do
      run "$csv" "$rep" "$c" -a "$a" -n "$N" -t "$T" -l "$l" ${CFG[$c]} "${COMMON[@]}"
    done; done; done
  done; done
}

p2b_ho() {
  # --ho options (docs/handoff.md "Shorter handoff path"), interleaved, plus the
  # references they are measured against: Phase 1 tournament and the server chain.
  local csv=$OUT/ho.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in $HO_NT; do nt_split "$nt"; for l in $HO_LOCKS; do
    for c in $MAIN_CFGS; do
      run "$csv" "$rep" "$c" -a tournament -n "$N" -t "$T" -l "$l" ${CFG[$c]} "${COMMON[@]}"
      run "$csv" "$rep" "$c" -a server -n "$N" -t "$T" -l "$l" -M handoff ${CFG[$c]} "${COMMON[@]}"
      for a in lamport tournament hybrid ak; do for h in $HO_OPTS; do
        run "$csv" "$rep" "$c" -a "$a" -n "$N" -t "$T" -l "$l" -M handoff --ho "$h" ${CFG[$c]} "${COMMON[@]}"
      done; done
    done
  done; done; done
}

open_loop() {
  local csv=$OUT/open.csv; header "$csv"
  local rates=${RATES:-"25k 50k 100k 150k 200k 250k 300k 400k"}
  for rep in $(seq "$REPS"); do
    for a in $OPEN_ALGS; do for c in $MAIN_CFGS; do for r in $rates; do
      run "$csv" "$rep" "$c" -a "$a" -n 16 -t "$OPEN_T" -r "$r" -A poisson ${CFG[$c]} "${COMMON[@]}"
    done; done; done
  done
}

dram() {
  # Same uc configs with metadata and data on the CPU node's DRAM: the CXL share.
  EXPECT_POOL=$CPUNODE MEMNODE=$CPUNODE preflight
  local csv=$OUT/dram.csv; header "$csv"
  for rep in $(seq "$REPS"); do for nt in 8x1 16x1; do nt_split "$nt"
    for a in $ALL_ALGS; do
      run "$csv" "$rep" uc_split_clwb_dram -a "$a" -n "$N" -t "$T" -b uc -U split --wb clwb \
        --mem-node "$CPUNODE" "${COMMON[@]}"
    done
  done; done
}

mt() {
  # Multi-threaded nodes. Variants: Phase 1 references, cohort passing (2a) and
  # cross-node handoff (2b, --ho all default) for the tournament and Lamport, and
  # the server chain. Config column: <cfg>:<part>.
  local csv=$OUT/mt.csv; header "$csv"
  local V_CLOSED=("-a tournament" "-a lamport" "-a server"
                  "-a tournament -M local" "-a lamport -M local"
                  "-a tournament -M handoff" "-a lamport -M handoff" "-a server -M handoff")
  local V_CAP=("-a tournament -M local" "-a lamport -M local" "-a tournament -M handoff"
               "-a lamport -M handoff" "-a server -M handoff")
  local V_OPEN=("-a tournament" "-a lamport" "-a tournament -M local"
                "-a tournament -M handoff" "-a lamport -M handoff" "-a server -M handoff")
  local V_NT=("-a tournament" "-a tournament -M local" "-a tournament -M handoff"
              "-a lamport -M handoff" "-a server -M handoff")
  local v MN MT
  MN=${MT_MAIN%x*}; MT=${MT_MAIN#*x}
  for rep in $(seq "$REPS"); do for c in $MAIN_CFGS; do
    for k in $MT_CAPS; do for v in "${V_CAP[@]}"; do
      run "$csv" "$rep" "$c:cap" $v -K "$k" -n "$MN" -t "$MT" -l 1 ${CFG[$c]} "${COMMON[@]}"
    done; done
    for l in $MT_LOCKS; do for v in "${V_CLOSED[@]}"; do
      run "$csv" "$rep" "$c:locks" $v -n "$MN" -t "$MT" -l "$l" ${CFG[$c]} "${COMMON[@]}"
    done; done
    for l in 1 256; do
      local rates=$MT_RATES1; [ "$l" = 256 ] && rates=$MT_RATES256
      for r in $rates; do for v in "${V_OPEN[@]}"; do
        run "$csv" "$rep" "$c:open" $v -n "$MN" -t "$MT" -l "$l" -r "$r" -A poisson ${CFG[$c]} "${COMMON[@]}"
      done; done
    done
    for nt in $MT_NT; do nt_split "$nt"; for l in 1 256; do for v in "${V_NT[@]}"; do
      run "$csv" "$rep" "$c:nt" $v -n "$N" -t "$T" -l "$l" ${CFG[$c]} "${COMMON[@]}"
    done; done; done
  done; done
}

kv() {
  [ -x "$KV" ] || die "$KV not built"
  local csv=$OUT/kv.csv
  [ -s "$csv" ] || echo "rep,config,$("$KV" -H)" >"$csv"
  local RUNBIN=$KV N T c l h w v
  N=${KV_NT%x*}; T=${KV_NT#*x}
  local KC=(-n "$N" -t "$T" -s "$SECS" -W "$WARMUP" -C)
  for rep in $(seq "$REPS"); do for c in $MAIN_CFGS; do
    # map: how much does a per-lock choice gain? (H=0 all Lamport, H=L all tournament)
    for w in A B; do for l in 64 1024; do
      local hs="0 1 4 16 64"; [ "$l" = 64 ] || hs="$hs $l"
      for h in $hs; do
        [ "$h" -le "$l" ] || continue
        run "$csv" "$rep" "$c:map" -w "$w" -l "$l" --algmap "top:$h" -M local ${CFG[$c]} "${KC[@]}"
      done
      run "$csv" "$rep" "$c:map" -w "$w" -l "$l" -a server -M handoff ${CFG[$c]} "${KC[@]}"
    done; done
    # skew x affinity, L=1024, workload A
    for v in "-d uniform" "-z 0.9" "-z 0.99"; do for f in 0 0.9; do
      for h in 0 16 1024; do
        run "$csv" "$rep" "$c:skew" $v -F "$f" -l 1024 --algmap "top:$h" -M local ${CFG[$c]} "${KC[@]}"
      done
      run "$csv" "$rep" "$c:skew" $v -F "$f" -l 1024 -a server -M handoff ${CFG[$c]} "${KC[@]}"
    done; done
    # precise vs full data maintenance (all tournament)
    for l in 64 1024; do for d in "" --full; do
      run "$csv" "$rep" "$c:data" -l "$l" --algmap "top:$l" -M local $d ${CFG[$c]} "${KC[@]}"
    done; done
    # open loop, L=1024, workload A, Zipf 0.99
    for r in $KV_RATES; do
      for h in 0 16 1024; do
        run "$csv" "$rep" "$c:open" -l 1024 --algmap "top:$h" -r "$r" ${CFG[$c]} "${KC[@]}"
      done
      run "$csv" "$rep" "$c:open" -l 1024 -a server -M handoff -r "$r" ${CFG[$c]} "${KC[@]}"
    done
  done; done
}

kvreact() {
  [ -x "$KV" ] || die "$KV not built"
  local csv=$OUT/kvreact.csv
  [ -s "$csv" ] || echo "rep,config,$("$KV" -H)" >"$csv"
  local RUNBIN=$KV N T c l v t r
  N=${KV_NT%x*}; T=${KV_NT#*x}
  local KC=(-n "$N" -t "$T" -s "$SECS" -W "$WARMUP" -C)
  for rep in $(seq "$REPS"); do for c in $MAIN_CFGS; do
    for v in "-z 0.99" "-d uniform" "-z 0.99 -F 0.9"; do for l in 64 1024; do
      for m in "--algmap top:0" "--algmap top:16" "--algmap top:$l"; do
        run "$csv" "$rep" "$c:closed" $v -l "$l" $m -M local ${CFG[$c]} "${KC[@]}"
      done
      for t in $REACT_THR; do
        run "$csv" "$rep" "$c:closed" $v -l "$l" -a reactive --react "$t" -M local ${CFG[$c]} "${KC[@]}"
      done
      run "$csv" "$rep" "$c:closed" $v -l "$l" -a server -M handoff ${CFG[$c]} "${KC[@]}"
    done; done
    for r in 1000k 2000k 4000k 6000k; do
      run "$csv" "$rep" "$c:open" -l 1024 --algmap top:16 -M local -r "$r" ${CFG[$c]} "${KC[@]}"
      for t in $REACT_THR; do
        run "$csv" "$rep" "$c:open" -l 1024 -a reactive --react "$t" -M local -r "$r" ${CFG[$c]} "${KC[@]}"
      done
      run "$csv" "$rep" "$c:open" -l 1024 -a server -M handoff -r "$r" ${CFG[$c]} "${KC[@]}"
    done
  done; done
}

kviso() {
  [ -x "$KV" ] || die "$KV not built"
  local csv=$OUT/kviso.csv
  [ -s "$csv" ] || echo "rep,config,$("$KV" -H)" >"$csv"
  local RUNBIN=$KV N T c r v
  N=${KV_NT%x*}; T=${KV_NT#*x}
  local KC=(-n "$N" -t "$T" -s "$SECS" -W "$WARMUP" -C -M local)
  local PIN="-a reactive --react off"
  for rep in $(seq "$REPS"); do for c in $MAIN_CFGS; do
    for v in "--algmap top:16" "$PIN --algmap top:16" "-a reactive --react 12:4:64" \
             "--algmap top:1024" "$PIN --algmap top:1024"; do
      run "$csv" "$rep" "$c:closed" -z 0.99 -l 1024 $v ${CFG[$c]} "${KC[@]}"
    done
    for v in "--algmap top:16" "$PIN --algmap top:16" "-a reactive --react 12:4:64"; do
      run "$csv" "$rep" "$c:closed" -z 0.99 -l 64 $v ${CFG[$c]} "${KC[@]}"
    done
    for v in "--algmap top:0" "$PIN --algmap top:0" "--algmap top:1024" "$PIN --algmap top:1024"; do
      run "$csv" "$rep" "$c:closed" -d uniform -l 1024 $v ${CFG[$c]} "${KC[@]}"
    done
    for r in 1000k 2000k 4000k 6000k; do
      for v in "--algmap top:16" "$PIN --algmap top:16" "-a reactive --react 12:4:64"; do
        run "$csv" "$rep" "$c:open" -z 0.99 -l 1024 $v -r "$r" ${CFG[$c]} "${KC[@]}"
      done
    done
  done; done
}

kstride() {
  [ -x "$KV" ] || die "$KV not built"
  local csv=$OUT/kstride.csv
  [ -s "$csv" ] || echo "rep,config,$("$KV" -H)" >"$csv"
  local RUNBIN=$KV N T p
  N=${KV_NT%x*}; T=${KV_NT#*x}
  local KC=(-n "$N" -t "$T" -s "$SECS" -W "$WARMUP" -C -M local -d uniform -l 1024 ${CFG[cf_clwb]})
  for rep in $(seq "$REPS"); do
    for p in 0 1 2 3 4 5 6 11 19; do   # 45..51, 56, 64 lines
      run "$csv" "$rep" fixed --algmap top:0 --pad-lines "$p" "${KC[@]}"
    done
    for p in 0 1 2 3 16; do            # 48..51, 64 lines
      run "$csv" "$rep" reactive -a reactive --react off --algmap top:0 --react-layout xy-first \
        --pad-lines "$p" "${KC[@]}"
    done
    for p in 0 1 6 14 15 30 46 47 62; do # 2, 3, 8, 16, 17, 32, 48, 49, 64 lines
      run "$csv" "$rep" lamport -a lamport --pad-lines "$p" "${KC[@]}"
    done
  done
}

klayout() {
  [ -x "$KV" ] || die "$KV not built"
  local csv=$OUT/klayout.csv
  [ -s "$csv" ] || echo "rep,config,$("$KV" -H)" >"$csv"
  local RUNBIN=$KV N T v
  N=${KV_NT%x*}; T=${KV_NT#*x}
  local KC=(-n "$N" -t "$T" -s "$SECS" -W "$WARMUP" -C -M local ${CFG[cf_clwb]})
  local PIN="-a reactive --react off --algmap top:0"
  for rep in $(seq "$REPS"); do
    for v in "--algmap top:0" "--algmap top:0 --pad-lines 1" "--algmap top:0 --pad-lines 3" "-a lamport" \
             "$PIN --react-layout mode-first" "$PIN --react-layout xy-first"; do
      run "$csv" "$rep" uniform -d uniform -l 1024 $v "${KC[@]}"
    done
    for v in "--algmap top:16" "-a reactive --react 12:4:64 --react-layout mode-first" \
             "-a reactive --react 12:4:64 --react-layout xy-first"; do
      run "$csv" "$rep" zipf -z 0.99 -l 1024 $v "${KC[@]}"
    done
    for v in "--algmap top:16" "-a reactive --react 12:4:64 --react-layout xy-first"; do
      run "$csv" "$rep" open6M -z 0.99 -l 1024 -r 6000k $v "${KC[@]}"
    done
  done
}

kvbase() {
  [ -x "$KV" ] || die "$KV not built"
  local csv=$OUT/kvbase.csv
  [ -s "$csv" ] || echo "rep,config,$("$KV" -H)" >"$csv"
  local RUNBIN=$KV N T r v k
  N=${KV_NT%x*}; T=${KV_NT#*x}
  local KC=(-n "$N" -t "$T" -s "$SECS" -W "$WARMUP" -C)
  local COH="-b coherent --mem-node $MEMNODE"
  local CF="${CFG[cf_clwb]}"
  local -a V=("$COH -a pthread -m" "$COH -a pthread" "$COH -a tournament -M local"
              "$COH -a reactive --react 12:4:64 -M local"
              "$CF -a reactive --react 12:4:64 -M local" "$CF --algmap top:16 -M local")
  for rep in $(seq "$REPS"); do
    for k in "-z 0.99 -l 1024" "-z 0.99 -l 64" "-d uniform -l 1024"; do for v in "${V[@]}"; do
      run "$csv" "$rep" closed $k $v "${KC[@]}"
    done; done
    for r in $KVBASE_RATES; do for v in "${V[@]}"; do
      run "$csv" "$rep" open -z 0.99 -l 1024 -r "$r" $v "${KC[@]}"
    done; done
  done
}

conc() {
  # Device concurrency: does the aggregate of uncached 8-byte accesses flatten
  # with T while full-line writes scale (partial-write hypothesis)?
  [ -x "$MEMCONC" ] || die "$MEMCONC not built"
  local csv=$OUT/conc.csv
  [ -s "$csv" ] || echo "rep,config,$("$MEMCONC" -H)" >"$csv"
  local ts; ts=$(echo $CONC_T | tr ' ' ',')
  local -A CC=(
    [uc]="-b uc --wb $WB --mem-node $MEMNODE" # metadata: kmod pool; data region bound too
    [cf_cxl]="-b cached-flush --wb $WB --mem-node $MEMNODE"
    [cf_dram]="-b cached-flush --wb $WB --mem-node $CPUNODE"
  )
  for rep in $(seq "$REPS"); do for c in uc cf_cxl cf_dram; do
    local o e rc; e=$(mktemp)
    o=$(timeout 1800 "${NUMA[@]}" "$MEMCONC" ${CC[$c]} -T "$ts" -s "$CONC_SECS" -W 0.2 -C 2>"$e"); rc=$?
    if [ $rc -ne 0 ] || [ -s "$e" ]; then
      { echo "conc $c rep $rep rc=$rc:"; cat "$e"; } >>"$ERR"
    fi
    rm -f "$e"
    [ -n "$o" ] && echo "$o" | sed "s/^/$rep,$c,/" >>"$csv"
  done; done
}

open2b() {
  local csv=$OUT/open2b.csv; header "$csv"
  for rep in $(seq "$REPS"); do for c in $MAIN_CFGS; do
    for l in 1 256; do
      local rates=$RATES1; [ "$l" = 256 ] && rates=$RATES256
      for r in $rates; do
        for v in "-a tournament" "-a lamport" "-a server -M handoff" \
                 "-a tournament -M handoff" "-a tournament -M handoff -X scan" \
                 "-a tournament -M handoff --ho none" \
                 "-a lamport -M handoff" "-a lamport -M handoff -X scan" \
                 "-a hybrid -M handoff -X scan"; do
          run "$csv" "$rep" "$c" $v -n "$OPEN2B_N" -t 1 -l "$l" -r "$r" -A poisson ${CFG[$c]} "${COMMON[@]}"
        done
      done
    done
    # Closed loop, L=256 (mostly uncontended): what -X scan costs vs the probe.
    for nt in ${OPEN2B_CLOSED_NT:-16x1 32x1}; do nt_split "$nt"
      for a in tournament lamport; do for x in adaptive scan; do
        run "$csv" "$rep" "$c" -a "$a" -M handoff -X "$x" -n "$N" -t "$T" -l 256 ${CFG[$c]} "${COMMON[@]}"
      done; done
    done
  done; done
}

PHASES=${*:-"preflight p1 2a 2b base open"}
for ph in $PHASES; do
  echo "== $ph ($(date +%T))"
  case $ph in
    preflight) preflight ;;
    p1) p1 ;;
    abl) abl ;;
    2a) p2a ;;
    2b) p2b ;;
    own) p2b_own ;;
    ho) p2b_ho ;;
    locks) many_locks ;;
    base) base ;;
    open) open_loop ;;
    dram) dram ;;
    conc) conc ;;
    open2b) open2b ;;
    mt) mt ;;
    kv) kv ;;
    kvreact) kvreact ;;
    kviso) kviso ;;
    kvbase) kvbase ;;
    klayout) klayout ;;
    kstride) kstride ;;
    *) die "unknown phase $ph" ;;
  esac
done
echo "done: $OUT"
