# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_freertos_event_timeouts C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
set(OVE_EVENT_SOURCE "${OVE_ROOT}/backends/freertos/freertos_sync.c" CACHE FILEPATH "Event adapter source")
option(OVE_SANITIZE "Instrument actual adapter and observers" OFF)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
foreach(header task semphr queue timers event_groups stream_buffer)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/${header}.h" "#include \"FreeRTOS.h\"\n")
endforeach()
function(event_profile bits abort mode suspend)
    set(name "event_${bits}_${abort}_${mode}_${suspend}")
    set(folder "${CMAKE_BINARY_DIR}/configs/${name}")
    file(MAKE_DIRECTORY "${folder}")
    set(config "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_SYNC 1\n")
    if(mode STREQUAL "zero")
        string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
    endif()
    file(WRITE "${folder}/ove_config.h" "${config}")
    add_executable(${name} native.c "${OVE_ROOT}/src/ove_sync.c")
    target_include_directories(${name} PRIVATE "${folder}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CMAKE_BINARY_DIR}/sdk"
        "${OVE_ROOT}/include" "${OVE_ROOT}/backends/freertos/include"
        "${OVE_ROOT}/backends/common")
    target_compile_definitions(${name} PRIVATE OVE_MODEL_TICK_BITS=${bits}
        OVE_MODEL_ABORT=${abort} OVE_MODEL_SUSPEND=${suspend}
        OVE_EVENT_SOURCE="${OVE_EVENT_SOURCE}")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g
        -ffunction-sections -fdata-sections)
    target_link_options(${name} PRIVATE -Wl,--gc-sections)
    if(OVE_SANITIZE)
        target_compile_options(${name} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endfunction()
foreach(bits 16 32 64)
    foreach(abort 0 1)
        foreach(mode heap zero)
            event_profile(${bits} ${abort} ${mode} 1)
        endforeach()
    endforeach()
endforeach()
event_profile(32 0 heap 0)
