# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(infer_target C CXX ASM)

if(NOT CMAKE_CROSSCOMPILING OR NOT BOARD_DIR MATCHES "/qemu-mps2-an500/freertos$")
    message(FATAL_ERROR "Qualification requires the ARM an500 FreeRTOS toolchain")
endif()
if(OVE_QUALIFY_CMSIS_NN)
    if(NOT EXISTS "${OVE_QUALIFY_CMSIS_NN}/Include/arm_nnfunctions.h" OR
       NOT EXISTS "${OVE_QUALIFY_CMSIS_CORE}/cmsis_gcc.h")
        message(FATAL_ERROR "Supply existing CMSIS-NN sources and CMSIS-Core includes")
    endif()
    set(CONFIG_OVE_INFER_CMSIS_NN ON)
    set(OVE_CMSIS_NN_DIR "${OVE_QUALIFY_CMSIS_NN}")
    set(OVE_CMSIS5_DIR "${OVE_QUALIFY_CMSIS_CORE}/../..")
else()
    unset(CONFIG_OVE_INFER_CMSIS_NN CACHE)
    unset(CONFIG_OVE_INFER_CMSIS_NN)
endif()

add_subdirectory("${BOARD_DIR}" board)
get_target_property(_sources firmware.elf SOURCES)
list(REMOVE_ITEM _sources "${BOARD_DIR}/main.c")
set_property(TARGET firmware.elf PROPERTY SOURCES "${_sources}")
target_sources(firmware.elf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/entry.c")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/entry.c"
    TARGET_DIRECTORY firmware.elf PROPERTIES COMPILE_OPTIONS "-Wall;-Werror")
target_compile_options(firmware.elf PRIVATE -fstack-usage)
add_custom_target(qualifier-images ALL DEPENDS firmware.elf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:firmware.elf>"
        "${CMAKE_CURRENT_BINARY_DIR}/board/firmware.bin"
        "${CMAKE_CURRENT_BINARY_DIR}/board/firmware.hex"
        "${CMAKE_BINARY_DIR}")

if(OVE_QUALIFY_CMSIS_NN)
    add_subdirectory("${OVE_QUALIFY_CMSIS_NN}" cmsis-nn)
    target_include_directories(cmsis-nn PUBLIC "${OVE_QUALIFY_CMSIS_CORE}")
    foreach(_target ove_tflm ove_tflm_port)
        target_include_directories(${_target} PRIVATE "${OVE_QUALIFY_CMSIS_NN}/Include")
    endforeach()
    target_link_libraries(ove_tflm PUBLIC cmsis-nn)
    target_include_directories(firmware.elf PRIVATE "${OVE_QUALIFY_CMSIS_NN}/Include")
    target_compile_definitions(firmware.elf PRIVATE OVE_QUALIFY_CMSIS_NN)
endif()

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/qualifier-elf.txt"
     CONTENT "$<TARGET_FILE:firmware.elf>\n")
