cmake_minimum_required(VERSION 3.16)
project(ncl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(NCL_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(NCL_STATS "Diagnostic counters in the lock paths (OFF: compiled out)" ON)
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

add_library(ncl STATIC src/mem.c src/data.c src/base.c src/lock.c src/server.c src/cpus.c)
target_include_directories(ncl PUBLIC include)
target_compile_definitions(ncl PUBLIC
  NCL_MAX_NODES=${NCL_MAX_NODES} NCL_MAX_PARTICIPANTS=${NCL_MAX_PARTICIPANTS})
target_link_libraries(ncl PUBLIC Threads::Threads)
if(NOT NCL_STATS)
  target_compile_definitions(ncl PUBLIC NCL_NO_STATS)
endif()

add_executable(ncl_stress tests/stress.c bench/harness.c)
target_link_libraries(ncl_stress ncl)

add_executable(ncl_bench bench/microbench.c bench/harness.c)
target_link_libraries(ncl_bench ncl m)
add_executable(ncl_memlat bench/memlat.c)
target_link_libraries(ncl_memlat ncl)
add_executable(ncl_memconc bench/memconc.c)
target_link_libraries(ncl_memconc ncl)
add_executable(ncl_kv bench/kv.c bench/harness.c)
target_link_libraries(ncl_kv ncl m)

enable_testing()
# Every node is a separate process: tests run the binaries through the launcher.
set(NCL_RUN ${CMAKE_SOURCE_DIR}/scripts/ncl_run.sh)
# Launcher and harness: ncl_bench smoke runs (closed and open loop, server
# threads, 2a), and a node whose command line differs from the creator's must be
# rejected (exit 2, WILL_FAIL).
add_test(NAME bench_emu_tournament
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_bench> -a tournament -b emu -n 4 -t 2 -s 0.3 -W 0.1 -P -C)
add_test(NAME bench_cached-flush_bakery_open
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_bench> -a bakery -b cached-flush -n 4 -t 2 -l 4 -s 0.3 -W 0.1 -r 200k -P)
add_test(NAME bench_emu_server_local
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_bench> -a server -b emu -M local -n 3 -t 2 -s 0.3 -W 0.1 -P)
add_test(NAME harness_detects_mismatched_command_line
  COMMAND sh -c "f=$(mktemp /dev/shm/ncl.XXXXXX) && $<TARGET_FILE:ncl_stress> -n 2 -d 100 --shm $f --init >/dev/null; $<TARGET_FILE:ncl_stress> -n 2 -d 200 --shm $f --node 0; r=$?; rm -f $f; exit $r")
set_tests_properties(harness_detects_mismatched_command_line PROPERTIES WILL_FAIL TRUE TIMEOUT 20)
foreach(alg lamport tournament hybrid server fastsrv ak)
  foreach(be emu cached-flush coherent uc)
    add_test(NAME stress_${alg}_${be}
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 3 -i 20000)
    # uc needs kmod/ncl_uc.ko loaded; skipped (exit 77) otherwise.
    set_tests_properties(stress_${alg}_${be} PROPERTIES SKIP_RETURN_CODE 77)
    if(be STREQUAL "uc")
      # /dev/ncl_uc allows one opener at a time.
      set_tests_properties(stress_${alg}_${be} PROPERTIES RESOURCE_LOCK ncl_uc_device)
    endif()
  endforeach()
  if(NOT alg MATCHES "server|fastsrv")  # (c)/(e) require the submutex
    add_test(NAME stress_${alg}_emu_nosub
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b emu -n 3 -t 3 -l 2 -i 10000 -m)
  endif()
  # Batched multi-line reads (cached-flush only; default -Q batch): N=8 with the
  # scan-heavy options. Exercises the batch index logic on a coherent host;
  # it cannot expose missing flushes or fences (see include/ncl/mem.h).
  add_test(NAME stress_${alg}_cached-flush_batch_n8
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b cached-flush -n 8 -t 2 -l 2 -i 10000)
  add_test(NAME stress_${alg}_emu_precise
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b emu -n 4 -t 2 -l 3 -i 20000 -H)
endforeach()
# Phase 2a local handoff: precise-hint accumulation (-K 4 -H), overflow
# fallback (-H), and cap-forced releases (-K 1).
foreach(alg lamport tournament hybrid server fastsrv ak)
  add_test(NAME stress_${alg}_emu_local
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -H)
  add_test(NAME stress_${alg}_emu_local_cap4
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -K 4 -H)
  add_test(NAME stress_${alg}_emu_local_cap1
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -K 1)
endforeach()
# Phase 2b: gate + owner handoff (design §3.3). Default = announce before the
# gate + adaptive pick_waiter; also the spec baseline (inside/scan), and each
# of inside and scan alone. -K 1 makes almost every release a cross-node handoff.
foreach(alg lamport tournament hybrid ak)
  foreach(be emu uc)
    add_test(NAME stress_${alg}_${be}_handoff
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 3 -l 2 -i 10000 -M handoff -H)
    add_test(NAME stress_${alg}_${be}_handoff_inside_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -G inside -K 1)
    add_test(NAME stress_${alg}_${be}_handoff_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -K 1)
    add_test(NAME stress_${alg}_${be}_handoff_inside_scan
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -G inside -X scan)
    add_test(NAME stress_${alg}_${be}_handoff_scan_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -X scan -K 1)
    # Default wait = own line (docs/handoff.md); also the shared-owner poll.
    add_test(NAME stress_${alg}_${be}_handoff_owner_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff --wait owner -K 1 --timeout 100)
    add_test(NAME stress_${alg}_${be}_handoff_owner_inside
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 3 -l 2 -i 10000 -M handoff --wait owner -G inside -H --timeout 100)
    if(be STREQUAL "uc")
      set_tests_properties(stress_${alg}_uc_handoff stress_${alg}_uc_handoff_inside_cap1
        stress_${alg}_uc_handoff_cap1 stress_${alg}_uc_handoff_inside_scan
        stress_${alg}_uc_handoff_scan_cap1 stress_${alg}_uc_handoff_owner_cap1
        stress_${alg}_uc_handoff_owner_inside
        PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
    endif()
  endforeach()
endforeach()
add_test(NAME emu_detects_missing_writeback_handoff
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -n 4 -t 3 -l 1 -i 5000 -M handoff -K 1 --skip-wb)
set_tests_properties(emu_detects_missing_writeback_handoff PROPERTIES WILL_FAIL TRUE)
add_test(NAME emu_detects_missing_invalidate_handoff
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b emu -n 4 -t 3 -l 1 -i 5000 -M handoff -K 1 --skip-inv)
set_tests_properties(emu_detects_missing_invalidate_handoff PROPERTIES WILL_FAIL TRUE)
add_test(NAME stress_hybrid_cached-flush_probe_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b cached-flush -n 8 -t 2 -l 2 -i 10000 -Q probe)
add_test(NAME stress_lamport_cached-flush_probe_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a lamport -b cached-flush -n 8 -t 2 -l 2 -i 10000 -Q probe)
add_test(NAME stress_tournament_cached-flush_probe_handoff_scan
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff -X scan -K 1 -Q probe)
add_test(NAME stress_tournament_cached-flush_batch_handoff_scan
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff -X scan -K 1)
add_test(NAME stress_tournament_cached-flush_handoff_own_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff --wait own -K 1 --timeout 100)
add_test(NAME stress_tournament_cached-flush_handoff_hoall_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff --ho all -K 1 --timeout 100)
foreach(alg lamport hybrid)
  # Uniform layout: announce lines are UC (uc) or emulated-uncached (emu).
  foreach(be emu uc)
    add_test(NAME stress_${alg}_${be}_handoff_hoall_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff --ho all -K 1 --timeout 100)
  endforeach()
  set_tests_properties(stress_${alg}_uc_handoff_hoall_cap1 PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
endforeach()
# --ho all is the 2b default: keep the original handoff path (--ho none) covered.
foreach(alg lamport tournament hybrid ak)
  foreach(be emu uc)
    add_test(NAME stress_${alg}_${be}_handoff_honone_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff --ho none -K 1 --timeout 100)
    add_test(NAME stress_${alg}_${be}_split_handoff_honone_scan
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -U split -n 4 -t 3 -l 2 -i 10000 -M handoff --ho none -X scan -H --timeout 100)
  endforeach()
  set_tests_properties(stress_${alg}_uc_handoff_honone_cap1 stress_${alg}_uc_split_handoff_honone_scan
    PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
endforeach()
add_test(NAME stress_tournament_cached-flush_handoff_honone_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b cached-flush -n 8 -t 2 -l 2 -i 10000 -M handoff --ho none -K 1 --timeout 100)
add_test(NAME ho_rejects_grant_owner_wait
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -M handoff --wait owner --ho grant)
add_test(NAME ho_rejects_phase1
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu --ho all)
add_test(NAME ho_rejects_server
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b emu -M handoff --ho clear)
add_test(NAME ho_rejects_skip_inside
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -M handoff -G inside --ho skip)
set_tests_properties(ho_rejects_grant_owner_wait ho_rejects_phase1 ho_rejects_server ho_rejects_skip_inside
  PROPERTIES WILL_FAIL TRUE)
add_test(NAME wait_rejects_server
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b emu -M handoff --wait own)
add_test(NAME wait_rejects_phase1
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu --wait owner)
set_tests_properties(wait_rejects_server wait_rejects_phase1 PROPERTIES WILL_FAIL TRUE)
add_test(NAME handoff_rejects_fastsrv
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a fastsrv -b emu -M handoff)
set_tests_properties(handoff_rejects_fastsrv PROPERTIES WILL_FAIL TRUE)
# (c) in handoff mode: holder-to-waiter chain via the server's succ[] ring
# (docs/handoff.md). _wrap runs > 2^14 grants on one lock with a window that
# keeps ending at the same ring slot (the stale-tag wrap bug hung this);
# _window2 uses a window smaller than N (frequent server resync).
foreach(be emu uc)
  add_test(NAME stress_server_${be}_handoff
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b ${be} -n 4 -t 3 -l 2 -i 10000 -M handoff -H)
  add_test(NAME stress_server_${be}_handoff_cap1
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b ${be} -n 4 -t 2 -l 2 -i 10000 -M handoff -K 1)
  add_test(NAME stress_server_${be}_handoff_scan
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b ${be} -n 4 -t 2 -l 3 -i 10000 -M handoff -X scan -K 1)
  add_test(NAME stress_server_${be}_handoff_window2
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b ${be} -n 6 -t 2 -l 2 -i 10000 -M handoff -K 1 -R 2)
  add_test(NAME stress_server_${be}_handoff_wrap
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b ${be} -n 6 -t 1 -l 1 -i 20000 -M handoff)
  set_tests_properties(stress_server_${be}_handoff_wrap PROPERTIES TIMEOUT 120)
  if(be STREQUAL "uc")
    set_tests_properties(stress_server_uc_handoff stress_server_uc_handoff_cap1
      stress_server_uc_handoff_scan stress_server_uc_handoff_wrap stress_server_uc_handoff_window2
      PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
  endif()
endforeach()
add_test(NAME emu_detects_missing_writeback_server_handoff
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b emu -n 4 -t 3 -l 1 -i 5000 -M handoff -K 1 --skip-wb)
set_tests_properties(emu_detects_missing_writeback_server_handoff PROPERTIES WILL_FAIL TRUE)
add_test(NAME emu_detects_missing_writeback_local
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -n 4 -t 3 -l 1 -i 5000 -M local --skip-wb)
set_tests_properties(emu_detects_missing_writeback_local PROPERTIES WILL_FAIL TRUE)
# The emulator must catch missing cache maintenance.
add_test(NAME emu_detects_missing_writeback
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a lamport -b emu -n 4 -t 1 -l 1 -i 5000 --skip-wb)
set_tests_properties(emu_detects_missing_writeback PROPERTIES WILL_FAIL TRUE)
add_test(NAME emu_detects_missing_invalidate
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a lamport -b emu -n 4 -t 1 -l 1 -i 5000 --skip-inv)
set_tests_properties(emu_detects_missing_invalidate PROPERTIES WILL_FAIL TRUE)
# Split metadata layout (-U split, docs/backends.md "Per-region memory types"):
# scanned arrays in a cached region, single words uncached. On emu the scan
# region has per-node private views, so these runs check the cached-metadata
# accesses (batched/probe scans included); on uc they run the real mix.
foreach(be emu uc)
  foreach(alg lamport tournament hybrid server fastsrv ak)
    add_test(NAME stress_${alg}_${be}_split
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -U split -n 4 -t 2 -l 3 -i 20000 --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split)
  endforeach()
  foreach(alg lamport tournament hybrid server)
    add_test(NAME stress_${alg}_${be}_split_handoff_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff -K 1 -X scan --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split_handoff_cap1)
  endforeach()
  # Split layout: the default own-line wait makes the grant a cached store into
  # the waiter's announce line (two writers, causally ordered).
  foreach(alg lamport tournament hybrid ak)
    add_test(NAME stress_${alg}_${be}_split_handoff_own_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --wait own -K 1 --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split_handoff_own_cap1)
  endforeach()
  add_test(NAME stress_tournament_${be}_split_handoff_own_n8_scan
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b ${be} -U split -n 8 -t 1 -l 1 -i 20000 -M handoff --wait own -X scan --timeout 100)
  list(APPEND split_tests_${be} stress_tournament_${be}_split_handoff_own_n8_scan)
  # Shorter handoff path (--ho, docs/handoff.md): all three options and each
  # alone; clear+skip with the shared-owner wait (grant needs --wait own).
  foreach(alg lamport tournament hybrid ak)
    add_test(NAME stress_${alg}_${be}_split_handoff_hoall_cap1
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --ho all -K 1 --timeout 100)
    add_test(NAME stress_${alg}_${be}_split_handoff_hoall_scan
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -U split -n 4 -t 3 -l 2 -i 10000 -M handoff --ho all -X scan -H --timeout 100)
    list(APPEND split_tests_${be} stress_${alg}_${be}_split_handoff_hoall_cap1 stress_${alg}_${be}_split_handoff_hoall_scan)
  endforeach()
  foreach(ho grant clear skip)
    add_test(NAME stress_tournament_${be}_split_handoff_ho${ho}_n8
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b ${be} -U split -n 8 -t 1 -l 1 -i 20000 -M handoff --ho ${ho} --timeout 100)
    list(APPEND split_tests_${be} stress_tournament_${be}_split_handoff_ho${ho}_n8)
  endforeach()
  add_test(NAME stress_hybrid_${be}_split_handoff_owner_hoclearskip
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --wait owner --ho clear,skip -K 1 --timeout 100)
  list(APPEND split_tests_${be} stress_hybrid_${be}_split_handoff_owner_hoclearskip)
  add_test(NAME stress_hybrid_${be}_split_handoff_owner_cap1
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b ${be} -U split -n 4 -t 2 -l 2 -i 10000 -M handoff --wait owner -K 1 --timeout 100)
  list(APPEND split_tests_${be} stress_hybrid_${be}_split_handoff_owner_cap1)
  add_test(NAME stress_hybrid_${be}_split_n8_batch
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b ${be} -U split -n 8 -t 2 -l 2 -i 10000 -Q batch --timeout 100)
  add_test(NAME stress_lamport_${be}_split_local
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a lamport -b ${be} -U split -n 4 -t 3 -l 2 -i 10000 -M local -H --timeout 100)
  list(APPEND split_tests_${be} stress_hybrid_${be}_split_n8_batch stress_lamport_${be}_split_local)
endforeach()
set_tests_properties(${split_tests_uc} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME split_rejects_cached_flush
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a lamport -b cached-flush -U split -i 100)
set_tests_properties(split_rejects_cached_flush PROPERTIES WILL_FAIL TRUE)
# The emulator must catch missing cache maintenance on the scan region:
# present[] is safety-critical for the hybrid splitter reset (mutex
# violations); a lost queue message hangs the lock server (watchdog FAIL).
# (2b tolerates lost announces by design: waiters recover through the gate.)
foreach(k wb inv)
  add_test(NAME emu_split_detects_missing_meta_${k}_hybrid
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b emu -U split -n 4 -t 2 -l 2 -i 5000 --skip-meta-${k} --timeout 20)
  add_test(NAME emu_split_detects_missing_meta_${k}_server
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a server -b emu -U split -n 4 -t 2 -l 2 -i 5000 --skip-meta-${k} --timeout 5)
  set_tests_properties(emu_split_detects_missing_meta_${k}_hybrid
    emu_split_detects_missing_meta_${k}_server PROPERTIES WILL_FAIL TRUE)
endforeach()
# KV store (bench/kv.c, docs/kv.md): every run checks each read and, at the
# end, every record and that versions sum to the updates; any error fails it.
set(KV_ARGS -n 4 -t 2 -l 16 -k 20000 -s 0.5 -W 0.1 -P)
foreach(m phase1 local handoff)
  foreach(w A F)
    add_test(NAME kv_emu_${m}_${w} COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -M ${m} -w ${w} ${KV_ARGS})
  endforeach()
  add_test(NAME kv_emu_split_${m}_affinity COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -U split -M ${m} -F 0.8 ${KV_ARGS})
  add_test(NAME kv_emu_algmap_${m} COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -M ${m} --algmap top:4 ${KV_ARGS})
  add_test(NAME kv_uc_split_algmap_${m} COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b uc -U split -M ${m} --algmap top:4 ${KV_ARGS})
  set_tests_properties(kv_uc_split_algmap_${m} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
endforeach()
add_test(NAME kv_emu_server_handoff COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a server -M handoff ${KV_ARGS})
add_test(NAME kv_emu_full_local COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu --full -M local -d uniform ${KV_ARGS})
add_test(NAME kv_cf_local_uniform COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b cached-flush -M local -d uniform ${KV_ARGS})
# Coherent baselines (sweep phase kvbase): pthread mutex per stripe with and without the
# submutex; -m with a non-coherent algorithm (every thread a participant) on emu.
add_test(NAME kv_coherent_pthread_nosub COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b coherent -a pthread -m ${KV_ARGS})
add_test(NAME kv_coherent_pthread COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b coherent -a pthread ${KV_ARGS})
add_test(NAME kv_emu_tournament_nosub COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a tournament -m ${KV_ARGS})
add_test(NAME kv_rejects_nosub_local COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -m -M local ${KV_ARGS})
set_tests_properties(kv_rejects_nosub_local PROPERTIES WILL_FAIL TRUE)
# The emulator must catch each missing piece of precise data maintenance:
# invalidate, writeback, refresh on a local handoff, and writing back dirty
# lines before invalidating them (reached only with long local runs: -K 256,
# few records per stripe).
foreach(d skip-inv skip-wb no-local-refresh)
  add_test(NAME kv_emu_detects_${d} COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -M local --debug ${d} ${KV_ARGS})
  set_tests_properties(kv_emu_detects_${d} PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME kv_emu_detects_no-dirty-wb
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -n 2 -t 4 -l 2 -k 2000 -M local -K 256 -s 1 -W 0.1 -P --debug no-dirty-wb)
add_test(NAME kv_emu_dirty_wb_long_runs
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -n 2 -t 4 -l 2 -k 2000 -M local -K 256 -s 1 -W 0.1 -P)
set_tests_properties(kv_emu_detects_no-dirty-wb PROPERTIES WILL_FAIL TRUE)
# Reactive lock (-a reactive, design §4f): default thresholds (8:4), switching on
# every global acquisition (--debug-react 8), that plus a widened late-y-store
# window (--lamport-delay), and 0:64 with a widened mode-check window, in every
# mode and layout; -d 2000 stretches the critical sections. Each unsafe variant
# must FAIL under the conditions that expose it: switch without taking the
# tournament (1), no mode re-check (2), no INVALID release (4), no slow-path
# re-check after a late y store (16).
foreach(m phase1 local handoff)
  foreach(be emu uc)
    foreach(v "def" "toggle" "toggle_late" "fast_mode")
      if(v STREQUAL "def")
        set(rx_args "")
      elseif(v STREQUAL "toggle")
        set(rx_args --debug-react 8)
      elseif(v STREQUAL "toggle_late")
        set(rx_args --debug-react 8 --lamport-delay 3000)
      else()
        set(rx_args --react 0:64 --mode-delay 3000)
      endif()
      add_test(NAME stress_reactive_${be}_${m}_${v}
        COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b ${be} -n 6 -t 2 -l 3 -i 20000 -d 2000 -M ${m} ${rx_args} --timeout 90)
      add_test(NAME stress_reactive_${be}_split_${m}_${v}
        COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b ${be} -U split -n 6 -t 2 -l 3 -i 20000 -d 2000 -M ${m} ${rx_args} --timeout 90)
      if(be STREQUAL "uc")
        set_tests_properties(stress_reactive_uc_${m}_${v} stress_reactive_uc_split_${m}_${v}
          PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
      endif()
    endforeach()
  endforeach()
endforeach()
add_test(NAME reactive_emu_detects_no_new_lock
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b emu -n 8 -t 2 -l 2 -i 50000 -d 2000 --react 0:64 --debug-react 1 --mode-delay 3000 --timeout 40)
add_test(NAME reactive_emu_detects_no_mode_check
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b emu -n 8 -t 2 -l 2 -i 50000 -d 2000 --debug-react 10 --timeout 20)
add_test(NAME reactive_emu_detects_no_invalid
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b emu -n 8 -t 2 -l 2 -i 50000 -d 2000 --debug-react 12 --timeout 20)
add_test(NAME reactive_emu_detects_no_slow_recheck
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b emu -n 8 -t 2 -l 2 -i 50000 -d 2000 --debug-react 24 --lamport-delay 3000 --timeout 20)
# The tournament path reads `mode` in the root's entry check, ordered after the root's
# flag and turn loads (batched on cached-flush). Safe variants on cached-flush; loading
# `mode` first, with a gap before the flag/turn loads, must FAIL.
foreach(m phase1 local handoff)
  add_test(NAME stress_reactive_cf_${m}_toggle
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b cached-flush -n 6 -t 2 -l 3 -i 20000 -d 2000 -M ${m} --debug-react 8 --timeout 90)
  add_test(NAME stress_reactive_cf_${m}_fast_mode
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b cached-flush -n 6 -t 2 -l 3 -i 20000 -d 2000 -M ${m} --react 0:64 --mode-delay 3000 --timeout 90)
endforeach()
# The xy-first block layout ([x][y][mode][tree]): safe variants and two unsafe ones.
foreach(be emu cached-flush)
  foreach(m phase1 local)
    add_test(NAME stress_reactive_xyfirst_${be}_${m}_toggle_late
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b ${be} -n 6 -t 2 -l 3 -i 20000 -d 2000 -M ${m} --react-layout xy-first --debug-react 8 --lamport-delay 3000 --timeout 90)
    add_test(NAME stress_reactive_xyfirst_${be}_${m}_fast_mode
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b ${be} -n 6 -t 2 -l 3 -i 20000 -d 2000 -M ${m} --react-layout xy-first --react 0:64 --mode-delay 3000 --timeout 90)
  endforeach()
endforeach()
add_test(NAME reactive_xyfirst_detects_no_new_lock
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b emu -n 8 -t 2 -l 2 -i 50000 -d 2000 --react-layout xy-first --react 0:64 --debug-react 1 --mode-delay 3000 --timeout 40)
add_test(NAME reactive_xyfirst_detects_no_invalid
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b emu -n 8 -t 2 -l 2 -i 50000 -d 2000 --react-layout xy-first --debug-react 12 --timeout 20)
set_tests_properties(reactive_xyfirst_detects_no_new_lock reactive_xyfirst_detects_no_invalid PROPERTIES WILL_FAIL TRUE)
add_test(NAME kv_emu_reactive_pinned_xyfirst COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive -M local --algmap top:4 --react off --react-layout xy-first ${KV_ARGS})
add_test(NAME kv_emu_reactive_xyfirst_0_64 COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive -M local --react 0:64 --react-layout xy-first ${KV_ARGS})
add_test(NAME reactive_emu_detects_mode_before_flag
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b emu -n 8 -t 2 -l 1 -i 50000 -d 2000 --react 0:64 --debug-react 32 --mode-delay 5000 --timeout 20)
add_test(NAME reactive_cf_detects_mode_before_flag
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a reactive -b cached-flush -n 8 -t 2 -l 1 -i 50000 -d 2000 --react 0:64 --debug-react 32 --mode-delay 5000 --timeout 20)
set_tests_properties(reactive_emu_detects_no_new_lock reactive_emu_detects_no_mode_check
  reactive_emu_detects_no_invalid reactive_emu_detects_no_slow_recheck
  reactive_emu_detects_mode_before_flag reactive_cf_detects_mode_before_flag PROPERTIES WILL_FAIL TRUE)
add_test(NAME kv_emu_reactive_local COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive -M local ${KV_ARGS})
add_test(NAME kv_emu_reactive_local_0_64 COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive -M local --react 0:64 ${KV_ARGS})
add_test(NAME kv_emu_reactive_local_dwell COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive -M local --react 4:2:1024 ${KV_ARGS})
# Reactive lock pinned to an initial per-lock mode (isolation runs): the
# tournament-mode locks start with Lamport's y = INVALID and never switch.
add_test(NAME kv_emu_reactive_pinned COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive -M local --algmap top:4 --react off ${KV_ARGS})
add_test(NAME kv_emu_reactive_pinned_then_switch COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive -M local --algmap top:8 --react 0:64 ${KV_ARGS})
add_test(NAME reactive_rejects_short_dwell COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a reactive --react 4:4:10 ${KV_ARGS})
set_tests_properties(reactive_rejects_short_dwell PROPERTIES WILL_FAIL TRUE)
# Per-lock algorithms (cfg.alg_per_lock, used by ncl_kv): even locks Lamport, odd
# locks tournament, in each mode and layout.
foreach(m phase1 local handoff)
  foreach(be emu uc)
    add_test(NAME stress_mixalgs_${be}_${m}
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b ${be} -n 4 -t 2 -l 4 -i 10000 -M ${m} --mix-algs --timeout 60)
    add_test(NAME stress_mixalgs_${be}_split_${m}
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a lamport -b ${be} -U split -n 4 -t 2 -l 4 -i 10000 -M ${m} --mix-algs --timeout 60)
    if(be STREQUAL "uc")
      set_tests_properties(stress_mixalgs_uc_${m} stress_mixalgs_uc_split_${m}
        PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
    endif()
  endforeach()
endforeach()
add_test(NAME mixalgs_rejects_hybrid COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b emu --mix-algs)
set_tests_properties(mixalgs_rejects_hybrid PROPERTIES WILL_FAIL TRUE)
# Full-line writeback of cached metadata (the emulator's default, as hardware):
# the two-writer announce line (waiter's announce + releaser's grant) is safe
# only because a waiter that learned of its handoff from `owner` waits for the
# grant before clearing its announce (design §2, §5). --grant-delay widens that
# window (otherwise reached ~once per 10^5 handoffs). With the wait: PASS, at
# the acquire (--ho none) and at the release (--ho clear,skip). Without it
# (--debug-no-grant-wait): the releaser's late full-line writeback resurrects
# the cleared announce: stale_announce (--ho none) or, at the release (clear),
# a lock granted to a node that is not waiting, a hang the watchdog catches -> FAIL. The old word-only model
# (--emu-word-wb) cannot express the overwrite and passes the broken protocol.
foreach(ho none clear,skip)
  string(REPLACE "," "" hn ${ho})
  add_test(NAME emu_line_wb_grant_wait_${hn}
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -U split -n 4 -t 2 -l 1 -i 3000 -M handoff --ho ${ho} -K 1 --grant-delay 50000 --timeout 60)
  add_test(NAME emu_line_wb_detects_no_grant_wait_${hn}
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -U split -n 4 -t 2 -l 1 -i 3000 -M handoff --ho ${ho} -K 1 --grant-delay 50000 --debug-no-grant-wait --timeout 5)
  set_tests_properties(emu_line_wb_detects_no_grant_wait_${hn} PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME emu_word_wb_misses_no_grant_wait
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -U split -n 4 -t 2 -l 1 -i 3000 -M handoff --ho none -K 1 --grant-delay 50000 --debug-no-grant-wait --emu-word-wb --timeout 60)
add_test(NAME emu_word_wb_rejects_uniform
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -M handoff --emu-word-wb)
set_tests_properties(emu_word_wb_rejects_uniform PROPERTIES WILL_FAIL TRUE)
# Anderson-Kim fast path (-a ak): as published (-F always) and the fairness
# variant (default); -m runs it with P = all threads (Obstacle[]/indx over P);
# -d 2000 stretches the windows the Obstacle mechanism guards.
foreach(be emu uc)
  add_test(NAME stress_ak_${be}_always
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ak -b ${be} -n 6 -t 2 -l 2 -i 20000 -F always)
  add_test(NAME stress_ak_${be}_n8_delay
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ak -b ${be} -n 8 -t 2 -l 1 -i 10000 -d 2000)
  add_test(NAME stress_ak_${be}_always_n8_delay
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ak -b ${be} -n 8 -t 2 -l 1 -i 10000 -d 2000 -F always)
endforeach()
set_tests_properties(stress_ak_uc_always stress_ak_uc_n8_delay stress_ak_uc_always_n8_delay
  PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME stress_ak_emu_nosub_always
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ak -b emu -n 3 -t 3 -l 2 -i 10000 -m -F always)
# Splitter reset race (docs/hybrid.md, "Reset race"): --reset-delay widens the
# reset check's window (as a preempted releaser would). The default (strict
# scan, plus the fastsrv server's present[P] marker) must pass; the old hint
# rule (--debug-unsafe-reopen) must fail. -F always is ak-only.
foreach(be emu uc)
  foreach(alg hybrid fastsrv)
    add_test(NAME stress_${alg}_${be}_reset_window
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b ${be} -n 6 -t 2 -l 2 -i 60000 --reset-delay 3000 --timeout 100)
  endforeach()
endforeach()
set_tests_properties(stress_hybrid_uc_reset_window stress_fastsrv_uc_reset_window
  PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME emu_detects_reset_race_hint
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b emu --debug-unsafe-reopen -n 6 -t 2 -l 2 -i 60000 --reset-delay 3000 --timeout 100)
set_tests_properties(emu_detects_reset_race_hint PROPERTIES WILL_FAIL TRUE)
foreach(alg hybrid fastsrv)
  add_test(NAME ${alg}_rejects_reopen_always
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a ${alg} -b emu -F always -i 100)
  set_tests_properties(${alg}_rejects_reopen_always PROPERTIES WILL_FAIL TRUE)
endforeach()
# AKSRV: (e) with AK's fast path; the server runs AK's reset at queue drain.
foreach(be emu uc)
  add_test(NAME stress_aksrv_${be}
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a aksrv -b ${be} -n 4 -t 2 -l 3 -i 20000 --timeout 100)
  add_test(NAME stress_aksrv_${be}_always
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a aksrv -b ${be} -F always -n 6 -t 2 -l 2 -i 20000 --timeout 100)
  add_test(NAME stress_aksrv_${be}_n8_delay
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a aksrv -b ${be} -n 8 -t 2 -l 1 -i 10000 -d 2000 --timeout 100)
  add_test(NAME stress_aksrv_${be}_local
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a aksrv -b ${be} -M local -K 4 -H -n 4 -t 3 -l 2 -i 10000 --timeout 100)
endforeach()
set_tests_properties(stress_aksrv_uc stress_aksrv_uc_always stress_aksrv_uc_n8_delay
  stress_aksrv_uc_local PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME stress_aksrv_cached-flush_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a aksrv -b cached-flush -n 8 -t 2 -l 2 -i 10000 --timeout 100)
add_test(NAME stress_aksrv_emu_split
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a aksrv -b emu -U split -n 6 -t 2 -l 2 -i 20000 --timeout 100)
add_test(NAME handoff_rejects_aksrv
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a aksrv -b emu -M handoff)
set_tests_properties(handoff_rejects_aksrv PROPERTIES WILL_FAIL TRUE)
# PTHREAD: process-shared pthread_mutex_t, a coherent-memory reference baseline
# (coherent backend, phase1 only).
add_test(NAME stress_pthread_coherent
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a pthread -b coherent -n 4 -t 2 -l 3 -i 20000 --timeout 100)
add_test(NAME stress_pthread_coherent_ablation
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a pthread -b coherent -m -n 4 -t 3 -l 2 -i 20000 --timeout 100)
add_test(NAME pthread_rejects_noncoherent
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a pthread -b emu -i 100)
set_tests_properties(pthread_rejects_noncoherent PROPERTIES WILL_FAIL TRUE)
add_test(NAME pthread_rejects_local
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a pthread -b coherent -M local -i 100)
set_tests_properties(pthread_rejects_local PROPERTIES WILL_FAIL TRUE)
# NUMA placement and CPU lists (docs/cxl_host.md). Node 0 exists everywhere.
add_test(NAME stress_hybrid_emu_mem_node0
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a hybrid -b emu -U split -n 4 -t 2 -l 2 -i 20000 --mem-node 0 --timeout 100)
add_test(NAME mem_node_rejects_missing_node
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b coherent -i 100 --mem-node 1000)
set_tests_properties(mem_node_rejects_missing_node PROPERTIES WILL_FAIL TRUE)
add_test(NAME cpus_rejects_bad_list
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b coherent -i 100 -p --cpus 3-1)
set_tests_properties(cpus_rejects_bad_list PROPERTIES WILL_FAIL TRUE)
add_test(NAME stress_tournament_emu_n24
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a tournament -b emu -n 24 -t 1 -l 2 -i 5000 --timeout 100)
# Flat merged bakery (-a bakery, design §4g g3/g5/g6): every backend, the split layout,
# each mode, the ablation, and a widened doorway (--bakery-delay). Dropping the choosing
# store (1) or ignoring choosing in the wait (2) must FAIL with the widened doorway.
foreach(be emu cached-flush coherent uc)
  add_test(NAME stress_bakery_${be}
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b ${be} -n 4 -t 2 -l 3 -i 20000 --timeout 100)
  set_tests_properties(stress_bakery_${be} PROPERTIES SKIP_RETURN_CODE 77)
  if(be STREQUAL "uc")
    set_tests_properties(stress_bakery_uc PROPERTIES RESOURCE_LOCK ncl_uc_device)
  endif()
endforeach()
add_test(NAME stress_bakery_cached-flush_batch_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b cached-flush -n 8 -t 2 -l 2 -i 10000 --timeout 100)
add_test(NAME stress_bakery_emu_nosub
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b emu -n 3 -t 3 -l 2 -i 10000 -m --timeout 100)
add_test(NAME stress_bakery_emu_precise
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b emu -n 4 -t 2 -l 3 -i 20000 -H --timeout 100)
foreach(be emu uc)
  foreach(m phase1 local handoff)
    add_test(NAME stress_bakery_${be}_split_${m}
      COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b ${be} -U split -n 6 -t 2 -l 3 -i 20000 -d 2000 -M ${m} -K 1 --timeout 100)
    if(be STREQUAL "uc")
      set_tests_properties(stress_bakery_uc_split_${m} PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
    endif()
  endforeach()
endforeach()
add_test(NAME stress_bakery_strict_emu_doorway
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-wait strict -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --timeout 200)
add_test(NAME bakery_strict_emu_detects_ignore_choosing
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-wait strict -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --debug-bakery 2 --timeout 40)
set_tests_properties(bakery_strict_emu_detects_ignore_choosing PROPERTIES WILL_FAIL TRUE)
foreach(m phase1 local handoff)
  add_test(NAME stress_bakery_emu_${m}_doorway
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 -M ${m} --bakery-delay 3000 --timeout 200)
endforeach()
add_test(NAME bakery_emu_detects_no_choosing
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --debug-bakery 1 --timeout 40)
add_test(NAME bakery_emu_detects_ignore_choosing
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --debug-bakery 2 --timeout 40)
set_tests_properties(bakery_emu_detects_no_choosing bakery_emu_detects_ignore_choosing PROPERTIES WILL_FAIL TRUE)
# Ablation of predecessor polling (--bakery-poll all: every poll re-reads all blockers).
add_test(NAME stress_bakery_emu_pollall_doorway
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --bakery-poll all --timeout 200)
add_test(NAME stress_bakery_cached-flush_pollall_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery -b cached-flush -n 8 -t 2 -l 2 -i 10000 --bakery-poll all --timeout 100)
add_test(NAME kv_emu_bakery_local COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a bakery -M local ${KV_ARGS})
add_test(NAME kv_emu_bakery_handoff COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a bakery -M handoff ${KV_ARGS})
# k-ary bakery tree (--bakery-arity K): partial nodes at every level (n=7 k=3,
# n=17 k=4: 17 -> 5 -> 2), each backend family, split layout, 2a, ablation, KV.
# Releasing leaf to root (--debug-bakery 4) lets a lower node's next winner write a
# seat still held above and must FAIL with the widened gap (--bakery-delay).
foreach(nk "7;3" "17;4" "8;2")
  list(GET nk 0 n)
  list(GET nk 1 k)
  add_test(NAME stress_bakery_tree_emu_n${n}_k${k}
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity ${k} -b emu -n ${n} -t 2 -l 2 -i 5000 --timeout 200)
endforeach()
add_test(NAME stress_bakery_tree_cached-flush_n16_k4
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity 4 -b cached-flush -n 16 -t 1 -l 2 -i 10000 --timeout 100)
add_test(NAME stress_bakery_tree_uc_n8_k2
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity 2 -b uc -n 8 -t 2 -l 2 -i 10000 --timeout 100)
set_tests_properties(stress_bakery_tree_uc_n8_k2 PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME stress_bakery_tree_emu_split_local
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity 4 -b emu -U split -n 9 -t 2 -l 3 -i 10000 -d 2000 -M local -K 1 --timeout 200)
add_test(NAME stress_bakery_tree_emu_nosub
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity 4 -b emu -n 3 -t 3 -l 2 -i 10000 -m --timeout 200)
foreach(k 2 4)
  add_test(NAME stress_bakery_tree_emu_k${k}_delay
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity ${k} -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --timeout 200)
  add_test(NAME bakery_tree_k${k}_detects_leaf_first_release
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity ${k} -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --debug-bakery 4 --timeout 60)
  set_tests_properties(bakery_tree_k${k}_detects_leaf_first_release PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME bakery_tree_detects_no_choosing
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity 4 -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --debug-bakery 1 --timeout 60)
set_tests_properties(bakery_tree_detects_no_choosing PROPERTIES WILL_FAIL TRUE)
add_test(NAME kv_emu_bakery_tree_local COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a bakery --bakery-arity 2 -M local ${KV_ARGS})
# Classic wait rule (--bakery-wait classic): flat and tree, doorway widened, 2a, cf, KV.
# It must still need the choosing store (--debug-bakery 1), and clearing j from the
# round-2 read, before our ticket is visible (--debug-bakery 8), must FAIL.
foreach(k 0 2)
  add_test(NAME stress_bakery_classic_emu_k${k}_doorway
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity ${k} --bakery-wait classic -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --timeout 200)
  add_test(NAME bakery_classic_k${k}_detects_no_choosing
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity ${k} --bakery-wait classic -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --debug-bakery 1 --timeout 60)
  add_test(NAME bakery_classic_k${k}_detects_early_clear
    COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-arity ${k} --bakery-wait classic -b emu -n 8 -t 2 -l 1 -i 30000 -d 2000 --bakery-delay 3000 --debug-bakery 8 --timeout 60)
  set_tests_properties(bakery_classic_k${k}_detects_no_choosing bakery_classic_k${k}_detects_early_clear
    PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME stress_bakery_classic_emu_split_local
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-wait classic -b emu -U split -n 6 -t 2 -l 3 -i 20000 -d 2000 -M local -K 1 --timeout 200)
add_test(NAME stress_bakery_classic_cached-flush_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-wait classic -b cached-flush -n 8 -t 2 -l 2 -i 10000 --timeout 100)
add_test(NAME stress_bakery_classic_uc_n8
  COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_stress> -a bakery --bakery-wait classic -b uc -n 8 -t 2 -l 2 -i 10000 --timeout 100)
set_tests_properties(stress_bakery_classic_uc_n8 PROPERTIES SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
add_test(NAME kv_emu_bakery_classic_local COMMAND ${NCL_RUN} $<TARGET_FILE:ncl_kv> -b emu -a bakery --bakery-wait classic -M local ${KV_ARGS})
