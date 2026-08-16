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

set(LEGACY_SEAMS "")
set(HOST_ADAPTERS
    "backends/common/lxp_ove_adapter.c"
    "backends/common/lxp_ove_disp_adapter.c"
    "backends/common/lxp_ove_fs_adapter.c"
    "backends/common/lxp_ove_host.c"
    "backends/common/lxp_ove_thread_adapter.c"
    "backends/freertos/freertos_lxp_host.c"
    "backends/nuttx/nuttx_lxp_host.c"
    "backends/zephyr/zephyr_lxp_host.c")

set(LXP_RTOS_PORTS
    "modules/lxp/ports/freertos/lxp_freertos_port.c"
    "modules/lxp/ports/nuttx/lxp_nuttx_port.c"
    "modules/lxp/ports/zephyr/lxp_zephyr_port.c")

set(LXP_ARCH_HEADERS
    "modules/lxp/include/lxp/arch/cortex_m_cache.h"
    "modules/lxp/include/lxp/arch/cortex_m_memory.h"
    "modules/lxp/include/lxp/arch/cortex_m_mpu.h")
foreach(HEADER IN LISTS LXP_ARCH_HEADERS)
    if(NOT EXISTS "${OVE_ROOT}/${HEADER}")
        message(FATAL_ERROR "LXP-owned architecture header is missing: ${HEADER}")
    endif()
endforeach()
foreach(HOST_FACADE IN ITEMS
        "modules/lxp/include/lxp/lxp_host.h"
        "modules/lxp/src/lxp_host.c")
    if(NOT EXISTS "${OVE_ROOT}/${HOST_FACADE}")
        message(FATAL_ERROR "LXP-owned host facade is missing: ${HOST_FACADE}")
    endif()
endforeach()
foreach(RETIRED_HEADER IN ITEMS
        "backends/common/ove_cortex_m_cache.h"
        "backends/common/ove_cortex_m_mpu.h")
    if(EXISTS "${OVE_ROOT}/${RETIRED_HEADER}")
        message(FATAL_ERROR "retired oveRTOS architecture duplicate remains: ${RETIRED_HEADER}")
    endif()
endforeach()

set(BUILD_TEMPLATE "${OVE_ROOT}/config/templates/ove_config.cmake.j2")
file(READ "${BUILD_TEMPLATE}" BUILD_TEXT)
foreach(SOURCE IN LISTS LEGACY_SEAMS HOST_ADAPTERS LXP_RTOS_PORTS)
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

if(EXISTS "${OVE_ROOT}/backends/freertos/freertos_lnx.c")
    message(FATAL_ERROR "retired consumer-owned FreeRTOS seam remains")
endif()
if(EXISTS "${OVE_ROOT}/backends/nuttx/nuttx_lnx_trap.c")
    message(FATAL_ERROR "retired consumer-owned NuttX seam remains")
endif()
if(EXISTS "${OVE_ROOT}/backends/zephyr/zephyr_lnx.c")
    message(FATAL_ERROR "retired consumer-owned Zephyr seam remains")
endif()
file(READ "${OVE_ROOT}/modules/lxp/ports/freertos/lxp_freertos_port.c"
    FREERTOS_PORT_TEXT)
if(FREERTOS_PORT_TEXT MATCHES "CONFIG_OVE_|#[ \t]*include[ \t]*[<\"]ove/")
    message(FATAL_ERROR "LXP FreeRTOS port regained oveRTOS coupling")
endif()
file(READ "${OVE_ROOT}/modules/lxp/ports/nuttx/lxp_nuttx_port.c"
    NUTTX_PORT_TEXT)
if(NUTTX_PORT_TEXT MATCHES "CONFIG_OVE_|#[ \t]*include[ \t]*[<\"]ove/")
    message(FATAL_ERROR "LXP NuttX port regained oveRTOS coupling")
endif()
file(READ "${OVE_ROOT}/modules/lxp/ports/zephyr/lxp_zephyr_port.c"
    ZEPHYR_PORT_TEXT)
if(ZEPHYR_PORT_TEXT MATCHES "CONFIG_OVE_|#[ \t]*include[ \t]*[<\"]ove/")
    message(FATAL_ERROR "LXP Zephyr port regained oveRTOS coupling")
endif()
file(READ "${OVE_ROOT}/modules/lxp/ports/qemu-mps2/engine.c"
    FREERTOS_FIXTURE_TEXT)
if(FREERTOS_FIXTURE_TEXT MATCHES
   "SVC_Handler|MemManage_Handler|xTaskCreateRestrictedStatic")
    message(FATAL_ERROR
        "standalone FreeRTOS fixture duplicated task/trap machinery from the production port")
endif()

# Consumer-owned task/trap code may not return in a backend file. All three
# production RTOS engines are represented by LXP-owned ports above.
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
if(APP_TEXT MATCHES
   "lxp_cpio_to_rootfs|lxp_run_config_t|ove_lxp_prepare_rootfs_access|ove_lxp_run\\(")
    message(FATAL_ERROR
        "app.c regained LXP-owned rootfs bootstrap or provider/run composition")
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
