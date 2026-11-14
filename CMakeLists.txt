cmake_minimum_required(VERSION 3.16)
project(ncl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(NCL_SANITIZE "Build with -fsanitize=address,undefined" OFF)
set(NCL_MAX_NODES 64 CACHE STRING "Max nodes")
set(NCL_MAX_PARTICIPANTS 256 CACHE STRING "Max global-algorithm participants")

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-mclflushopt)
endif()
if(NCL_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_library(ncl STATIC src/mem.c src/base.c src/lock.c src/server.c src/cpus.c)
target_include_directories(ncl PUBLIC include)
target_compile_definitions(ncl PUBLIC
  NCL_MAX_NODES=${NCL_MAX_NODES} NCL_MAX_PARTICIPANTS=${NCL_MAX_PARTICIPANTS})
target_link_libraries(ncl PUBLIC Threads::Threads)

add_executable(ncl_stress tests/stress.c)
target_link_libraries(ncl_stress ncl)

add_executable(ncl_bench bench/microbench.c)
target_link_libraries(ncl_bench ncl m)
add_executable(ncl_memlat bench/memlat.c)
target_link_libraries(ncl_memlat ncl)

enable_testing()
foreach(alg lamport tournament hybrid server fastsrv ak)
  foreach(be emu cached-flush coherent uc)
    add_test(NAME stress_${alg}_${be}
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 2 -l 3 -i 20000)
    # uc needs kmod/ncl_uc.ko loaded; skipped (exit 77) otherwise.
    set_tests_properties(stress_${alg}_${be} PROPERTIES SKIP_RETURN_CODE 77)
    if(be STREQUAL "uc")
      # /dev/ncl_uc allows one opener at a time.
      set_tests_properties(stress_${alg}_${be} PROPERTIES RESOURCE_LOCK ncl_uc_device)
    endif()
  endforeach()
  if(NOT alg MATCHES "server|fastsrv")  # (c)/(e) require the submutex
    add_test(NAME stress_${alg}_emu_nosub
      COMMAND ncl_stress -a ${alg} -b emu -n 3 -t 3 -l 2 -i 10000 -m)
  endif()
  # Batched multi-line reads (cached-flush only; default -Q batch): N=8 with the
  # scan-heavy options. Exercises the batch index logic on a coherent host;
  # it cannot expose missing flushes or fences (see include/ncl/mem.h).
  add_test(NAME stress_${alg}_cached-flush_batch_n8
    COMMAND ncl_stress -a ${alg} -b cached-flush -n 8 -t 2 -l 2 -i 10000)
  add_test(NAME stress_${alg}_emu_precise
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 2 -l 3 -i 20000 -H)
endforeach()
# Phase 2a local handoff: precise-hint accumulation (-K 4 -H), overflow
# fallback (-H), and cap-forced releases (-K 1).
foreach(alg lamport tournament hybrid server fastsrv ak)
  add_test(NAME stress_${alg}_emu_local
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -H)
  add_test(NAME stress_${alg}_emu_local_cap4
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -K 4 -H)
  add_test(NAME stress_${alg}_emu_local_cap1
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -K 1)
endforeach()
# Phase 2b: gate + owner handoff (design §3.3). Default = announce before the
# gate + adaptive pick_waiter; also the spec baseline (inside/scan), lazy
# announce, and always-probe. -K 1 makes almost every release a cross-node handoff.
foreach(alg lamport tournament hybrid ak)
  foreach(be emu uc)
    add_test(NAME stress_${alg}_${be}_handoff
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 3 -l 2 -i 10000 -M handoff -H)
    add_test(NAME stress_${alg}_${be}_handoff_lazy_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -G lazy -K 1)
    add_test(NAME stress_${alg}_${be}_handoff_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -K 1)
    add_test(NAME stress_${alg}_${be}_handoff_inside_scan
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -G inside -X scan)
    add_test(NAME stress_${alg}_${be}_handoff_probe_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -X probe -K 1)
    # Default wait = own line (docs/handoff.md); also the shared-owner poll.
    add_test(NAME stress_${alg}_${be}_handoff_owner_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff --wait owner -K 1 --timeout 100)
    add_test(NAME stress_${alg}_${be}_handoff_owner_lazy
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 3 -l 2 -i 10000 -M handoff --wait owner -G lazy -H --timeout 100)
    if(be STREQUAL "uc")
      set_tests_properties(stress_${alg}_uc_handoff stress_${alg}_uc_handoff_lazy_cap1
        stress_${alg}_uc_handoff_cap1 stress_${alg}_uc_handoff_inside_scan
        stress_${alg}_uc_handoff_probe_cap1 stress_${alg}_uc_handoff_owner_cap1
        stress_${alg}_uc_handoff_owner_lazy
        PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
    endif()
  endforeach()
endforeach()
add_test(NAME emu_detects_missing_writeback_handoff
  COMMAND ncl_stress -a tournament -b emu -n 4 -t 3 -l 1 -i 5000 -M handoff -K 1 --skip-wb)
set_tests_properties(emu_detects_missing_writeback_handoff PROPERTIES WILL_FAIL TRUE)
add_test(NAME emu_detects_missing_invalidate_handoff
  COMMAND ncl_stress -a hybrid -b emu -n 4 -t 3 -l 1 -i 5000 -M handoff -K 1 --skip-inv)
set_tests_properties(emu_detects_missing_invalidate_handoff PROPERTIES WILL_FAIL TRUE)
add_test(NAME stress_hybrid_cached-flush_probe_n8
  COMMAND ncl_stress -a hybrid -b cached-flush -n 8 -t 2 -l 2 -i 10000 -Q probe)
add_test(NAME stress_lamport_cached-flush_probe_n8
  COMMAND ncl_stress -a lamport -b cached-flush -n 8 -t 2 -l 2 -i 10000 -Q probe)
add_test(NAME stress_tournament_cached-flush_probe_handoff_scan
  COMMAND ncl_stress -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff -X scan -K 1 -Q probe)
add_test(NAME stress_tournament_cached-flush_batch_handoff_scan
  COMMAND ncl_stress -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff -X scan -K 1)
add_test(NAME stress_tournament_cached-flush_handoff_own_n8
  COMMAND ncl_stress -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff --wait own -K 1 --timeout 100)
add_test(NAME stress_tournament_cached-flush_handoff_hoall_n8
  COMMAND ncl_stress -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff --ho all -K 1 --timeout 100)
foreach(alg lamport hybrid)
  # Uniform layout: announce lines are UC (uc) or emulated-uncached (emu).
  foreach(be emu uc)
    add_test(NAME stress_${alg}_${be}_handoff_hoall_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff --ho all -K 1 --timeout 100)
  endforeach()
  set_tests_properties(stress_${alg}_uc_handoff_hoall_cap1 PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
endforeach()
add_test(NAME ho_rejects_grant_owner_wait
  COMMAND ncl_stress -a tournament -b emu -M handoff --wait owner --ho grant)
add_test(NAME ho_rejects_phase1
  COMMAND ncl_stress -a tournament -b emu --ho all)
add_test(NAME ho_rejects_server
  COMMAND ncl_stress -a server -b emu -M handoff --ho clear)
add_test(NAME ho_rejects_skip_lazy
  COMMAND ncl_stress -a tournament -b emu -M handoff -G lazy --ho skip)
set_tests_properties(ho_rejects_grant_owner_wait ho_rejects_phase1 ho_rejects_server ho_rejects_skip_lazy
  PROPERTIES WILL_FAIL TRUE)
add_test(NAME wait_rejects_server
  COMMAND ncl_stress -a server -b emu -M handoff --wait own)
add_test(NAME wait_rejects_phase1
  COMMAND ncl_stress -a tournament -b emu --wait owner)
set_tests_properties(wait_rejects_server wait_rejects_phase1 PROPERTIES WILL_FAIL TRUE)
add_test(NAME handoff_rejects_fastsrv
  COMMAND ncl_stress -a fastsrv -b emu -M handoff)
set_tests_properties(handoff_rejects_fastsrv PROPERTIES WILL_FAIL TRUE)
# (c) in handoff mode: holder-to-waiter chain via the server's succ[] ring
# (docs/handoff.md). _wrap runs > 2^14 grants on one lock with a window that
# keeps ending at the same ring slot (the stale-tag wrap bug hung this);
# _window2 uses a window smaller than N (frequent server resync).
foreach(be emu uc)
  add_test(NAME stress_server_${be}_handoff
    COMMAND ncl_stress -a server -b ${be} -n 4 -t 3 -l 2 -i 10000 -M handoff -H)
  add_test(NAME stress_server_${be}_handoff_cap1
    COMMAND ncl_stress -a server -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -K 1)
  add_test(NAME stress_server_${be}_handoff_scan
    COMMAND ncl_stress -a server -b ${be} -n 4 -t 2 -l 3 -i 10000 -M handoff -X scan -K 1)
  add_test(NAME stress_server_${be}_handoff_window2
    COMMAND ncl_stress -a server -b ${be} -n 6 -t 2 -l 2 -i 10000 -M handoff -K 1 -R 2)
  add_test(NAME stress_server_${be}_handoff_wrap
    COMMAND ncl_stress -a server -b ${be} -n 6 -t 1 -l 1 -i 20000 -M handoff)
  set_tests_properties(stress_server_${be}_handoff_wrap PROPERTIES TIMEOUT 120)
  if(be STREQUAL "uc")
    set_tests_properties(stress_server_uc_handoff stress_server_uc_handoff_cap1
      stress_server_uc_handoff_scan stress_server_uc_handoff_wrap stress_server_uc_handoff_window2
      PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
  endif()
endforeach()
add_test(NAME emu_detects_missing_writeback_server_handoff
  COMMAND ncl_stress -a server -b emu -n 4 -t 3 -l 1 -i 5000 -M handoff -K 1 --skip-wb)
set_tests_properties(emu_detects_missing_writeback_server_handoff PROPERTIES WILL_FAIL TRUE)
add_test(NAME emu_detects_missing_writeback_local
  COMMAND ncl_stress -a tournament -b emu -n 4 -t 3 -l 1 -i 5000 -M local --skip-wb)
set_tests_properties(emu_detects_missing_writeback_local PROPERTIES WILL_FAIL TRUE)
# The emulator must catch missing cache maintenance.
add_test(NAME emu_detects_missing_writeback
  COMMAND ncl_stress -a lamport -b emu -n 4 -t 1 -l 1 -i 5000 --skip-wb)
set_tests_properties(emu_detects_missing_writeback PROPERTIES WILL_FAIL TRUE)
add_test(NAME emu_detects_missing_invalidate
  COMMAND ncl_stress -a lamport -b emu -n 4 -t 1 -l 1 -i 5000 --skip-inv)
set_tests_properties(emu_detects_missing_invalidate PROPERTIES WILL_FAIL TRUE)
# Split metadata layout (-U split, docs/backends.md "Per-region memory types"):
# scanned arrays in a cached region, single words uncached. On emu the scan
# region has per-node private views, so these runs check the cached-metadata
# accesses (batched/probe scans included); on uc they run the real mix.
foreach(be emu uc)
  foreach(alg lamport tournament hybrid server fastsrv ak)
    add_test(NAME stress_${alg}_${be}_split
      COMMAND ncl_stress -a ${alg} -b ${be} -U split -n 4 -t 2 -l 3 -i 20000 --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split)
  endforeach()
  foreach(alg lamport tournament hybrid server)
    add_test(NAME stress_${alg}_${be}_split_handoff_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff -K 1 -X scan --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split_handoff_cap1)
  endforeach()
  # Split layout: the default own-line wait makes the grant a cached store into
  # the waiter's announce line (two writers, causally ordered).
  foreach(alg lamport tournament hybrid ak)
    add_test(NAME stress_${alg}_${be}_split_handoff_own_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --wait own -K 1 --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split_handoff_own_cap1)
  endforeach()
  add_test(NAME stress_tournament_${be}_split_handoff_own_n8_probe
    COMMAND ncl_stress -a tournament -b ${be} -U split -n 8 -t 1 -l 1 -i 20000 -M handoff --wait own -X probe --timeout 100)
  list(APPEND split_tests_${be} stress_tournament_${be}_split_handoff_own_n8_probe)
  # Shorter handoff path (--ho, docs/handoff.md): all three options and each
  # alone; clear+skip with the shared-owner wait (grant needs --wait own).
  foreach(alg lamport tournament hybrid ak)
    add_test(NAME stress_${alg}_${be}_split_handoff_hoall_cap1
      COMMAND ncl_stress -a ${alg} -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --ho all -K 1 --timeout 100)
    add_test(NAME stress_${alg}_${be}_split_handoff_hoall_scan
      COMMAND ncl_stress -a ${alg} -b ${be} -U split -n 4 -t 3 -l 2 -i 10000 -M handoff --ho all -X scan -H --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split_handoff_hoall_cap1 stress_${alg}_${be}_split_handoff_hoall_scan)
  endforeach()
  foreach(ho grant clear skip)
    add_test(NAME stress_tournament_${be}_split_handoff_ho${ho}_n8
      COMMAND ncl_stress -a tournament -b ${be} -U split -n 8 -t 1 -l 1 -i 20000 -M handoff --ho ${ho} --timeout 100)
    list(APPEND split_tests_${be} stress_tournament_${be}_split_handoff_ho${ho}_n8)
  endforeach()
  add_test(NAME stress_hybrid_${be}_split_handoff_owner_hoclearskip
    COMMAND ncl_stress -a hybrid -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --wait owner --ho clear,skip -K 1 --timeout 100)
  list(APPEND split_tests_${be} stress_hybrid_${be}_split_handoff_owner_hoclearskip)
  add_test(NAME stress_hybrid_${be}_split_handoff_owner_cap1
    COMMAND ncl_stress -a hybrid -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --wait owner -K 1 --timeout 100)
  list(APPEND split_tests_${be} stress_hybrid_${be}_split_handoff_owner_cap1)
  add_test(NAME stress_hybrid_${be}_split_n8_batch
    COMMAND ncl_stress -a hybrid -b ${be} -U split -n 8 -t 2 -l 2 -i 10000 -Q batch --timeout 100)
  add_test(NAME stress_lamport_${be}_split_local
    COMMAND ncl_stress -a lamport -b ${be} -U split -n 4 -t 3 -l 2 -i 10000 -M local -H --timeout 100)
  list(APPEND split_tests_${be} stress_hybrid_${be}_split_n8_batch stress_lamport_${be}_split_local)
endforeach()
set_tests_properties(${split_tests_uc} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME split_rejects_cached_flush
  COMMAND ncl_stress -a lamport -b cached-flush -U split -i 100)
set_tests_properties(split_rejects_cached_flush PROPERTIES WILL_FAIL TRUE)
# The emulator must catch missing cache maintenance on the scan region:
# present[] is safety-critical for the hybrid splitter reset (mutex
# violations); a lost queue message hangs the lock server (watchdog FAIL).
# (2b tolerates lost announces by design: waiters recover through the gate.)
foreach(k wb inv)
  add_test(NAME emu_split_detects_missing_meta_${k}_hybrid
    COMMAND ncl_stress -a hybrid -b emu -U split -n 4 -t 2 -l 2 -i 5000 --skip-meta-${k} --timeout 20)
  add_test(NAME emu_split_detects_missing_meta_${k}_server
    COMMAND ncl_stress -a server -b emu -U split -n 4 -t 2 -l 2 -i 5000 --skip-meta-${k} --timeout 5)
  set_tests_properties(emu_split_detects_missing_meta_${k}_hybrid
    emu_split_detects_missing_meta_${k}_server PROPERTIES WILL_FAIL TRUE)
endforeach()
# Anderson-Kim fast path (-a ak): as published (-F always) and the fairness
# variant (default); -m runs it with P = all threads (Obstacle[]/indx over P);
# -d 2000 stretches the windows the Obstacle mechanism guards.
foreach(be emu uc)
  add_test(NAME stress_ak_${be}_always
    COMMAND ncl_stress -a ak -b ${be} -n 6 -t 2 -l 2 -i 20000 -F always)
  add_test(NAME stress_ak_${be}_n8_delay
    COMMAND ncl_stress -a ak -b ${be} -n 8 -t 2 -l 1 -i 10000 -d 2000)
  add_test(NAME stress_ak_${be}_always_n8_delay
    COMMAND ncl_stress -a ak -b ${be} -n 8 -t 2 -l 1 -i 10000 -d 2000 -F always)
endforeach()
set_tests_properties(stress_ak_uc_always stress_ak_uc_n8_delay stress_ak_uc_always_n8_delay
  PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME stress_ak_emu_nosub_always
  COMMAND ncl_stress -a ak -b emu -n 3 -t 3 -l 2 -i 10000 -m -F always)
# Splitter reset race (docs/hybrid.md, "Reset race"): --reset-delay widens the
# reset check's window (as a preempted releaser would). The default (strict
# scan, plus the fastsrv server's present[P] marker) must pass; the old hint
# rule (--debug-unsafe-reopen) must fail. -F always is ak-only.
foreach(be emu uc)
  foreach(alg hybrid fastsrv)
    add_test(NAME stress_${alg}_${be}_reset_window
      COMMAND ncl_stress -a ${alg} -b ${be} -n 6 -t 2 -l 2 -i 60000 --reset-delay 3000 --timeout 100)
  endforeach()
endforeach()
set_tests_properties(stress_hybrid_uc_reset_window stress_fastsrv_uc_reset_window
  PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME emu_detects_reset_race_hint
  COMMAND ncl_stress -a hybrid -b emu --debug-unsafe-reopen -n 6 -t 2 -l 2 -i 60000 --reset-delay 3000 --timeout 100)
set_tests_properties(emu_detects_reset_race_hint PROPERTIES WILL_FAIL TRUE)
foreach(alg hybrid fastsrv)
  add_test(NAME ${alg}_rejects_reopen_always
    COMMAND ncl_stress -a ${alg} -b emu -F always -i 100)
  set_tests_properties(${alg}_rejects_reopen_always PROPERTIES WILL_FAIL TRUE)
endforeach()
# AKSRV: (e) with AK's fast path; the server runs AK's reset at queue drain.
foreach(be emu uc)
  add_test(NAME stress_aksrv_${be}
    COMMAND ncl_stress -a aksrv -b ${be} -n 4 -t 2 -l 3 -i 20000 --timeout 100)
  add_test(NAME stress_aksrv_${be}_always
    COMMAND ncl_stress -a aksrv -b ${be} -F always -n 6 -t 2 -l 2 -i 20000 --timeout 100)
  add_test(NAME stress_aksrv_${be}_n8_delay
    COMMAND ncl_stress -a aksrv -b ${be} -n 8 -t 2 -l 1 -i 10000 -d 2000 --timeout 100)
  add_test(NAME stress_aksrv_${be}_local
    COMMAND ncl_stress -a aksrv -b ${be} -M local -K 4 -H -n 4 -t 3 -l 2 -i 10000 --timeout 100)
endforeach()
set_tests_properties(stress_aksrv_uc stress_aksrv_uc_always stress_aksrv_uc_n8_delay
  stress_aksrv_uc_local PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME stress_aksrv_cached-flush_n8
  COMMAND ncl_stress -a aksrv -b cached-flush -n 8 -t 2 -l 2 -i 10000 --timeout 100)
add_test(NAME stress_aksrv_emu_split
  COMMAND ncl_stress -a aksrv -b emu -U split -n 6 -t 2 -l 2 -i 20000 --timeout 100)
add_test(NAME handoff_rejects_aksrv
  COMMAND ncl_stress -a aksrv -b emu -M handoff)
set_tests_properties(handoff_rejects_aksrv PROPERTIES WILL_FAIL TRUE)
# PTHREAD: process-shared pthread_mutex_t, a coherent-memory reference baseline
# (coherent backend, phase1 only).
add_test(NAME stress_pthread_coherent
  COMMAND ncl_stress -a pthread -b coherent -n 4 -t 2 -l 3 -i 20000 --timeout 100)
add_test(NAME stress_pthread_coherent_ablation
  COMMAND ncl_stress -a pthread -b coherent -m -n 4 -t 3 -l 2 -i 20000 --timeout 100)
add_test(NAME pthread_rejects_noncoherent
  COMMAND ncl_stress -a pthread -b emu -i 100)
set_tests_properties(pthread_rejects_noncoherent PROPERTIES WILL_FAIL TRUE)
add_test(NAME pthread_rejects_local
  COMMAND ncl_stress -a pthread -b coherent -M local -i 100)
set_tests_properties(pthread_rejects_local PROPERTIES WILL_FAIL TRUE)
# NUMA placement and CPU lists (docs/cxl_host.md). Node 0 exists everywhere.
add_test(NAME stress_hybrid_emu_mem_node0
  COMMAND ncl_stress -a hybrid -b emu -U split -n 4 -t 2 -l 2 -i 20000 --mem-node 0 --timeout 100)
add_test(NAME mem_node_rejects_missing_node
  COMMAND ncl_stress -a tournament -b coherent -i 100 --mem-node 1000)
set_tests_properties(mem_node_rejects_missing_node PROPERTIES WILL_FAIL TRUE)
add_test(NAME cpus_rejects_bad_list
  COMMAND ncl_stress -a tournament -b coherent -i 100 -p --cpus 3-1)
set_tests_properties(cpus_rejects_bad_list PROPERTIES WILL_FAIL TRUE)
add_test(NAME stress_tournament_emu_n24
  COMMAND ncl_stress -a tournament -b emu -n 24 -t 1 -l 2 -i 5000 --timeout 100)
