#!/usr/bin/env bash
# Open-loop latency-vs-offered-load sweep; one CSV row per (alg, rate).
# Usage: scripts/sweep_load.sh [extra ncl_bench args...] > out.csv
#   env: ALGS="lamport tournament hybrid" RATES="100k 300k ..." BENCH=build/ncl_bench
# Runs are sequential (the uc device allows one experiment at a time).
set -euo pipefail
BENCH=${BENCH:-$(dirname "$0")/../build/ncl_bench}
ALGS=${ALGS:-"lamport tournament hybrid"}
RATES=${RATES:-"100k 250k 500k 750k 1M 1.25M 1.5M 1.75M 2M"}
"$BENCH" -H
for a in $ALGS; do
  for r in $RATES; do
    "$BENCH" -a "$a" -r "$r" -C "$@"
  done
done
