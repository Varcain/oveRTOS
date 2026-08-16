# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# Transitional ownership ledger for the LXP port migration. A migration commit
# updates this inventory while removing the old consumer-owned implementation.

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

set(EXPECTED_APP_FILES
    "README.md"
    "app.yaml"
    "patches/freertos/0001-arm-cm4-mpu-drop-global-user-peripheral-map.patch"
    "src/app.c"
    "src/rt_scope.c"
    "src/rt_scope.h")
file(GLOB_RECURSE ACTUAL_APP_FILES
    RELATIVE "${OVE_ROOT}/apps/c/linux_interop"
    "${OVE_ROOT}/apps/c/linux_interop/*")
list(SORT EXPECTED_APP_FILES)
list(SORT ACTUAL_APP_FILES)
if(NOT ACTUAL_APP_FILES STREQUAL EXPECTED_APP_FILES)
    message(FATAL_ERROR
        "linux_interop inventory changed without updating the LXP migration ledger\n"
        "expected: ${EXPECTED_APP_FILES}\nactual: ${ACTUAL_APP_FILES}")
endif()

set(LEGACY_SEAMS
    "backends/freertos/freertos_lnx.c"
    "backends/nuttx/nuttx_lnx_trap.c"
    "backends/zephyr/zephyr_lnx.c")
set(HOST_ADAPTERS
    "backends/common/lxp_ove_adapter.c"
    "backends/common/lxp_ove_disp_adapter.c"
    "backends/common/lxp_ove_fs_adapter.c"
    "backends/common/lxp_ove_host.c"
    "backends/common/lxp_ove_thread_adapter.c")

set(BUILD_TEMPLATE "${OVE_ROOT}/config/templates/ove_config.cmake.j2")
file(READ "${BUILD_TEMPLATE}" BUILD_TEXT)
foreach(SOURCE IN LISTS LEGACY_SEAMS HOST_ADAPTERS)
    if(NOT EXISTS "${OVE_ROOT}/${SOURCE}")
        message(FATAL_ERROR "migration ledger names missing source: ${SOURCE}")
    endif()
    string(REGEX MATCHALL "${SOURCE}" SOURCE_REFERENCES "${BUILD_TEXT}")
    list(LENGTH SOURCE_REFERENCES SOURCE_REFERENCE_COUNT)
    if(NOT SOURCE_REFERENCE_COUNT EQUAL 1)
        message(FATAL_ERROR
            "${SOURCE} must occur exactly once in ${BUILD_TEMPLATE}; found ${SOURCE_REFERENCE_COUNT}")
    endif()
endforeach()

# General task/trap code may not spread to additional backend files unnoticed.
file(GLOB ACTUAL_LEGACY_SEAMS
    RELATIVE "${OVE_ROOT}"
    "${OVE_ROOT}/backends/freertos/*lnx*.c"
    "${OVE_ROOT}/backends/nuttx/*lnx*.c"
    "${OVE_ROOT}/backends/zephyr/*lnx*.c")
list(SORT LEGACY_SEAMS)
list(SORT ACTUAL_LEGACY_SEAMS)
if(NOT ACTUAL_LEGACY_SEAMS STREQUAL LEGACY_SEAMS)
    message(FATAL_ERROR
        "RTOS seam inventory changed without updating the LXP migration ledger\n"
        "expected: ${LEGACY_SEAMS}\nactual: ${ACTUAL_LEGACY_SEAMS}")
endif()

# app.c may compose host providers and policy during the migration, but direct
# native-RTOS headers are confined to the rt_scope benchmark exception.
file(READ "${OVE_ROOT}/apps/c/linux_interop/src/app.c" APP_TEXT)
if(APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"](FreeRTOS\\.h|task\\.h|semphr\\.h|nuttx/|zephyr/)")
    message(FATAL_ERROR "app.c must not include native RTOS headers")
endif()

execute_process(
    COMMAND sh "${OVE_ROOT}/modules/lxp/scripts/check-decoupled.sh"
    WORKING_DIRECTORY "${OVE_ROOT}/modules/lxp"
    RESULT_VARIABLE DECOUPLED_RESULT
    OUTPUT_VARIABLE DECOUPLED_OUTPUT
    ERROR_VARIABLE DECOUPLED_ERROR)
if(NOT DECOUPLED_RESULT EQUAL 0)
    message(FATAL_ERROR
        "LXP core ownership check failed:\n${DECOUPLED_OUTPUT}${DECOUPLED_ERROR}")
endif()

message(STATUS "LXP port ownership ledger is consistent")
