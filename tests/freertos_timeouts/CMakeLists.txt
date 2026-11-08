# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_freertos_timeouts C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
set(OVE_TEST_CONVERSION "${OVE_ROOT}/backends/freertos/include/ove_ns_to_ticks.h" CACHE FILEPATH "Private conversion body")
option(OVE_EXTENDED_RATES "Include large positive rate arithmetic models" OFF)
option(OVE_SANITIZE "Instrument arithmetic and native observers" OFF)
set(rates 3 100 1000 1024 3000 20000 1000000000)
if(OVE_EXTENDED_RATES)
    list(APPEND rates 1000000001 3000000000 18446744073709551615)
endif()
foreach(bits 16 32 64)
    foreach(rate IN LISTS rates)
        set(name "arith_${bits}_${rate}")
        add_executable(${name} arithmetic.c)
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk"
            "${CMAKE_CURRENT_SOURCE_DIR}" "${OVE_ROOT}/include")
        target_compile_definitions(${name} PRIVATE OVE_TICK_BITS=${bits} OVE_TICK_RATE=${rate}ULL
            OVE_TEST_CONVERSION="${OVE_TEST_CONVERSION}")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g)
        if(OVE_SANITIZE)
            target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
            target_link_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
        endif()
    endforeach()
endforeach()

option(OVE_NATIVE_MODELS "Compile inert actual-consumer fixtures" OFF)
if(OVE_NATIVE_MODELS)
    set(OVE_LWIP_DIR "" CACHE PATH "Stock lwIP source checkout")
    if(NOT EXISTS "${OVE_LWIP_DIR}/src/include/lwip/dns.h" OR
       NOT EXISTS "${OVE_LWIP_DIR}/src/include/lwip/sockets.h")
        message(FATAL_ERROR "Set OVE_LWIP_DIR to the pinned lwIP checkout, or use run.py to fetch it")
    endif()
    foreach(bits 16 32 64)
        foreach(rate 1000 3000 20000)
            foreach(mode heap zero)
                set(name "native_${bits}_${rate}_${mode}")
                set(folder "${CMAKE_BINARY_DIR}/configs/${name}")
                file(MAKE_DIRECTORY "${folder}")
                set(config "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_QUEUE 1\n#define CONFIG_OVE_STREAM 1\n#define CONFIG_OVE_EVENTGROUP 1\n#define CONFIG_OVE_NET 1\n")
                if(mode STREQUAL "zero")
                    string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
                endif()
                file(WRITE "${folder}/ove_config.h" "${config}")
                add_executable(${name} native.c
                    "${OVE_ROOT}/backends/freertos/freertos_queue.c"
                    "${OVE_ROOT}/backends/freertos/freertos_sync.c"
                    "${OVE_ROOT}/backends/freertos/freertos_stream.c"
                    "${OVE_ROOT}/backends/freertos/freertos_eventgroup.c"
                    "${OVE_ROOT}/backends/freertos/freertos_net.c"
                    "${OVE_ROOT}/src/ove_queue.c" "${OVE_ROOT}/src/ove_sync.c"
                    "${OVE_ROOT}/src/ove_stream.c" "${OVE_ROOT}/src/ove_eventgroup.c")
                target_include_directories(${name} PRIVATE "${folder}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/native-sdk"
                    "${OVE_ROOT}/tests/eventgroup_contracts/sdk/freertos"
                    "${OVE_ROOT}/tests/eventgroup_deferred/sdk"
                    "${OVE_ROOT}/include" "${OVE_ROOT}/backends/freertos/include"
                    "${OVE_ROOT}/backends/common"
                    "${OVE_LWIP_DIR}/src/include"
                    "${OVE_ROOT}/backends/freertos/lwip_port")
                target_compile_definitions(${name} PRIVATE _GNU_SOURCE
                    OVE_TICK_BITS=${bits} OVE_MODEL_TICK_BITS=${bits} OVE_TICK_RATE=${rate}ULL
                    OVE_TEST_CONVERSION="${OVE_TEST_CONVERSION}")
                target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
                    -ffunction-sections -fdata-sections -O1 -g)
                target_link_options(${name} PRIVATE -Wl,--gc-sections)
                if(OVE_SANITIZE)
                    target_compile_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
                    target_link_options(${name} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
                endif()
            endforeach()
        endforeach()
    endforeach()
endif()

# Invalid configurations are compile controls, never executable subjects.
foreach(bits 16 32 64)
    add_library(reject_zero_${bits} OBJECT EXCLUDE_FROM_ALL zero.c)
    target_include_directories(reject_zero_${bits} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sdk"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${OVE_ROOT}/include")
    target_compile_definitions(reject_zero_${bits} PRIVATE OVE_TICK_BITS=${bits} OVE_TICK_RATE=0ULL
        OVE_TEST_CONVERSION="${OVE_TEST_CONVERSION}")
    target_compile_options(reject_zero_${bits} PRIVATE -Wall -Wextra -Werror)
endforeach()
