cmake_minimum_required(VERSION 3.16)
project(ncl C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(NCL_SANITIZE "Build with -fsanitize=address,undefined" OFF)
set(NCL_MAX_NODES 16 CACHE STRING "Max nodes")
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

add_library(ncl STATIC src/mem.c src/base.c src/lock.c src/server.c)
target_include_directories(ncl PUBLIC include)
target_compile_definitions(ncl PUBLIC
  NCL_MAX_NODES=${NCL_MAX_NODES} NCL_MAX_PARTICIPANTS=${NCL_MAX_PARTICIPANTS})
target_link_libraries(ncl PUBLIC Threads::Threads)

add_executable(ncl_stress tests/stress.c)
target_link_libraries(ncl_stress ncl)

add_executable(ncl_bench bench/microbench.c)
target_link_libraries(ncl_bench ncl m)

enable_testing()
foreach(alg lamport tournament hybrid server fastsrv)
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
  add_test(NAME stress_${alg}_emu_precise
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 2 -l 3 -i 20000 -H)
endforeach()
# Phase 2a local handoff: precise-hint accumulation (-K 4 -H), overflow
# fallback (-H), and cap-forced releases (-K 1).
foreach(alg lamport tournament hybrid server fastsrv)
  add_test(NAME stress_${alg}_emu_local
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -H)
  add_test(NAME stress_${alg}_emu_local_cap4
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -K 4 -H)
  add_test(NAME stress_${alg}_emu_local_cap1
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 3 -l 2 -i 10000 -M local -K 1)
endforeach()
# (d)/(e) with the strict all-slots fast-release check (-F scan; default is hint).
foreach(alg hybrid fastsrv)
  add_test(NAME stress_${alg}_emu_fastrel_scan
    COMMAND ncl_stress -a ${alg} -b emu -n 4 -t 2 -l 3 -i 20000 -F scan)
  add_test(NAME stress_${alg}_uc_fastrel_scan
    COMMAND ncl_stress -a ${alg} -b uc -n 4 -t 2 -l 3 -i 20000 -F scan)
  set_tests_properties(stress_${alg}_uc_fastrel_scan PROPERTIES
    SKIP_RETURN_CODE 77 RESOURCE_LOCK ncl_uc_device)
endforeach()
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
