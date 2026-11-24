#!/usr/bin/env bash
# Launch one ncl_stress / ncl_bench / ncl_kv experiment: one process per node.
#
# usage: scripts/ncl_run.sh [PREFIX...] BINARY [OPTIONS...]
#   e.g. scripts/ncl_run.sh build/ncl_bench -a bakery -n 8 -t 2 -C
#        scripts/ncl_run.sh numactl --cpunodebind=0 build/ncl_kv -n 16 -t 3 ...
# The whole command line is run once per role with a role appended
# (bench/harness.h):
#   CMD --query                ask for the plan: "nodes=N timeout=S uc=0|1"
#   CMD --shm FILE --init      create and set up the shared region in FILE
#   CMD --shm FILE --node I    node I, for I = 0..N-1, all concurrently
# Node 0 prints the report. Exit status: the query's or the creator's if they
# fail (77 = skipped, e.g. no uc driver), else the first failing node's, else 0.
# A node failing kills the others (they would wait for it forever). timeout=S > 0
# (ncl_stress --timeout) kills the nodes after S seconds and fails.
#
# The processes share nothing but FILE (and, for -b uc, the driver): on one host
# they stand in for nodes on separate hosts sharing a CXL device. To run nodes on
# several hosts, start each node's command on its host instead of locally, with
# FILE naming the shared device there.
#
# env: NCL_SHM_DIR (/dev/shm) directory for FILE; NCL_UC_DEV (/dev/ncl_uc).
#      For -b uc the launcher opens the driver once and passes it to every process
#      (NCL_UC_FD): the driver admits one open file at a time, which keeps
#      experiments from overlapping.
set -uo pipefail

[ $# -ge 1 ] || { sed -n '2,8p' "$0" >&2; exit 2; }

plan=$("$@" --query) || exit $?
nodes= timeout_s=0 uc=0
for kv in $plan; do
  case $kv in
    nodes=*) nodes=${kv#nodes=} ;;
    timeout=*) timeout_s=${kv#timeout=} ;;
    uc=*) uc=${kv#uc=} ;;
  esac
done
[[ $nodes =~ ^[1-9][0-9]*$ ]] || { echo "ncl_run: bad plan from --query: $plan" >&2; exit 2; }

shm=$(mktemp "${NCL_SHM_DIR:-/dev/shm}/ncl.XXXXXX") || exit 2
pids=()
cleanup() {
  [ ${#pids[@]} -gt 0 ] && kill -KILL "${pids[@]}" 2>/dev/null
  wait 2>/dev/null
  rm -f "$shm"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT

if [ "$uc" = 1 ]; then
  dev=${NCL_UC_DEV:-/dev/ncl_uc}
  exec {ucfd}<>"$dev" || { echo "ncl_run: cannot open $dev (another experiment holds it?)" >&2; exit 2; }
  export NCL_UC_FD=$ucfd
fi

"$@" --shm "$shm" --init || exit $?

for ((i = 0; i < nodes; i++)); do
  "$@" --shm "$shm" --node "$i" &
  pids+=($!)
done

# Reap the nodes as they finish; on a failure (or the timeout) kill the rest.
rc=0 left=$nodes start=$SECONDS
declare -A done_pid=()
while [ "$left" -gt 0 ]; do
  for p in "${pids[@]}"; do
    [ -n "${done_pid[$p]+x}" ] && continue
    kill -0 "$p" 2>/dev/null && continue
    wait "$p"; s=$?
    done_pid[$p]=1
    left=$((left - 1))
    if [ "$s" -ne 0 ] && [ "$rc" -eq 0 ]; then
      rc=$s
      if [ "$left" -gt 0 ]; then
        echo "ncl_run: node process $p exited with status $s; stopping the others" >&2
        kill -KILL "${pids[@]}" 2>/dev/null
      fi
    fi
  done
  [ "$left" -gt 0 ] || break
  if [ "$timeout_s" -gt 0 ] && [ $((SECONDS - start)) -ge "$timeout_s" ] && [ "$rc" -eq 0 ]; then
    echo "ncl_run: timeout after $timeout_s s: killing nodes -> FAIL" >&2
    echo "total: TIMEOUT -> FAIL"
    kill -KILL "${pids[@]}" 2>/dev/null
    rc=1
  fi
  sleep 0.01
done
exit "$rc"
