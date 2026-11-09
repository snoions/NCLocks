#!/usr/bin/env bash
# Open-loop sweep; one CSV row per (alg, arrival, rate, rep).
# Usage: scripts/sweep_load.sh [extra ncl_bench args...] > out.csv
#   env: ALGS="lamport tournament hybrid"
#        RATES="100k 250k ..."         offered loads (aggregate req/s)
#        ARRIVALS="poisson"            e.g. "poisson onoff:1000:0.25"
#        REPS=1                        repetitions per point (interleaved)
#        BENCH=build/ncl_bench
# Runs are sequential (the uc device allows one experiment at a time).
# Repetitions are interleaved across configurations so slow drift (thermal,
# background load) spreads over all points instead of biasing one.
set -euo pipefail
BENCH=${BENCH:-$(dirname "$0")/../build/ncl_bench}
ALGS=${ALGS:-"lamport tournament hybrid"}
RATES=${RATES:-"100k 250k 500k 750k 1M 1.25M 1.5M 1.75M 2M"}
ARRIVALS=${ARRIVALS:-poisson}
REPS=${REPS:-1}
echo "rep,$("$BENCH" -H)"
for rep in $(seq 1 "$REPS"); do
  for a in $ALGS; do
    for arr in $ARRIVALS; do
      for r in $RATES; do
        echo "$rep,$("$BENCH" -a "$a" -A "$arr" -r "$r" -C "$@")"
      done
    done
  done
done
