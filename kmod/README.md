# ncl_uc: uncached memory for the `uc` backend

User space cannot get an uncached mapping of RAM on its own. This module
allocates a pool of RAM pages and maps it into user space through `/dev/ncl_uc`
with x86 PAT memory type UC-. On RAM whose MTRR type is WB, UC- behaves as UC.

## Build and load

```
cd kmod && make
sudo insmod ncl_uc.ko                  # size_mb=64 memtype=uc dev_mode=0600
sudo chown $USER /dev/ncl_uc           # or: insmod ... dev_mode=0666
dmesg | tail -1                        # "ncl_uc: 64 MiB pool, memtype=uc ..."
sudo rmmod ncl_uc                      # when done (restores WB, frees pages)
```

`nid=N` allocates the pool on NUMA node N (e.g. a CXL memory node) with
`__GFP_THISNODE`, so there is no fallback to another node, and the driver checks
`page_to_nid()` for every page. The node is reported in `dmesg` and through
`NCL_UC_IOC_INFO`. The library prints it as `uc_pool_node=`, and `--mem-node N` refuses
a pool that is on any other node. A node onlined only as `ZONE_MOVABLE` cannot satisfy
`GFP_KERNEL`, and the load then fails with -ENOMEM. The library still accepts a driver
built before `nid` existed and reports its pool node as -1.

Use `memtype=wc` or `memtype=wb` only for deliberate comparison runs. The
library refuses them unless `NCL_UC_ACCEPT_ANY_MEMTYPE=1` is set.

Only one process tree can have the device open at a time, so one experiment
runs at a time. Nodes are forked children and inherit the open device and its
mapping.

## Why the driver converts the pages first

On x86 with PAT, setting `pgprot_noncached()` on a `remap_pfn_range()`
mapping of RAM is **not enough**. `track_pfn_remap()` and `reserve_pfn_range()`
replace the requested cache mode with the page's tracked memtype, which is WB
for ordinary RAM, and the result is a cached mapping. The driver therefore
calls `set_pages_array_uc()` at load time. That records UC- as the pages'
memtype and also changes the kernel's own mapping of those pages, so there is
no conflicting WB alias.

## How the library verifies it (`src/mem.c`, `map_uc_meta`)

1. `NCL_UC_IOC_INFO`: the pool memtype and whether PAT is enabled.
2. `NCL_UC_IOC_PROBE` on **every** metadata page decodes the cache bits of the
   live PTE in this process. Anything other than UC or UC- aborts the run.
3. The mean latency of plain loads over 64 KiB of metadata must be at least
   20 ns. Cache hits cost a few ns and UC loads go to DRAM, so this catches a
   mapping that the PTE reports as UC but that is still being cached, for
   example because of an MTRR override.

The result is printed at startup, for example:
`mem: backend=uc meta_memtype=UC- meta_load=...ns`.

## Limits

- Written for kernels 5.12 to 6.9. Newer kernels changed the `follow_pte()`
  API, and the module fails to compile there with a clear `#error`.
- Without `nid=` the pool is ordinary DRAM, usually on the loading CPU's node. UC removes caching but not the coherent
  interconnect. Device latency and CXL behavior are still approximated, using
  `-L` latency injection if needed.
- Do not use locked or atomic read-modify-write instructions on this mapping:
  they cause bus locks. The lock algorithms use only plain 8-byte loads and
  stores on metadata.
