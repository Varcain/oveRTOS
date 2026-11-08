# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_freertos_event_waiters C CXX)
find_package(Threads REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
set(OVE_DIR "${OVE_ROOT}")
set(OVE_EVENT_SOURCE "${OVE_ROOT}/backends/freertos/freertos_sync.c" CACHE FILEPATH "Event adapter source")
set(OVE_BACKEND_INCLUDE "${OVE_ROOT}/backends/freertos/include" CACHE PATH "Event storage and conversion headers")
option(OVE_MODEL_EVENT_LIST "Observe private live Event records" ON)
option(OVE_SANITIZE "Instrument adapter and initialized observers" OFF)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
foreach(header task semphr queue timers event_groups stream_buffer)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/${header}.h" "#include \"FreeRTOS.h\"\n")
endforeach()


foreach(bits 16 32 64)
    foreach(mpu 0 1)
        foreach(abort 0 1)
            foreach(mode heap zero)
                set(name "threaded_${bits}_${mpu}_${abort}_${mode}")
                set(folder "${CMAKE_BINARY_DIR}/configs/${name}")
                file(MAKE_DIRECTORY "${folder}")
                set(config "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_SYNC 1\n")
                if(mode STREQUAL "zero")
                    string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
                endif()
                file(WRITE "${folder}/ove_config.h" "${config}")
                add_library(provider_${name} STATIC provider.c "${OVE_ROOT}/src/ove_sync.c")
                target_include_directories(provider_${name} PUBLIC "${folder}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CMAKE_BINARY_DIR}/sdk"
                    "${CMAKE_CURRENT_SOURCE_DIR}" "${OVE_ROOT}/include"
                    "${OVE_BACKEND_INCLUDE}" "${OVE_ROOT}/backends/common")
                target_compile_definitions(provider_${name} PUBLIC
                    OVE_EVENT_SOURCE="${OVE_EVENT_SOURCE}"
                    OVE_MODEL_EVENT_LIST=$<BOOL:${OVE_MODEL_EVENT_LIST}>
                    OVE_MODEL_TICK_BITS=${bits} OVE_MODEL_MPU=${mpu} OVE_MODEL_ABORT=${abort})
                target_compile_options(provider_${name} PRIVATE -Wall -Wextra -Werror
                    -Wno-unused-parameter -O1 -g -ffunction-sections -fdata-sections)
                target_link_libraries(provider_${name} PUBLIC Threads::Threads)
                target_link_options(provider_${name} PUBLIC -Wl,--gc-sections)
                add_executable(${name} threaded.c)
                target_link_libraries(${name} PRIVATE provider_${name})
                target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -O1 -g)
                if(OVE_SANITIZE)
                    target_compile_options(provider_${name} PRIVATE -fsanitize=address,undefined
                        -fno-sanitize-recover=all -fno-omit-frame-pointer)
                    target_compile_options(${name} PRIVATE -fsanitize=address,undefined
                        -fno-sanitize-recover=all -fno-omit-frame-pointer)
                    target_link_options(provider_${name} PUBLIC -fsanitize=address,undefined
                        -fno-sanitize-recover=all)
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()
foreach(mode heap zero)
    set(folder "${CMAKE_BINARY_DIR}/configs/${mode}")
    file(MAKE_DIRECTORY "${folder}")
    set(config "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_SYNC 1\n")
    if(mode STREQUAL "zero")
        string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
    endif()
    file(WRITE "${folder}/ove_config.h" "${config}")
    add_executable(event_${mode} native.c "${OVE_ROOT}/src/ove_sync.c")
    target_include_directories(event_${mode} PRIVATE "${folder}"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CMAKE_BINARY_DIR}/sdk"
        "${OVE_ROOT}/include" "${OVE_BACKEND_INCLUDE}" "${OVE_ROOT}/backends/common")
    target_compile_definitions(event_${mode} PRIVATE OVE_EVENT_SOURCE="${OVE_EVENT_SOURCE}"
        OVE_MODEL_EVENT_LIST=$<BOOL:${OVE_MODEL_EVENT_LIST}>)
    target_compile_options(event_${mode} PRIVATE -Wall -Wextra -Werror
        -Wno-unused-parameter -O1 -g -ffunction-sections -fdata-sections)
    target_link_options(event_${mode} PRIVATE -Wl,--gc-sections)
    if(OVE_SANITIZE)
        target_compile_options(event_${mode} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(event_${mode} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()

if(OVE_MODEL_EVENT_LIST)
    foreach(erratum 0 1)
        foreach(mpu 0 1)
            foreach(mode heap zero)
                set(name "masks_${erratum}_${mpu}_${mode}")
                add_executable(${name} masks.c)
                target_include_directories(${name} PRIVATE
                    "${CMAKE_CURRENT_SOURCE_DIR}/mask_sdk"
                    "${CMAKE_BINARY_DIR}/configs/threaded_32_${mpu}_0_${mode}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CMAKE_BINARY_DIR}/sdk"
                    "${OVE_ROOT}/include" "${OVE_BACKEND_INCLUDE}" "${OVE_ROOT}/backends/common")
                target_compile_definitions(${name} PRIVATE OVE_EVENT_SOURCE="${OVE_EVENT_SOURCE}"
                    OVE_MODEL_MPU=${mpu} OVE_MODEL_ERRATUM=${erratum}
                    configMAX_SYSCALL_INTERRUPT_PRIORITY=128)
                target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g
                    -ffunction-sections -fdata-sections)
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
endif()

include("${OVE_ROOT}/cmake/OveBindingsCommon.cmake")
_ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/binding_sizes.c"
    "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
foreach(mode heap zero)
    set(provider "provider_threaded_32_0_0_${mode}")
    set(generated "${CMAKE_BINARY_DIR}/bindings_${mode}")
    file(MAKE_DIRECTORY "${generated}")
    configure_file("${CMAKE_BINARY_DIR}/configs/threaded_32_0_0_${mode}/ove_config.h"
        "${generated}/ove_config.h" COPYONLY)
    _ove_binding_build_sizes_probe(binding_sizes_${mode} ${provider}
        "${CMAKE_BINARY_DIR}/binding_sizes.c")
    _ove_binding_extract_sizes("${generated}/ove_storage_sizes.env" binding_sizes_${mode}
        "Extracting actual Event observer storage sizes")
    add_custom_target(binding_storage_${mode} DEPENDS "${generated}/ove_storage_sizes.env")
    add_executable(shared_cpp_${mode} EXCLUDE_FROM_ALL bindings.cpp)
    target_include_directories(shared_cpp_${mode} PRIVATE "${OVE_ROOT}/bindings/cpp")
    target_link_libraries(shared_cpp_${mode} PRIVATE ${provider})
    target_compile_options(shared_cpp_${mode} PRIVATE -std=c++23 -Wall -Wextra -Werror)
endforeach()
