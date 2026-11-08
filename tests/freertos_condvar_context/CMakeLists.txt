# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_freertos_condvar_context C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
set(OVE_CV_SOURCE "${OVE_ROOT}/backends/freertos/freertos_sync.c" CACHE FILEPATH "CV adapter source")
option(OVE_SANITIZE "Instrument initialized register and ownership observers" OFF)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
foreach(header task semphr queue timers event_groups stream_buffer)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/${header}.h" "#include \"FreeRTOS.h\"\n")
endforeach()
foreach(bits 16 32)
    foreach(context ordinary v1allow0 v1allow1)
        foreach(erratum 0 1)
            foreach(mode heap zero)
                set(name "context_${bits}_${context}_${erratum}_${mode}")
                set(folder "${CMAKE_BINARY_DIR}/configs/${name}")
                file(MAKE_DIRECTORY "${folder}")
                set(config "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_SYNC 1\n")
                if(mode STREQUAL "zero")
                    string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
                endif()
                file(WRITE "${folder}/ove_config.h" "${config}")
                add_executable(${name} context.c)
                target_include_directories(${name} PRIVATE "${folder}"
                    "${OVE_ROOT}/tests/freertos_event_waiters/mask_sdk"
                    "${OVE_ROOT}/tests/freertos_event_waiters/sdk" "${CMAKE_BINARY_DIR}/sdk"
                    "${OVE_ROOT}/include" "${OVE_ROOT}/backends/freertos/include"
                    "${OVE_ROOT}/backends/common")
                target_compile_definitions(${name} PRIVATE OVE_CV_SOURCE="${OVE_CV_SOURCE}"
                    OVE_MODEL_TICK_BITS=${bits} OVE_MODEL_ERRATUM=${erratum}
                    OVE_MODEL_MPU=$<NOT:$<STREQUAL:${context},ordinary>>
                    OVE_MODEL_ALLOW=$<STREQUAL:${context},v1allow1>
                    configMAX_SYSCALL_INTERRUPT_PRIORITY=128)
                target_compile_options(${name} PRIVATE -Wall -Wextra -Werror
                    -Wno-unused-parameter -O1 -g -ffunction-sections -fdata-sections)
                target_link_options(${name} PRIVATE -Wl,--gc-sections)
                if(OVE_SANITIZE)
                    target_compile_options(${name} PRIVATE -fsanitize=address,undefined
                        -fno-sanitize-recover=all -fno-omit-frame-pointer)
                    target_link_options(${name} PRIVATE -fsanitize=address,undefined
                        -fno-sanitize-recover=all)
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()
