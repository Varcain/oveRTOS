# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_eventgroup_contracts C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
option(OVE_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(OVE_ZERO_HEAP "Exercise zero-heap configuration" OFF)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
foreach(header queue semphr task timers event_groups stream_buffer)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/${header}.h" "#include <FreeRTOS.h>\n")
endforeach()
foreach(platform posix freertos nuttx zephyr)
    string(TOUPPER "${platform}" upper)
    set(config "#pragma once\n#define CONFIG_OVE_EVENTGROUP 1\n#define CONFIG_OVE_RTOS_${upper} 1\n")
    if(OVE_ZERO_HEAP)
        string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${platform}")
    file(WRITE "${CMAKE_BINARY_DIR}/${platform}/ove_config.h" "${config}")
    if(platform STREQUAL "posix")
        set(default_source "${OVE_ROOT}/tests/host/posix/posix_eventgroup.c")
        set(storage_include "${OVE_ROOT}/tests/host/posix/include")
    else()
        set(default_source "${OVE_ROOT}/backends/${platform}/${platform}_eventgroup.c")
        set(storage_include "${OVE_ROOT}/backends/${platform}/include")
    endif()
    set(OVE_${upper}_EVENTGROUP_SOURCE "${default_source}" CACHE FILEPATH "EventGroup provider source")
    add_executable(eventgroup_${platform} provider.c)
    target_compile_definitions(eventgroup_${platform} PRIVATE _GNU_SOURCE
        OVE_EVENTGROUP_PROVIDER="${OVE_${upper}_EVENTGROUP_SOURCE}")
    target_include_directories(eventgroup_${platform} PRIVATE "${CMAKE_BINARY_DIR}/${platform}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk/${platform}" "${CMAKE_BINARY_DIR}/sdk"
        "${OVE_ROOT}/tests/sync_contracts/sdk/${platform}"
        "${OVE_ROOT}/tests/sync_condvar_native/sdk/${platform}"
        "${OVE_ROOT}/include" "${storage_include}" "${OVE_ROOT}/backends/common")
    target_compile_options(eventgroup_${platform} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
        -ffunction-sections -fdata-sections -g -O1)
    target_link_options(eventgroup_${platform} PRIVATE -Wl,--gc-sections)
    target_link_libraries(eventgroup_${platform} PRIVATE pthread)
    if(OVE_SANITIZE)
        target_compile_options(eventgroup_${platform} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(eventgroup_${platform} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()

foreach(platform posix freertos nuttx zephyr)
    string(TOUPPER "${platform}" upper)
    if(platform STREQUAL "posix")
        set(irq_source "${OVE_ROOT}/tests/host/posix/posix_irq.c")
    else()
        set(irq_source "${OVE_ROOT}/backends/${platform}/${platform}_irq.c")
    endif()
    add_executable(eventgroup_context_${platform} context.c "${irq_source}")
    target_compile_definitions(eventgroup_context_${platform} PRIVATE _GNU_SOURCE)
    target_include_directories(eventgroup_context_${platform} PRIVATE
        "${CMAKE_BINARY_DIR}/${platform}" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/${platform}" "${CMAKE_BINARY_DIR}/sdk"
        "${OVE_ROOT}/tests/sync_contracts/sdk/${platform}"
        "${OVE_ROOT}/tests/sync_condvar_native/sdk/${platform}"
        "${OVE_ROOT}/include" "${OVE_ROOT}/backends/common")
    target_compile_options(eventgroup_context_${platform} PRIVATE -Wall -Wextra -Werror -g -O1)
    if(OVE_SANITIZE)
        target_compile_options(eventgroup_context_${platform} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(eventgroup_context_${platform} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()

add_executable(eventgroup_integration integration.c
    "${OVE_ROOT}/tests/host/posix/posix_eventgroup.c"
    "${OVE_ROOT}/tests/host/posix/posix_irq.c" "${OVE_ROOT}/src/ove_eventgroup.c")
target_compile_definitions(eventgroup_integration PRIVATE _GNU_SOURCE)
target_include_directories(eventgroup_integration PRIVATE "${CMAKE_BINARY_DIR}/posix"
    "${OVE_ROOT}/include" "${OVE_ROOT}/tests/host/posix/include"
    "${OVE_ROOT}/backends/common")
target_compile_options(eventgroup_integration PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -g -O1)
target_link_libraries(eventgroup_integration PRIVATE pthread)
if(OVE_SANITIZE)
    target_compile_options(eventgroup_integration PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(eventgroup_integration PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all)
endif()
