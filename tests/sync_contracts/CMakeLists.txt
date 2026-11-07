# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_sync_contract_tests C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
option(OVE_ZERO_HEAP "Compile caller-owned allocation mode" OFF)
option(OVE_SANITIZE "Instrument host contract observers" OFF)
set(OVE_POSIX_SYNC_SOURCE "${OVE_ROOT}/tests/host/posix/posix_sync.c"
    CACHE FILEPATH "POSIX adapter included by the observer")
set(OVE_CPP_SYNC_HEADER "${OVE_ROOT}/bindings/cpp/ove/sync.hpp"
    CACHE FILEPATH "C++ synchronization header included by the observer")
set(OVE_COMMON_SYNC_SOURCE "${OVE_ROOT}/src/ove_sync.c"
    CACHE FILEPATH "Common heap synchronization functions included by the observer")
set(OVE_IRQ_HEADER "${OVE_ROOT}/include/ove/irq.h"
    CACHE FILEPATH "Interrupt context header copied into source-gate profiles")
set(OVE_POSIX_IRQ_SOURCE "${OVE_ROOT}/tests/host/posix/posix_irq.c"
    CACHE FILEPATH "POSIX interrupt context implementation")
set(OVE_POSIX_TIMER_SOURCE "${OVE_ROOT}/tests/host/posix/posix_timer.c"
    CACHE FILEPATH "POSIX timer callback implementation")
set(OVE_CONTEXT_TIMER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/context_timer.c"
    CACHE FILEPATH "Owned timer context observer")
set(OVE_FREERTOS_EVENT_OBSERVER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/freertos_event.c"
    CACHE FILEPATH "FreeRTOS Event lifetime observer")
option(OVE_NATIVE_MODELS "Build SDK dispatch models" ON)
option(OVE_CONTEXT_PROBES "Build context source-gate profiles" ON)
option(OVE_OUTCOME_LEGACY "Infer checked ownership from the legacy wait status" OFF)
set(OVE_CONDVAR_OUTCOME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/condvar_outcome.c"
    CACHE FILEPATH "Checked condition-variable ownership observer")
set(OVE_DISABLED_SYNC_HEADER "${OVE_ROOT}/include/ove/sync.h"
    CACHE FILEPATH "Synchronization header used by the disabled outcome observer")
foreach(platform freertos zephyr nuttx)
    string(TOUPPER "${platform}" upper)
    set(OVE_${upper}_SYNC_SOURCE "${OVE_ROOT}/backends/${platform}/${platform}_sync.c"
        CACHE FILEPATH "${platform} synchronization implementation")
    set(OVE_${upper}_IRQ_SOURCE "${OVE_ROOT}/backends/${platform}/${platform}_irq.c"
        CACHE FILEPATH "${platform} interrupt context implementation")
endforeach()
set(config "#pragma once\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_RTOS_POSIX 1\n#define CONFIG_OVE_THREAD_STATE_STATS 1\n")
if(OVE_ZERO_HEAP)
    string(APPEND config "#define CONFIG_OVE_ZERO_HEAP 1\n")
endif()
foreach(policy release hook)
    add_executable(contracts_cpp_${policy} cpp.cpp)
    target_compile_definitions(contracts_cpp_${policy} PRIVATE
        OVE_CPP_SYNC_HEADER="${OVE_CPP_SYNC_HEADER}")
    if(policy STREQUAL "release")
        target_compile_definitions(contracts_cpp_${policy} PRIVATE NDEBUG=1)
    else()
        target_compile_definitions(contracts_cpp_${policy} PRIVATE OVE_RETURNING_ASSERT=1)
    endif()
    target_include_directories(contracts_cpp_${policy} PRIVATE "${CMAKE_BINARY_DIR}"
        "${OVE_ROOT}/include" "${OVE_ROOT}/bindings/cpp"
        "${OVE_ROOT}/tests/host/posix/include")
    target_compile_options(contracts_cpp_${policy} PRIVATE -Wall -Wextra -Werror -O0 -g3)
    target_link_options(contracts_cpp_${policy} PRIVATE -Wl,--wrap=abort)
    if(OVE_SANITIZE)
        target_compile_options(contracts_cpp_${policy} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(contracts_cpp_${policy} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()

function(instrument target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror
        -Wno-unused-parameter -fstack-usage -O0 -g3)
    if(OVE_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endfunction()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/sdk")
foreach(header semphr task queue timers event_groups stream_buffer)
    file(WRITE "${CMAKE_BINARY_DIR}/sdk/${header}.h" "#include \"FreeRTOS.h\"\n")
endforeach()

if(OVE_NATIVE_MODELS)
    foreach(platform freertos zephyr zephyr_core nuttx)
        string(REPLACE "_core" "" backend "${platform}")
        string(TOUPPER "${backend}" upper)
        set(native_config "#pragma once\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_RTOS_${upper} 1\n")
        if(OVE_ZERO_HEAP)
            string(APPEND native_config "#define CONFIG_OVE_ZERO_HEAP 1\n")
        endif()
        if(platform STREQUAL "zephyr_core")
            string(APPEND native_config "#define CONFIG_OBJ_CORE 1\n#define CONFIG_OBJ_CORE_MUTEX 1\n#define CONFIG_OBJ_CORE_SEM 1\n#define CONFIG_OBJ_CORE_CONDVAR 1\n")
        endif()
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${platform}")
        file(WRITE "${CMAKE_BINARY_DIR}/${platform}/ove_config.h" "${native_config}")
        add_executable(contracts_${platform} native.c)
        target_compile_definitions(contracts_${platform} PRIVATE _GNU_SOURCE
            OVE_SYNC_BACKEND_SOURCE="${OVE_${upper}_SYNC_SOURCE}")
        target_include_directories(contracts_${platform} PRIVATE
            "${CMAKE_BINARY_DIR}/${platform}" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/${backend}"
            "${CMAKE_BINARY_DIR}/sdk"
            "${OVE_ROOT}/tests/sync_condvar_native/sdk/${backend}"
            "${OVE_ROOT}/include" "${OVE_ROOT}/backends/${backend}/include"
            "${OVE_ROOT}/backends/common")
        instrument(contracts_${platform})
        if(NOT platform STREQUAL "zephyr_core")
            add_executable(outcome_${platform} "${OVE_CONDVAR_OUTCOME_SOURCE}")
            target_compile_definitions(outcome_${platform} PRIVATE _GNU_SOURCE
                OVE_SYNC_BACKEND_SOURCE="${OVE_${upper}_SYNC_SOURCE}")
            if(OVE_OUTCOME_LEGACY)
                target_compile_definitions(outcome_${platform} PRIVATE OVE_TEST_LEGACY_OUTCOME=1)
            endif()
            target_include_directories(outcome_${platform} PRIVATE
                "${CMAKE_BINARY_DIR}/${platform}" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/${backend}"
                "${CMAKE_BINARY_DIR}/sdk" "${OVE_ROOT}/tests/sync_condvar_native/sdk/${backend}"
                "${OVE_ROOT}/include" "${OVE_ROOT}/backends/${backend}/include"
                "${OVE_ROOT}/backends/common")
            target_compile_options(outcome_${platform} PRIVATE -ffunction-sections -fdata-sections)
            target_link_options(outcome_${platform} PRIVATE -Wl,--gc-sections)
            instrument(outcome_${platform})
        endif()
        if(platform STREQUAL "freertos")
            add_executable(contracts_freertos_event "${OVE_FREERTOS_EVENT_OBSERVER_SOURCE}")
            target_compile_definitions(contracts_freertos_event PRIVATE
                OVE_SYNC_BACKEND_SOURCE="${OVE_FREERTOS_SYNC_SOURCE}")
            target_include_directories(contracts_freertos_event PRIVATE
                "${CMAKE_BINARY_DIR}/freertos" "${CMAKE_CURRENT_SOURCE_DIR}/sdk/freertos"
                "${CMAKE_BINARY_DIR}/sdk" "${OVE_ROOT}/include"
                "${OVE_ROOT}/backends/freertos/include" "${OVE_ROOT}/backends/common")
            instrument(contracts_freertos_event)
        endif()
    endforeach()
endif()

if(OVE_CONTEXT_PROBES)
    foreach(platform freertos zephyr nuttx posix)
        string(TOUPPER "${platform}" upper)
        foreach(features disabled sync async both)
            set(context_dir "${CMAKE_BINARY_DIR}/configs/context_${platform}_${features}")
            file(MAKE_DIRECTORY "${context_dir}/ove")
            set(context_config "#pragma once\n#define CONFIG_OVE_RTOS_${upper} 1\n")
            if(features STREQUAL "sync" OR features STREQUAL "both")
                string(APPEND context_config "#define CONFIG_OVE_SYNC 1\n")
            endif()
            if(features STREQUAL "async" OR features STREQUAL "both")
                string(APPEND context_config "#define CONFIG_OVE_ASYNC 1\n")
            endif()
            if(OVE_ZERO_HEAP)
                string(APPEND context_config "#define CONFIG_OVE_ZERO_HEAP 1\n")
            endif()
            file(WRITE "${context_dir}/ove_config.h" "${context_config}")
            configure_file("${OVE_IRQ_HEADER}" "${context_dir}/ove/irq.h" COPYONLY)
            set(target "context_${platform}_${features}")
            add_executable(${target} context.c "${OVE_${upper}_IRQ_SOURCE}")
            target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
            target_include_directories(${target} PRIVATE "${context_dir}"
                "${CMAKE_CURRENT_SOURCE_DIR}/sdk/${platform}" "${CMAKE_BINARY_DIR}/sdk"
                "${OVE_ROOT}/tests/sync_condvar_native/sdk/${platform}" "${OVE_ROOT}/include"
                "${OVE_ROOT}/backends/common")
            target_link_libraries(${target} PRIVATE pthread)
            if(platform STREQUAL "posix" AND (features STREQUAL "async" OR features STREQUAL "both"))
                target_link_options(${target} PRIVATE -Wl,--wrap=pthread_mutex_lock
                    -Wl,--wrap=pthread_mutex_unlock)
            endif()
            instrument(${target})
        endforeach()
    endforeach()
    set(timer_dir "${CMAKE_BINARY_DIR}/configs/context_timer")
    file(MAKE_DIRECTORY "${timer_dir}/ove")
    set(timer_config "#pragma once\n#define CONFIG_OVE_RTOS_POSIX 1\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_TIMER 1\n")
    if(OVE_ZERO_HEAP)
        string(APPEND timer_config "#define CONFIG_OVE_ZERO_HEAP 1\n")
    endif()
    file(WRITE "${timer_dir}/ove_config.h" "${timer_config}")
    configure_file("${OVE_IRQ_HEADER}" "${timer_dir}/ove/irq.h" COPYONLY)
    add_executable(context_timer "${OVE_CONTEXT_TIMER_SOURCE}" "${OVE_POSIX_IRQ_SOURCE}" "${OVE_POSIX_TIMER_SOURCE}")
    target_compile_definitions(context_timer PRIVATE _GNU_SOURCE)
    target_include_directories(context_timer PRIVATE "${timer_dir}" "${OVE_ROOT}/include"
        "${OVE_ROOT}/tests/host/posix/include")
    target_link_libraries(context_timer PRIVATE pthread)
    target_link_options(context_timer PRIVATE -Wl,--wrap=posix_irq_enter_isr
        -Wl,--wrap=posix_irq_leave_isr -Wl,--wrap=pthread_attr_setstacksize
        -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join)
    instrument(context_timer)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/ove_config.h" "${config}")
add_executable(outcome_posix "${OVE_CONDVAR_OUTCOME_SOURCE}")
target_compile_definitions(outcome_posix PRIVATE _GNU_SOURCE
    OVE_SYNC_BACKEND_SOURCE="${OVE_POSIX_SYNC_SOURCE}")
if(OVE_OUTCOME_LEGACY)
    target_compile_definitions(outcome_posix PRIVATE OVE_TEST_LEGACY_OUTCOME=1)
endif()
target_include_directories(outcome_posix PRIVATE "${CMAKE_BINARY_DIR}"
    "${OVE_ROOT}/include" "${OVE_ROOT}/tests/host/posix/include" "${OVE_ROOT}/backends/common")
target_compile_options(outcome_posix PRIVATE -ffunction-sections -fdata-sections)
target_link_options(outcome_posix PRIVATE -Wl,--gc-sections)
target_link_libraries(outcome_posix PRIVATE pthread)
instrument(outcome_posix)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/configs/outcome_disabled/ove")
configure_file("${OVE_DISABLED_SYNC_HEADER}"
    "${CMAKE_BINARY_DIR}/configs/outcome_disabled/ove/sync.h" COPYONLY)
file(WRITE "${CMAKE_BINARY_DIR}/configs/outcome_disabled/ove_config.h"
    "#pragma once\n#define CONFIG_OVE_RTOS_POSIX 1\n")
add_executable(outcome_disabled condvar_disabled.c)
target_include_directories(outcome_disabled PRIVATE "${CMAKE_BINARY_DIR}/configs/outcome_disabled"
    "${OVE_ROOT}/include" "${OVE_ROOT}/tests/host/posix/include")
instrument(outcome_disabled)
set(posix_kinds posix)
if(NOT OVE_ZERO_HEAP)
    list(APPEND posix_kinds posix_heap)
endif()
foreach(kind IN LISTS posix_kinds)
    add_executable(contracts_${kind} ${kind}.c)
    target_compile_definitions(contracts_${kind} PRIVATE _GNU_SOURCE
        OVE_POSIX_SYNC_SOURCE="${OVE_POSIX_SYNC_SOURCE}"
        OVE_COMMON_SYNC_SOURCE="${OVE_COMMON_SYNC_SOURCE}")
    target_include_directories(contracts_${kind} PRIVATE "${CMAKE_BINARY_DIR}"
        "${OVE_ROOT}/include" "${OVE_ROOT}/tests/host/posix/include"
        "${OVE_ROOT}/backends/common")
    target_compile_options(contracts_${kind} PRIVATE -Wall -Wextra -Werror
        -Wno-unused-parameter -O0 -g3)
    target_link_libraries(contracts_${kind} PRIVATE pthread)
    if(OVE_SANITIZE)
        target_compile_options(contracts_${kind} PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(contracts_${kind} PRIVATE -fsanitize=address,undefined
            -fno-sanitize-recover=all)
    endif()
endforeach()
