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
    "backends/common/lxp_ove_observability.c"
    "backends/common/lxp_ove_console.c"
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
        "modules/lxp/include/lxp/lxp_async_gate.h"
        "modules/lxp/src/lxp_async_gate.c"
        "modules/lxp/include/lxp/lxp_host.h"
        "modules/lxp/src/lxp_host.c")
    if(NOT EXISTS "${OVE_ROOT}/${HOST_FACADE}")
        message(FATAL_ERROR "LXP-owned host facade is missing: ${HOST_FACADE}")
    endif()
endforeach()

file(READ "${OVE_ROOT}/backends/common/lxp_ove_fs_adapter.c" STORAGE_ADAPTER_TEXT)
if(STORAGE_ADAPTER_TEXT MATCHES
   "g_async_(state|owner|cancelled)|g_selected_owner|g_raw_(readers|writer)")
    message(FATAL_ERROR
        "storage adapter regained LXP-owned async or Linux raw-open state")
endif()
if(STORAGE_ADAPTER_TEXT MATCHES "lxp_(fs|block)_kick")
    message(FATAL_ERROR
        "storage adapter regained a process-global LXP completion dependency")
endif()
foreach(STORAGE_READY_TYPE IN ITEMS "lxp_fs_ready_fn" "lxp_block_ready_fn")
    if(NOT STORAGE_ADAPTER_TEXT MATCHES "${STORAGE_READY_TYPE}")
        message(FATAL_ERROR
            "storage adapter omits run-scoped readiness ownership: ${STORAGE_READY_TYPE}")
    endif()
endforeach()
if(NOT STORAGE_ADAPTER_TEXT MATCHES "lxp_async_gate")
    message(FATAL_ERROR "storage adapter bypasses the LXP asynchronous provider gate")
endif()
foreach(STORAGE_HEADER IN ITEMS
        "modules/lxp/include/lxp/lxp_fs_ops.h"
        "modules/lxp/include/lxp/lxp_block_ops.h")
    file(READ "${OVE_ROOT}/${STORAGE_HEADER}" STORAGE_HEADER_TEXT)
    if(STORAGE_HEADER_TEXT MATCHES "lxp_(fs|block)_kick")
        message(FATAL_ERROR
            "canonical storage contract regained a process-global completion symbol: ${STORAGE_HEADER}")
    endif()
endforeach()
if(NOT STORAGE_ADAPTER_TEXT MATCHES
   "CONFIG_OVE_FS_MAX_OPEN_FILES[ \t]*>=[ \t]*LXP_NHOSTFS_OPEN")
    message(FATAL_ERROR
        "storage adapter does not bind native capacity to LXP's descriptor table")
endif()
foreach(NATIVE_FS_BACKEND IN ITEMS
        "backends/freertos/freertos_fs.c"
        "backends/nuttx/nuttx_fs.c"
        "backends/zephyr/zephyr_fs.c")
    file(READ "${OVE_ROOT}/${NATIVE_FS_BACKEND}" NATIVE_FS_TEXT)
    if(NATIVE_FS_TEXT MATCHES
       "#[ \t]*include[ \t]*[<\"]lxp/|(^|[^A-Za-z0-9_])LXP_[A-Z0-9_]+")
        message(FATAL_ERROR
            "generic filesystem backend regained LXP coupling: ${NATIVE_FS_BACKEND}")
    endif()
endforeach()
file(READ "${OVE_ROOT}/apps/c/linux_interop/app.yaml" LINUX_APP_CONFIG_TEXT)
if(NOT LINUX_APP_CONFIG_TEXT MATCHES "CONFIG_OVE_FS_MAX_OPEN_FILES=16")
    message(FATAL_ERROR
        "linux_interop must provision all sixteen LXP host filesystem descriptors")
endif()
file(READ "${OVE_ROOT}/config/templates/prj.conf.j2" ZEPHYR_CONFIG_TEMPLATE_TEXT)
if(NOT ZEPHYR_CONFIG_TEMPLATE_TEXT MATCHES
   "CONFIG_FS_FATFS_NUM_FILES=\\{\\{ config[.]get\\(\"CONFIG_OVE_FS_MAX_OPEN_FILES\"")
    message(FATAL_ERROR
        "Zephyr FatFs file pool bypasses the OVE filesystem capacity")
endif()
file(READ
    "${OVE_ROOT}/boards/stm32f746g-discovery/freertos/inc/ffconf.h"
    FREERTOS_FATFS_CONFIG_TEXT)
if(NOT FREERTOS_FATFS_CONFIG_TEXT MATCHES
   "#define[ \t]+_FS_LOCK[ \t]+CONFIG_OVE_FS_MAX_OPEN_FILES")
    message(FATAL_ERROR
        "FreeRTOS FatFs lock table bypasses the OVE filesystem capacity")
endif()
file(READ "${OVE_ROOT}/backends/common/lxp_ove_adapter.c" NETWORK_ADAPTER_TEXT)
if(NETWORK_ADAPTER_TEXT MATCHES "lxp_sock_kick|#[ \t]*include[ \t]*[<\"]lxp/lxp_net\\.h")
    message(FATAL_ERROR
        "network adapter regained an implicit dependency on the LXP socket core")
endif()
if(NOT NETWORK_ADAPTER_TEXT MATCHES "lxp_net_ready_fn")
    message(FATAL_ERROR
        "network adapter bypasses the run-scoped LXP readiness callback")
endif()
file(READ "${OVE_ROOT}/backends/common/lxp_ove_disp_adapter.c" DISPLAY_ADAPTER_TEXT)
if(DISPLAY_ADAPTER_TEXT MATCHES "lxp_input_report_touch|lxp_dev_tick")
    message(FATAL_ERROR
        "display adapter regained LXP-owned input or tick scheduling")
endif()
foreach(DISPLAY_LIFECYCLE IN ITEMS
        "dma2d_init = d_dma2d_init"
        "touch_deinit = d_touch_deinit")
    if(NOT DISPLAY_ADAPTER_TEXT MATCHES "${DISPLAY_LIFECYCLE}")
        message(FATAL_ERROR
            "display adapter omits provider lifecycle binding: ${DISPLAY_LIFECYCLE}")
    endif()
endforeach()
file(READ "${OVE_ROOT}/backends/freertos/freertos_hooks.c" FREERTOS_HOOKS_TEXT)
if(FREERTOS_HOOKS_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"]lxp/|(^|[^A-Za-z0-9_])lxp_[A-Za-z0-9_]+")
    message(FATAL_ERROR "generic FreeRTOS hooks regained a direct LXP dependency")
endif()
if(NOT FREERTOS_HOOKS_TEXT MATCHES
   "ove_freertos_tick_callback_t callback = g_tick_callback")
    message(FATAL_ERROR "generic FreeRTOS tick hook bypasses its owned subscriber seam")
endif()
file(READ "${OVE_ROOT}/backends/freertos/freertos_lxp_host.c" FREERTOS_HOST_TEXT)
foreach(TICK_BINDING IN ITEMS
        "tick_subscribe = ove_freertos_tick_subscribe"
        "tick_unsubscribe = ove_freertos_tick_unsubscribe")
    if(NOT FREERTOS_HOST_TEXT MATCHES "${TICK_BINDING}")
        message(FATAL_ERROR "FreeRTOS LXP host omits run-scoped ${TICK_BINDING}")
    endif()
endforeach()
file(READ "${OVE_ROOT}/apps/c/linux_interop/src/app.c" APP_TEXT)
if(APP_TEXT MATCHES
   "g_uart_lookahead|uart_rx_ready|OVE_UART_REG|static long console_(read|write)|static int console_poll|ove_console_(try_getchar|putchar|write)")
    message(FATAL_ERROR
        "linux_interop app regained oveRTOS-owned console transport mechanics")
endif()
file(READ "${OVE_ROOT}/backends/zephyr/zephyr_console.c" ZEPHYR_CONSOLE_TEXT)
if(ZEPHYR_CONSOLE_TEXT MATCHES "lxp_console_kick|#[ \t]*include[ \t]*[<\"]lxp/")
    message(FATAL_ERROR
        "Zephyr console regained an implicit dependency on the LXP core")
endif()
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
file(READ "${OVE_ROOT}/apps/c/linux_interop/src/rt_scope.c" RT_SCOPE_TEXT)
file(READ "${OVE_ROOT}/include/ove/lxp_host.h" OVE_LXP_HOST_HEADER_TEXT)
file(READ "${OVE_ROOT}/include/ove/lxp_observability.h" OVE_LXP_OBSERVABILITY_HEADER_TEXT)
file(READ "${OVE_ROOT}/include/ove/thread.h" OVE_THREAD_HEADER_TEXT)
if(APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"](FreeRTOS\\.h|task\\.h|semphr\\.h|nuttx/|zephyr/)")
    message(FATAL_ERROR "app.c must not include native RTOS headers")
endif()
if(APP_TEXT MATCHES
   "lxp_cpio_to_rootfs|lxp_run_config_t|ove_lxp_prepare_rootfs_access|ove_lxp_run\\(")
    message(FATAL_ERROR
        "app.c regained LXP-owned rootfs bootstrap or provider/run composition")
endif()
if(APP_TEXT MATCHES
   "lxp_file_t|ROOTFS_(MAX_FILES|NAME_BYTES)|rootfs_(storage|capacity|name_storage|name_capacity)")
    message(FATAL_ERROR
        "app.c regained rootfs workspace allocation or capacity knowledge")
endif()
if(OVE_LXP_HOST_HEADER_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"]lxp/|(^|[^A-Za-z0-9_])lxp_(file|host)_t|rootfs_(files|names)|[ \t]core;")
    message(FATAL_ERROR
        "public ove_lxp_host_t exposes canonical LXP host storage representation")
endif()
if(APP_TEXT MATCHES "g_linux_host[.]_opaque|g_linux_host[.]_alignment")
    message(FATAL_ERROR "app.c inspects opaque ove_lxp_host_t storage")
endif()
if(OVE_LXP_OBSERVABILITY_HEADER_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"]lxp/|(^|[^A-Za-z0-9_])lxp_[A-Za-z0-9_]*_t|(^|[^A-Za-z0-9_])LXP_[A-Z0-9_]+")
    message(FATAL_ERROR
        "public OVE observability contract aliases canonical LXP representation")
endif()
if(OVE_THREAD_HEADER_TEXT MATCHES
   "(^|[^A-Za-z0-9_])(lxp_slot|LXP slot|Linux-personality slot)")
    message(FATAL_ERROR
        "generic ove_thread_info regained personality-specific ownership state")
endif()
if(APP_TEXT MATCHES
   "(^|[^A-Za-z0-9_])(lxp_launch_config_t|lxp_guest_exit_info_t|LXP_EXIT_REASON_[A-Z0-9_]*|LXP_RUN_E[A-Z0-9_]*)")
    message(FATAL_ERROR
        "app.c bypasses the oveRTOS launch and guest-exit contract")
endif()
if(RT_SCOPE_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"]lxp/|LXP_NR_|lxp_zephyr_critical_metrics|ove_freertos_lnx_metrics")
    message(FATAL_ERROR
        "rt_scope.c bypasses the oveRTOS diagnostics metrics contract")
endif()
if(APP_TEXT MATCHES
   "lxp_sock_set_netif|lxp_netfs_mount_config|#[ \t]*include[ \t]*[<\"]lxp/lxp_(net|netfs)\\.h|ove_netif_(init|up|down|deinit)\\(")
    message(FATAL_ERROR
        "app.c regained LXP topology globals or native network lifecycle ownership")
endif()
if(APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"]lxp/(lxp_(config|diag|latency)|ports/)|(^|[^A-Za-z0-9_])lxp_(run_health|diag_|lat_|freertos_slot_stack)")
    message(FATAL_ERROR
        "app.c regained direct LXP observability-registry or RTOS-port knowledge")
endif()
if(APP_TEXT MATCHES "ove_hal_(dma2d|fb)_")
    message(FATAL_ERROR
        "app.c regained generic display-provider initialization or access")
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
