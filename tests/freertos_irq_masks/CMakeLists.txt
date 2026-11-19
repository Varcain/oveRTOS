# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_freertos_irq_masks C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Compile caller-owned allocation mode" OFF)
option(OVE_SANITIZE "Instrument the register models" OFF)
set(config "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_ASYNC 1\n")
if(OVE_ZERO_HEAP)
    string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/config/ove_config.h" "${config}")
set(rejections fast systick pendsv nmi hardfault fault supervisor debug reserved impossible group)
# MPU V1 with and without unprivileged critical sections, and the ordinary port.
foreach(port mpu0_allow0 mpu1_allow0 mpu1_allow1)
    string(REGEX MATCHALL "[01]" flags "${port}")
    list(GET flags 0 mpu)
    list(GET flags 1 allow)
    foreach(erratum 0 1)
        set(name "${port}_erratum${erratum}")
        add_executable(${name} model.c)
        target_compile_definitions(${name} PRIVATE OVE_MODEL_MPU=${mpu} OVE_MODEL_ALLOW=${allow}
            OVE_MODEL_ERRATUM=${erratum} OVE_FREERTOS_IRQ_PORT_SUPPORTED=1
            OVE_IRQ_SOURCE="${OVE_ROOT}/backends/freertos/freertos_irq.c")
        target_include_directories(${name} PRIVATE "${CMAKE_BINARY_DIR}/config"
            "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${OVE_ROOT}/include" "${OVE_ROOT}/backends/common"
            "${OVE_ROOT}/backends/freertos/include")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g)
        if(OVE_SANITIZE)
            target_compile_options(${name} PRIVATE -fsanitize=address,undefined
                -fno-sanitize-recover=all -fno-omit-frame-pointer)
            target_link_options(${name} PRIVATE -fsanitize=address,undefined
                -fno-sanitize-recover=all)
        endif()
        add_test(NAME ${name}.normal COMMAND ${name} normal)
        set(cases ${rejections})
        if(NOT mpu)
            list(APPEND cases ordinary-user)
        endif()
        foreach(case IN LISTS cases)
            add_test(NAME ${name}.${case} COMMAND ${name} reject ${case})
        endforeach()
    endforeach()
endforeach()
