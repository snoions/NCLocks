# Running on a NUMA host with a CXL memory node

Target: 2 sockets with 60 cores each (SMT2; CPUs 0-59 and 120-179 on node 0,
60-119 and 180-239 on node 1) and a CXL 2.0 memory expander exposed as CPU-less
NUMA node 2 (16 GB, ZONE_NORMAL). The firmware distances are 0-2: 14 and 1-2: 24,
so the device hangs off socket 0.

What this host adds over the single-socket dev box is real device latency and
bandwidth for the shared regions, more nodes, and socket topology. It does NOT add
non-coherence: CXL 2.0 memory attached to a single host is still hardware-coherent.
Non-coherence is still emulated by the uc mapping and the flush discipline, and its
correctness is still checked by `emu`.

## Placement rules

- Keep emulated nodes on **socket 0**. In a real multi-host setup every host has a
  direct CXL link. Socket 1 reaches the device through socket 0, which adds an
  asymmetry no real host would see. Use socket 1 only for deliberate "far node"
  experiments.
- Run everything under `numactl --cpunodebind=0 --membind=0`. Node-private memory
  (stacks, submutexes, emu private views) then stays in socket 0's DRAM, and the
  default CPU list becomes socket 0's CPUs (0-59, then the SMT siblings 120-179).
  `--mem-node 2` binds only the shared regions to the CXL node: a mapping's own
  policy overrides the process policy.
- Pinned slot k = node*T + idx runs on the k-th CPU of the list. Servers with
  `-Y next` take slots N*T+n, and with `-Y sibling` they take the SMT sibling of
  their node's first worker (CPU + 120 here). Dedicated cores are plentiful, so
  prefer `-Y next` while N*T + N <= 60. The run warns if pinned threads share CPUs or
  span NUMA nodes.

## Steps

1. Inspect the host:
   ```
   numactl -H; uname -r; lscpu | grep -iE 'model name|socket|numa'
   grep -o 'clwb\|clflushopt' /proc/cpuinfo | sort -u
   dmesg | grep -iE 'x86/PAT|cxl' | head
   ```
   The kmod builds for kernels 5.12-6.9 only (`follow_pte`). The uc backend needs
   PAT.
2. Build: `cmake -S . -B build && cmake --build build -j`, then `cd kmod && make`.
3. Load the driver with its pool on the CXL node:
   ```
   sudo insmod kmod/ncl_uc.ko nid=2 size_mb=256
   sudo chown $USER /dev/ncl_uc
   dmesg | tail -1          # "... memtype=uc, node=2 ..."
   ```
   For uc-on-DRAM comparison runs, reload with `nid=0`.
4. Correctness first: `cd build && ctest -j8`, then emu at the new scale:
   ```
   numactl --cpunodebind=0 --membind=0 ./ncl_stress -a hybrid -b emu -n 16 -t 3 -i 200000 --mem-node 2 --timeout 600
   numactl --cpunodebind=0 --membind=0 ./ncl_stress -a tournament -b emu -n 16 -t 2 -i 100000 --mem-node 2 --skip-wb   # must FAIL
   numactl --cpunodebind=0 --membind=0 ./ncl_stress -a server -b uc -n 16 -t 2 -p -i 100000 --mem-node 2
   ```
5. Latency sanity check. `ncl_memlat` follows `NCL_MEM_NODE` for its memfd regions, and
   uses the driver's pool for uc metadata:
   ```
   NCL_MEM_NODE=2 numactl --cpunodebind=0 --membind=0 ./ncl_memlat uc 7
   ```
   Every startup line shows `mem_node=`, `uc_pool_node=`, `meta_load=` and `wb=`. CSV rows
   carry `mem_node,uc_pool_node,wb,cpus`.
6. Experiments (3 reps, report throughput together with fairness):
   - Phase 1, 2a and 2b, every algorithm, `-b uc --mem-node 2`, N in {4, 8, 16, 32},
     T in {1, 3}.
   - Baselines on the same host:
     - uc metadata on DRAM (driver `nid=0`, `--mem-node 0`): the CXL latency share;
     - `-b coherent --mem-node 2`: the non-coherence share on the same device;
     - `-a pthread -b coherent` on node 2 and on node 0.
   - `--wb clwb` vs `clflushopt` for the data writeback (footprint sweep).
   - Open-loop sweeps at N=16 (`scripts/sweep_load.sh`).
   - Optional: one far node (`--cpus` listing a socket-1 CPU for the last node).
