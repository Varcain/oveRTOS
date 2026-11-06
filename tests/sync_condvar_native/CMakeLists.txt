# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_sync_condvar_tests C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Compile caller-owned allocation mode" OFF)
option(OVE_SANITIZE "Instrument the host adapter observers" OFF)
set(OVE_FREERTOS_SYNC_SOURCE "${OVE_ROOT}/backends/freertos/freertos_sync.c"
    CACHE FILEPATH "FreeRTOS adapter source included by the observer")
set(OVE_ZEPHYR_SYNC_SOURCE "${OVE_ROOT}/backends/zephyr/zephyr_sync.c"
    CACHE FILEPATH "Zephyr adapter source included by the observer")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
foreach(header semphr task queue timers event_groups stream_buffer)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/${header}.h" "#include \"FreeRTOS.h\"\n")
endforeach()
foreach(platform freertos zephyr)
    string(TOUPPER "${platform}" upper)
    set(config "#ifndef OVE_CONFIG_H\n#define OVE_CONFIG_H\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_RTOS_${upper} 1\n")
    if(OVE_ZERO_HEAP)
        string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
    endif()
    string(APPEND config "#endif\n")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${platform}")
    file(WRITE "${CMAKE_BINARY_DIR}/${platform}/ove_config.h" "${config}")
    add_executable(condvar_${platform} ${platform}.c checks.c)
    target_compile_definitions(condvar_${platform} PRIVATE _GNU_SOURCE
        OVE_SYNC_BACKEND_SOURCE="${OVE_${upper}_SYNC_SOURCE}")
    target_include_directories(condvar_${platform} PRIVATE
        "${CMAKE_BINARY_DIR}/${platform}" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/${platform}"
        "${CMAKE_BINARY_DIR}/sdk" "${OVE_ROOT}/include"
        "${OVE_ROOT}/backends/${platform}/include" "${OVE_ROOT}/backends/${platform}"
        "${OVE_ROOT}/backends/common")
    target_compile_options(condvar_${platform} PRIVATE -Wall -Wextra -Werror
        -O0 -g3 -ffunction-sections -fdata-sections)
    target_link_options(condvar_${platform} PRIVATE -Wl,--gc-sections)
    target_link_libraries(condvar_${platform} PRIVATE pthread)
    if(OVE_SANITIZE)
        target_compile_options(condvar_${platform} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(condvar_${platform} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()

set(OVE_CPP_SYNC_HEADER "${OVE_ROOT}/bindings/cpp/ove/sync.hpp"
    CACHE FILEPATH "C++ synchronization header used by the observer")
set(cpp_config "#pragma once\n#define CONFIG_OVE_RTOS_POSIX 1\n#define CONFIG_OVE_SYNC 1\n")
if(OVE_ZERO_HEAP)
    string(APPEND cpp_config "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cpp")
file(WRITE "${CMAKE_BINARY_DIR}/cpp/ove_config.h" "${cpp_config}")
foreach(policy release hook)
    add_executable(condvar_cpp_${policy} cpp.cpp)
    target_compile_definitions(condvar_cpp_${policy} PRIVATE
        OVE_CPP_SYNC_HEADER="${OVE_CPP_SYNC_HEADER}")
    if(policy STREQUAL "release")
        target_compile_definitions(condvar_cpp_${policy} PRIVATE NDEBUG=1)
    else()
        target_compile_definitions(condvar_cpp_${policy} PRIVATE OVE_CONDVAR_RETURNING_ASSERT=1)
    endif()
    target_include_directories(condvar_cpp_${policy} PRIVATE
        "${CMAKE_BINARY_DIR}/cpp" "${OVE_ROOT}/include" "${OVE_ROOT}/bindings/cpp"
        "${OVE_ROOT}/tests/host/posix/include")
    target_compile_options(condvar_cpp_${policy} PRIVATE -Wall -Wextra -Werror -O0 -g3)
    target_link_options(condvar_cpp_${policy} PRIVATE -Wl,--wrap=abort)
    if(OVE_SANITIZE)
        target_compile_options(condvar_cpp_${policy} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(condvar_cpp_${policy} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()

set(OVE_ERRNO_HEADER "${OVE_ROOT}/backends/common/ove_backend_common.h"
    CACHE FILEPATH "Shared errno helper for compile-only domain controls")
foreach(mode 0 1 2)
    add_library(errno_domain_${mode} OBJECT errno_domain.c)
    target_compile_definitions(errno_domain_${mode} PRIVATE
        OVE_ERRNO_MODE=${mode} OVE_ERRNO_HELPER="${OVE_ERRNO_HEADER}")
    target_compile_options(errno_domain_${mode} PRIVATE -Wall -Wextra -Werror)
    target_include_directories(errno_domain_${mode} PRIVATE
        "${CMAKE_BINARY_DIR}/freertos" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/freertos"
        "${CMAKE_BINARY_DIR}/sdk" "${OVE_ROOT}/include"
        "${OVE_ROOT}/backends/freertos/include" "${OVE_ROOT}/backends/common")
endforeach()
