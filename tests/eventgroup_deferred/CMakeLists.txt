# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_eventgroup_deferred C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
set(OVE_DEFERRED_PROVIDER "${OVE_ROOT}/backends/freertos/freertos_eventgroup.c"
    CACHE FILEPATH "EventGroup provider body")
option(OVE_SANITIZE "Enable native address/undefined sanitizers" OFF)
foreach(profile heap zero mpu_heap mpu_zero minimum nondefault)
    set(depth 10)
    set(width 32)
    set(mpu 0)
    set(trace 0)
    set(config "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_EVENTGROUP 1\n")
    if(profile MATCHES "zero|nondefault")
        string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
    endif()
    if(profile MATCHES "mpu")
        set(mpu 1)
    endif()
    if(profile STREQUAL "mpu_heap")
        set(trace 1)
    endif()
    if(profile STREQUAL "heap" OR profile STREQUAL "mpu_zero")
        set(width 16)
    endif()
    if(profile STREQUAL "minimum")
        set(depth 1)
    elseif(profile STREQUAL "nondefault")
        set(depth 3)
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${profile}")
    file(WRITE "${CMAKE_BINARY_DIR}/${profile}/ove_config.h" "${config}")
    add_executable(deferred_${profile} model.c)
    target_compile_definitions(deferred_${profile} PRIVATE
        OVE_DEFERRED_PROVIDER="${OVE_DEFERRED_PROVIDER}"
        OVE_MODEL_TIMER_DEPTH=${depth} OVE_MODEL_TICK_BITS=${width} OVE_MODEL_MPU=${mpu}
        configUSE_TRACE_FACILITY=${trace})
    target_include_directories(deferred_${profile} PRIVATE "${CMAKE_BINARY_DIR}/${profile}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${OVE_ROOT}/tests/eventgroup_contracts/sdk/freertos"
        "${OVE_ROOT}/include" "${OVE_ROOT}/backends/freertos/include"
        "${OVE_ROOT}/backends/common")
    target_compile_options(deferred_${profile} PRIVATE -Wall -Wextra -Werror
        -Wno-unused-parameter -ffunction-sections -fdata-sections -O1 -g)
    target_link_options(deferred_${profile} PRIVATE -Wl,--gc-sections)
    if(OVE_SANITIZE)
        target_compile_options(deferred_${profile} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(deferred_${profile} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()
