# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_nuttx_timeouts C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
set(OVE_TEST_CONVERSION "${OVE_ROOT}/backends/nuttx/include/ove_ns_to_ticks.h" CACHE FILEPATH "Conversion body")
option(OVE_TEST_SAFE_WAIT "Observe separate finite wait conversion" ON)
option(OVE_SANITIZE "Instrument arithmetic and native observers" OFF)
foreach(bits 32 64)
 foreach(tick 100000 1000000 1500000 10000000)
  set(name "arith_${bits}_${tick}")
  add_executable(${name} arithmetic.c)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${OVE_ROOT}/include")
  target_compile_definitions(${name} PRIVATE OVE_CLOCK_BITS=${bits} OVE_TICK_NS=${tick}
      OVE_TEST_CONVERSION="${OVE_TEST_CONVERSION}" OVE_TEST_SAFE_WAIT=$<BOOL:${OVE_TEST_SAFE_WAIT}>)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
  if(OVE_SANITIZE)
   target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
   target_link_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
  endif()
 endforeach()
endforeach()

set(OVE_NUTTX_SOURCE_DIR "${OVE_ROOT}/backends/nuttx" CACHE PATH "Native provider bodies")
set(OVE_NUTTX_PRIVATE_INCLUDE "${OVE_NUTTX_SOURCE_DIR}/include" CACHE PATH "Private conversion header")
foreach(bits 32 64)
 foreach(tick 1000000 10000000)
  foreach(mode heap zero)
   set(name "native_${bits}_${tick}_${mode}")
   add_executable(${name} native.c
      "${OVE_NUTTX_SOURCE_DIR}/nuttx_queue.c" "${OVE_NUTTX_SOURCE_DIR}/nuttx_sync.c"
      "${OVE_NUTTX_SOURCE_DIR}/nuttx_eventgroup.c" "${OVE_NUTTX_SOURCE_DIR}/nuttx_stream.c"
      "${OVE_NUTTX_SOURCE_DIR}/nuttx_timer.c" "${OVE_ROOT}/src/ove_queue.c"
      "${OVE_ROOT}/src/ove_sync.c" "${OVE_ROOT}/src/ove_eventgroup.c"
      "${OVE_ROOT}/src/ove_stream.c" "${OVE_ROOT}/src/ove_timer.c")
   target_compile_definitions(${name} PRIVATE _GNU_SOURCE OVE_CLOCK_BITS=${bits} OVE_TICK_NS=${tick})
   if(mode STREQUAL "zero")
    target_compile_definitions(${name} PRIVATE OVE_MODEL_ZERO_HEAP=1)
   endif()
   target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${OVE_ROOT}/tests/sync_contracts/sdk/nuttx"
      "${OVE_NUTTX_PRIVATE_INCLUDE}" "${OVE_ROOT}/backends/nuttx/include"
      "${OVE_ROOT}/backends/common" "${OVE_ROOT}/include")
   target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g)
   if(OVE_SANITIZE)
    target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
   endif()
  endforeach()
 endforeach()
endforeach()
