#!/usr/bin/env bash
# Evaluation sweep for a NUMA host with a CXL memory node (docs/cxl_host.md).
#
# Usage: scripts/cxl_sweep.sh [PHASE...]        (default: preflight p1 abl 2a 2b base open)
#   preflight  check placement (CPUs, mem node, uc pool node) and stop on mismatch
#   p1         Phase 1, closed loop, every algorithm, main configs, all (N,T)
#   abl        Phase 1 ablation: uc uniform + clflushopt (the dev-box default), N=8,16
#   2a         Phase 2a local handoff (-M local), T=3
#   2b         Phase 2b cross-node handoff (-M handoff, defaults --wait own, --ho all), gate algorithms + server chain
#   base       coherent baselines: tournament and pthread (+ no-submutex) on CXL and DRAM
#   open       open loop (Poisson) at N=16, T=1, offered load scaled to CXL capacity
#   locks      Phase 1 with L locks chosen uniformly per acquisition (-l), all algorithms, N=16,32
#   ho         2b shorter handoff path (--ho none | grant,clear | all = grant,clear,skip), gate algorithms,
#              with Phase 1 tournament and the server chain as references, L=1 and 256
#   own        2b waiters: shared owner word vs own announce line (--wait), gate algorithms
#   dram       main uc configs with metadata on DRAM: RELOAD THE DRIVER WITH nid=0 FIRST
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
#      MEMNODE (2: CXL), CPUNODE (0: the socket the device hangs off),
#      NT ("4x1 8x1 16x1 32x1 4x3 8x3 16x3"), OWN_NT ("4x1 8x1 16x1 32x1 16x3"),
#      CFGS ("cf_clwb"; add uc_split_clwb for uc),
#      LOCKS ("1 4 16 64 256"), LOCKS_NT ("16x1 32x1"),
#      HO_NT ("4x1 16x1 32x1 16x3"), HO_LOCKS ("1 256"), HO_OPTS ("none grant,clear all"), RUN_TIMEOUT (120 s per run), WB (clwb),
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
NT=${NT:-"4x1 8x1 16x1 32x1 4x3 8x3 16x3"}
OWN_NT=${OWN_NT:-"4x1 8x1 16x1 32x1 16x3"} # phase own
LOCKS=${LOCKS:-"1 4 16 64 256"}             # phase locks
LOCKS_NT=${LOCKS_NT:-"16x1 32x1"}
HO_NT=${HO_NT:-"4x1 16x1 32x1 16x3"}         # phase ho
HO_LOCKS=${HO_LOCKS:-"1 256"}
HO_OPTS=${HO_OPTS:-"none grant,clear all"}
RUN_TIMEOUT=${RUN_TIMEOUT:-120}
CONC_T=${CONC_T:-"1 2 4 8 16 24 32 48 60"} # phase conc
CONC_SECS=${CONC_SECS:-1}
MEMCONC=${MEMCONC:-$ROOT/build/ncl_memconc}
RATES1=${RATES1:-"25k 50k 100k 200k 300k 400k 450k"}             # phase open2b, L=1
RATES256=${RATES256:-"250k 500k 1000k 1500k 2000k 2500k 3000k"} # phase open2b, L=256
OPEN2B_N=${OPEN2B_N:-16}
WB=${WB:-clwb} # writeback instruction of the main configs (clflushopt for hosts without clwb)

NUMA=(numactl --cpunodebind="$CPUNODE" --membind="$CPUNODE")
COMMON=(-s "$SECS" -W "$WARMUP" -c "$CS" -B "$BACKOFF" -C)
ALL_ALGS="tournament hybrid ak server fastsrv aksrv lamport"
GATE_ALGS="lamport tournament hybrid ak server" # -M handoff (server = chain)

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
  o=$(timeout "$RUN_TIMEOUT" "${NUMA[@]}" "$BENCH" "$@" 2>"$e")
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
    for a in tournament hybrid ak server fastsrv aksrv; do for c in $MAIN_CFGS; do for r in $rates; do
      run "$csv" "$rep" "$c" -a "$a" -n 16 -t 1 -r "$r" -A poisson ${CFG[$c]} "${COMMON[@]}"
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

conc() {
  # Device concurrency: does the aggregate of uncached 8-byte accesses flatten
  # with T while full-line writes scale (partial-write hypothesis)?
  [ -x "$MEMCONC" ] || die "$MEMCONC not built"
  local csv=$OUT/conc.csv
  [ -s "$csv" ] || echo "rep,config,$("$MEMCONC" -H)" >"$csv"
  local ts; ts=$(echo $CONC_T | tr ' ' ',')
  local -A CC=(
    [uc]="-b uc --wb $WB"
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

PHASES=${*:-"preflight p1 abl 2a 2b base open"}
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
    *) die "unknown phase $ph" ;;
  esac
done
echo "done: $OUT"
