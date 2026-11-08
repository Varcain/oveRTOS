# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_eventgroup_scalar C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(OVE_DIR "${OVE_ROOT}")
set(OVE_SCALAR_PUBLIC_INCLUDE "${OVE_ROOT}/include" CACHE PATH "Public contract headers")
option(OVE_SCALAR_HAS_QUERY "Require the public configured-mask query" ON)
option(OVE_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(OVE_ZERO_HEAP "Use caller-owned storage configuration" OFF)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
foreach(header queue semphr task timers stream_buffer)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/${header}.h" "#include <FreeRTOS.h>\n")
endforeach()

# Real POSIX integration for the public language bindings.
set(binding_config "#pragma once\n#define CONFIG_OVE_EVENTGROUP 1\n#define CONFIG_OVE_TIME 1\n#define CONFIG_OVE_ASYNC 1\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_TIMER 1\n#define CONFIG_OVE_RTOS_POSIX 1\n")
if(OVE_ZERO_HEAP)
    string(APPEND binding_config "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bindings")
file(WRITE "${CMAKE_BINARY_DIR}/bindings/ove_config.h" "${binding_config}")
add_library(eventgroup_binding STATIC binding_clock.c
    "${OVE_ROOT}/tests/host/posix/posix_eventgroup.c"
    "${OVE_ROOT}/tests/host/posix/posix_irq.c" "${OVE_ROOT}/src/ove_eventgroup.c")
target_include_directories(eventgroup_binding PUBLIC "${CMAKE_BINARY_DIR}/bindings"
    "${OVE_ROOT}/include" "${OVE_ROOT}/tests/host/posix/include"
    "${OVE_ROOT}/backends/common")
target_compile_definitions(eventgroup_binding PRIVATE _GNU_SOURCE)
target_compile_options(eventgroup_binding PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
include("${OVE_ROOT}/cmake/OveBindingsCommon.cmake")
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/bindings/ove_storage_sizes.c"
    "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
_ove_binding_build_sizes_probe(binding_sizes eventgroup_binding
    "${CMAKE_BINARY_DIR}/bindings/ove_storage_sizes.c")
_ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/bindings/ove_storage_sizes.env" binding_sizes
    "Extracting actual POSIX EventGroup storage sizes")
add_custom_target(storage_sizes ALL DEPENDS "${CMAKE_BINARY_DIR}/bindings/ove_storage_sizes.env")
foreach(profile posix nuttx zephyr freertos16 freertos32 freertos64)
    set(platform "${profile}")
    if(profile MATCHES "freertos([0-9]+)")
        set(platform freertos)
        set(width "${CMAKE_MATCH_1}")
    endif()
    string(TOUPPER "${platform}" upper)
    set(config "#pragma once\n#define CONFIG_OVE_EVENTGROUP 1\n#define CONFIG_OVE_TIME 1\n#define CONFIG_OVE_RTOS_${upper} 1\n")
    if(OVE_ZERO_HEAP)
        string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${profile}")
    file(WRITE "${CMAKE_BINARY_DIR}/${profile}/ove_config.h" "${config}")
    if(platform STREQUAL "posix")
        set(source "${OVE_ROOT}/tests/host/posix/posix_eventgroup.c")
        set(storage "${OVE_ROOT}/tests/host/posix/include")
    else()
        set(source "${OVE_ROOT}/backends/${platform}/${platform}_eventgroup.c")
        set(storage "${OVE_ROOT}/backends/${platform}/include")
    endif()
    set(OVE_SCALAR_${upper}_SOURCE "${source}" CACHE FILEPATH "EventGroup provider source")
    add_executable(scalar_${profile} scalar.c)
    target_compile_definitions(scalar_${profile} PRIVATE _GNU_SOURCE
        OVE_SCALAR_PROVIDER="${OVE_SCALAR_${upper}_SOURCE}"
        OVE_SCALAR_HAS_QUERY=$<BOOL:${OVE_SCALAR_HAS_QUERY}>)
    if(platform STREQUAL "zephyr")
        target_compile_definitions(scalar_${profile} PRIVATE OVE_MODEL_EVENT_TRACE=1)
    endif()
    if(platform STREQUAL "freertos")
        target_compile_definitions(scalar_${profile} PRIVATE OVE_MODEL_TICK_BITS=${width})
    endif()
    target_include_directories(scalar_${profile} PRIVATE "${CMAKE_BINARY_DIR}/${profile}"
        "${OVE_ROOT}/tests/eventgroup_contracts/sdk/${platform}" "${CMAKE_BINARY_DIR}/sdk"
        "${OVE_ROOT}/tests/sync_contracts/sdk/${platform}"
        "${OVE_ROOT}/tests/sync_condvar_native/sdk/${platform}"
        "${OVE_SCALAR_PUBLIC_INCLUDE}" "${OVE_ROOT}/include"
        "${storage}" "${OVE_ROOT}/backends/common")
    target_compile_options(scalar_${profile} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
        -ffunction-sections -fdata-sections -O1 -g)
    target_link_options(scalar_${profile} PRIVATE -Wl,--gc-sections)
    target_link_libraries(scalar_${profile} PRIVATE pthread)
    if(OVE_SANITIZE)
        target_compile_options(scalar_${profile} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(scalar_${profile} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()
