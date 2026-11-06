# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(firmware C ASM)

if(NOT CMAKE_CROSSCOMPILING OR NOT BOARD_DIR MATCHES "/qemu-mps2-an500/freertos$")
    message(FATAL_ERROR "Event controls require the ARM an500 FreeRTOS workspace")
endif()
option(OVE_EVENT_MPU "Build the real CM4-MPU V1 route without a Linux personality" OFF)
option(OVE_EVENT_TARGET_CANARY "Retain an early harmless failure through all controls" OFF)

if(OVE_EVENT_MPU)
    include("${OVE_DIR}/cmake/OveCommon.cmake")
    include("${OVE_DIR}/cmake/OveFreeRTOS.cmake")
    ove_setup_project()
    ove_add_board_sources("${BOARD_DIR}/startup_CMSDK_CM7.s"
        "${BOARD_DIR}/system_CMSDK_CM7.c" "${CMAKE_CURRENT_SOURCE_DIR}/entry.c")
    ove_add_freertos_kernel(ARM_CM4_MPU)
    ove_add_backend_sources()
    ove_exclude_backends(BOARD GPIO TIME WATCHDOG)
    ove_link_firmware("${BOARD_DIR}/mps2_an500.ld")
    target_compile_definitions(firmware.elf PRIVATE
        configUSE_MPU_WRAPPERS_V1=1 configTOTAL_MPU_REGIONS=16
        configENABLE_ERRATA_837070_WORKAROUND=0
        configALLOW_UNPRIVILEGED_CRITICAL_SECTIONS=0
        configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY=0)
else()
    add_subdirectory("${BOARD_DIR}" board)
    get_target_property(_sources firmware.elf SOURCES)
    list(REMOVE_ITEM _sources "${BOARD_DIR}/main.c")
    set_property(TARGET firmware.elf PROPERTY SOURCES "${_sources}")
    target_sources(firmware.elf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/entry.c")
endif()

if(NOT EXISTS "${OVE_GEN_DIR}/FreeRTOSConfig.h")
    message(FATAL_ERROR "Configure the isolated workspace before building the Event control")
endif()
# Test-local failure disposition; generated config and stock kernel stay intact.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/event-config")
file(WRITE "${CMAKE_BINARY_DIR}/event-config/FreeRTOSConfig.h"
    "#include \"${OVE_GEN_DIR}/FreeRTOSConfig.h\"\n"
    "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
    "void event_target_reject(void);\n"
    "#ifdef __cplusplus\n}\n#endif\n"
    "#undef configASSERT\n#define configASSERT(x) do { if (!(x)) event_target_reject(); } while (0)\n")
target_include_directories(firmware.elf BEFORE PRIVATE "${CMAKE_BINARY_DIR}/event-config")
target_include_directories(firmware.elf PRIVATE "${OVE_DIR}/tests")
target_compile_options(firmware.elf PRIVATE -fstack-usage)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/entry.c"
    TARGET_DIRECTORY firmware.elf PROPERTIES COMPILE_OPTIONS "-Wall;-Werror")
target_link_options(firmware.elf PRIVATE
    -Wl,--undefined=event_compile_task_anchor
    -Wl,--undefined=event_compile_isr_anchor
    -Wl,--undefined=event_native_domain_values)
if(OVE_EVENT_TARGET_CANARY)
    target_compile_definitions(firmware.elf PRIVATE OVE_EVENT_TARGET_CANARY=1)
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/event-elf.txt" CONTENT "$<TARGET_FILE:firmware.elf>\n")
