# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

set(APP_SOURCE "${OVE_ROOT}/apps/c/linux_interop/src/app.c")
set(NETWORK_SMOKE_SOURCE
    "${OVE_ROOT}/apps/c/linux_interop/src/network_smoke.c")
set(QUALIFICATION_SOURCE
    "${OVE_ROOT}/apps/c/linux_interop/src/qualification.c")
set(ROUNDTRIP_SOURCE "${OVE_ROOT}/apps/c/linux_interop/src/roundtrip.c")
set(RT_SCOPE_SOURCE
    "${OVE_ROOT}/backends/common/lxp_ove_rt_scope.c")
set(BOARD_RT_SCOPE_SOURCE
    "${OVE_ROOT}/boards/stm32f746g-discovery/common/rt_scope.c")
set(FULL_PROFILE_CONFIG "${OVE_ROOT}/apps/c/linux_interop/app.yaml")
set(MODULE_CONFIG "${OVE_ROOT}/config/Config.in.modules")

# Refactoring baseline captured on 2026-08-21. The STM32 full-profile images at
# this boundary were 315108 B (FreeRTOS), 356252 B (NuttX), and 337780 B
# (Zephyr). Those binary sizes are informational because backend/toolchain
# changes legitimately move them; the source ceilings below are enforced.
# Tighten the ceilings as demo, qualification, and board policy are separated.
set(APP_SOURCE_LINE_CEILING 132)
set(NETWORK_SMOKE_SOURCE_LINE_CEILING 110)
set(QUALIFICATION_SOURCE_LINE_CEILING 407)
set(ROUNDTRIP_SOURCE_LINE_CEILING 136)
set(RT_SCOPE_SOURCE_LINE_CEILING 815)
set(BOARD_RT_SCOPE_SOURCE_LINE_CEILING 300)
set(FULL_PROFILE_CONFIG_LINE_CEILING 55)

function(assert_line_ceiling PATH CEILING)
    file(READ "${PATH}" TEXT)
    string(REGEX MATCHALL "\n" LINE_BREAKS "${TEXT}")
    list(LENGTH LINE_BREAKS LINE_COUNT)
    if(NOT TEXT STREQUAL "" AND NOT TEXT MATCHES "\n$")
        math(EXPR LINE_COUNT "${LINE_COUNT} + 1")
    endif()
    if(LINE_COUNT GREATER CEILING)
        message(FATAL_ERROR
            "${PATH} grew to ${LINE_COUNT} lines; refactoring ceiling is ${CEILING}")
    endif()
endfunction()

assert_line_ceiling("${APP_SOURCE}" ${APP_SOURCE_LINE_CEILING})
assert_line_ceiling("${NETWORK_SMOKE_SOURCE}"
                    ${NETWORK_SMOKE_SOURCE_LINE_CEILING})
assert_line_ceiling("${QUALIFICATION_SOURCE}"
                    ${QUALIFICATION_SOURCE_LINE_CEILING})
assert_line_ceiling("${ROUNDTRIP_SOURCE}" ${ROUNDTRIP_SOURCE_LINE_CEILING})
assert_line_ceiling("${RT_SCOPE_SOURCE}" ${RT_SCOPE_SOURCE_LINE_CEILING})
assert_line_ceiling("${BOARD_RT_SCOPE_SOURCE}"
                    ${BOARD_RT_SCOPE_SOURCE_LINE_CEILING})
assert_line_ceiling("${FULL_PROFILE_CONFIG}"
                    ${FULL_PROFILE_CONFIG_LINE_CEILING})

file(READ "${APP_SOURCE}" APP_TEXT)
file(READ "${NETWORK_SMOKE_SOURCE}" NETWORK_SMOKE_TEXT)
file(READ "${QUALIFICATION_SOURCE}" QUALIFICATION_TEXT)
file(READ "${ROUNDTRIP_SOURCE}" ROUNDTRIP_TEXT)
file(READ "${RT_SCOPE_SOURCE}" RT_SCOPE_TEXT)
file(READ "${BOARD_RT_SCOPE_SOURCE}" BOARD_RT_SCOPE_TEXT)
set(HOST_APP_TEXT
    "${APP_TEXT}\n${NETWORK_SMOKE_TEXT}\n${QUALIFICATION_TEXT}\n${ROUNDTRIP_TEXT}")
set(ENGINE_NEUTRAL_SCOPE_TEXT "${RT_SCOPE_TEXT}")

if(HOST_APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"](FreeRTOS\\.h|task\\.h|semphr\\.h|nuttx/|zephyr/)")
    message(FATAL_ERROR
        "linux_interop host code must use only engine-neutral oveRTOS APIs")
endif()

if(ENGINE_NEUTRAL_SCOPE_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"](FreeRTOS\\.h|task\\.h|semphr\\.h|nuttx/|zephyr/)")
    message(FATAL_ERROR
        "common LXP RT-scope service must use only engine-neutral oveRTOS APIs")
endif()

if(HOST_APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"]lxp/|(^|[^A-Za-z0-9_])lxp_(run|host|dev|proc|syscall)[A-Za-z0-9_]*[ \t\r\n]*\\(")
    message(FATAL_ERROR
        "linux_interop host code bypasses the public oveRTOS LXP facade")
endif()

if(APP_TEXT MATCHES
   "loader_rootfs_image\\.h|ove/lxp_memory_layout\\.h|ove_lxp_host_init_cpio[ \t\r\n]*\\(")
    message(FATAL_ERROR
        "linux_interop app must use the configuration-driven LXP host bootstrap")
endif()

if(APP_TEXT MATCHES "/bin/busybox|/usr/bin/fpcheck")
    message(FATAL_ERROR
        "linux_interop app regained rootfs-owned guest executable policy")
endif()
if(APP_TEXT MATCHES "rc2[ \t]*>=[ \t]*0" OR
   ROUNDTRIP_TEXT MATCHES "guest_status[ \t]*>=[ \t]*0")
    message(FATAL_ERROR
        "linux_interop must not treat nonzero guest exit statuses as success")
endif()
if(NOT APP_TEXT MATCHES
   "GUEST_ENTRYPOINT[ \t]+\"/usr/libexec/ove-interop-guest\"")
    message(FATAL_ERROR
        "linux_interop app must launch the stable rootfs guest entrypoint")
endif()
foreach(HOST_LIFECYCLE_CALL IN ITEMS init run observe deinit)
    if(NOT APP_TEXT MATCHES
       "ove_lxp_host_${HOST_LIFECYCLE_CALL}[ \t\r\n]*[(]")
        message(FATAL_ERROR
            "linux_interop app must keep the explicit host ${HOST_LIFECYCLE_CALL} boundary")
    endif()
endforeach()
if(APP_TEXT MATCHES
   "static[ \t]+(const[ \t]+char[ \t]*[*]|void)[ \t]+(exit_reason_name|on_enosys|on_guest_exit)")
    message(FATAL_ERROR
        "linux_interop app regained generic console-diagnostic plumbing")
endif()
if(NOT APP_TEXT MATCHES "ove_lxp_console_bind_diagnostics[ \t\r\n]*[(]")
    message(FATAL_ERROR
        "linux_interop app must use the reusable console diagnostics binding")
endif()
if(APP_TEXT MATCHES
   "static[ \t]+[^\r\n]*(feed_read|consume_write|roundtrip_worker|network_transport_smoke)[ \t]*[(]")
    message(FATAL_ERROR
        "linux_interop app regained application-owned workload implementation")
endif()
if(ROUNDTRIP_TEXT MATCHES
   "static[ \t]+volatile[ \t]+int[ \t]+g_(feed_ready|linux_done|worker_exited|round_trip_n)")
    message(FATAL_ERROR
        "round-trip scenario regained ad-hoc volatile thread handshakes")
endif()
if(NOT ROUNDTRIP_TEXT MATCHES "ove_thread_request_stop[ \t\r\n]*[(]")
    message(FATAL_ERROR
        "round-trip scenario must use the cooperative thread-stop contract")
endif()
if(QUALIFICATION_TEXT MATCHES
   "static[ \t]+volatile[ \t]+int[ \t]+g_mon_(stop|exited)")
    message(FATAL_ERROR
        "latency qualification regained ad-hoc volatile thread handshakes")
endif()
if(NOT QUALIFICATION_TEXT MATCHES "ove_thread_request_stop[ \t\r\n]*[(]")
    message(FATAL_ERROR
        "latency qualification must use the cooperative thread-stop contract")
endif()
if(APP_TEXT MATCHES "linux_interop_roundtrip_(worker|worker_stack_size)[ \t\r\n]*[(]")
    message(FATAL_ERROR
        "linux_interop app must not inspect scenario-owned RTOS resources")
endif()
string(FIND "${ROUNDTRIP_TEXT}"
    "linux_interop_qualification_observe_thread(\"worker\""
    ROUNDTRIP_OBSERVE_POSITION)
string(FIND "${ROUNDTRIP_TEXT}" "ove_thread_deinit(g_worker)"
    ROUNDTRIP_DEINIT_POSITION)
if(ROUNDTRIP_OBSERVE_POSITION EQUAL -1 OR ROUNDTRIP_DEINIT_POSITION EQUAL -1 OR
   ROUNDTRIP_OBSERVE_POSITION GREATER ROUNDTRIP_DEINIT_POSITION)
    message(FATAL_ERROR
        "round-trip worker stack usage must be captured before thread teardown")
endif()
string(FIND "${QUALIFICATION_TEXT}"
    "linux_interop_qualification_observe_thread(\"lat-monitor\""
    LATENCY_OBSERVE_POSITION)
string(FIND "${QUALIFICATION_TEXT}" "ove_thread_deinit(g_mon)"
    LATENCY_DEINIT_POSITION)
if(LATENCY_OBSERVE_POSITION EQUAL -1 OR LATENCY_DEINIT_POSITION EQUAL -1 OR
   LATENCY_OBSERVE_POSITION GREATER LATENCY_DEINIT_POSITION)
    message(FATAL_ERROR
        "latency monitor stack usage must be captured before thread teardown")
endif()
foreach(SCENARIO_CALL IN ITEMS
        linux_interop_roundtrip_prepare
        linux_interop_roundtrip_complete
        linux_interop_network_report
        linux_interop_network_smoke)
    if(NOT APP_TEXT MATCHES "${SCENARIO_CALL}[ \t\r\n]*[(]")
        message(FATAL_ERROR
            "linux_interop app omits scenario module boundary: ${SCENARIO_CALL}")
    endif()
endforeach()

# Board registers and platform termination belong behind oveRTOS APIs.
string(REGEX MATCHALL
    "\\*\\(volatile[ ]+unsigned[ ]+int[ ]*\\*\\)0x[0-9A-Fa-f]+[uU]?"
    TARGET_REGISTER_WRITES "${APP_TEXT}")
list(LENGTH TARGET_REGISTER_WRITES TARGET_REGISTER_WRITE_COUNT)
if(NOT TARGET_REGISTER_WRITE_COUNT EQUAL 0)
    message(FATAL_ERROR
        "linux_interop app must not access target registers directly")
endif()

if(HOST_APP_TEXT MATCHES "CONFIG_OVE_RTOS_(FREERTOS|NUTTX|ZEPHYR)")
    message(FATAL_ERROR
        "linux_interop host lifecycle must not branch by RTOS engine")
endif()

if(HOST_APP_TEXT MATCHES
   "(^|[^A-Za-z0-9_])(REG32|NVIC_[A-Za-z0-9_]*|IRQ_CONNECT|irq_attach|up_(enable|disable)_irq|TIM[0-9]+_IRQHandler)[ \t\r\n(]" OR
   HOST_APP_TEXT MATCHES "0x400[0-9A-Fa-f]{5}")
    message(FATAL_ERROR
        "linux_interop host code regained board register or IRQ ownership")
endif()
foreach(BOARD_SCOPE_CALL IN ITEMS
        ove_hal_rt_scope_irq_prepare
        ove_hal_rt_scope_hardware_prepare
        ove_hal_rt_scope_worker_stack)
    if(NOT BOARD_RT_SCOPE_TEXT MATCHES "${BOARD_SCOPE_CALL}[ \t\r\n]*[(]")
        message(FATAL_ERROR
            "STM32 RT-scope provider omits board seam: ${BOARD_SCOPE_CALL}")
    endif()
endforeach()

if(HOST_APP_TEXT MATCHES "static[ \t]+uint8_t[ \t]+g_[A-Za-z0-9_]*stack")
    message(FATAL_ERROR
        "linux_interop host thread stacks must use oveRTOS storage helpers")
endif()

if(APP_TEXT MATCHES
   "CONFIG_OVE_WATCHDOG|CONFIG_OVE_LINUX_(FAULTTEST|SMASHTEST|LATENCY)|ove_watchdog_|ove_reset_cause")
    message(FATAL_ERROR
        "linux_interop app regained qualification-only implementation policy")
endif()

# Linux personality owns its required modules.  Applications select the
# personality and optional features rather than restating transitive plumbing.
file(READ "${MODULE_CONFIG}" MODULE_CONFIG_TEXT)
string(FIND "${MODULE_CONFIG_TEXT}" "config OVE_LINUX\n" LINUX_CONFIG_START)
if(LINUX_CONFIG_START EQUAL -1)
    message(FATAL_ERROR "config OVE_LINUX is missing")
endif()
string(SUBSTRING "${MODULE_CONFIG_TEXT}" ${LINUX_CONFIG_START} -1 LINUX_CONFIG_REST)
string(FIND "${LINUX_CONFIG_REST}" "\nconfig " LINUX_CONFIG_END)
if(LINUX_CONFIG_END EQUAL -1)
    set(LINUX_CONFIG_BLOCK "${LINUX_CONFIG_REST}")
else()
    string(SUBSTRING "${LINUX_CONFIG_REST}" 0 ${LINUX_CONFIG_END} LINUX_CONFIG_BLOCK)
endif()
if(NOT LINUX_CONFIG_BLOCK MATCHES "(^|\n)[ \t]+select OVE_ARENA(\n|$)")
    message(FATAL_ERROR "OVE_LINUX must select its arena dependency")
endif()
if(NOT LINUX_CONFIG_BLOCK MATCHES "(^|\n)[ \t]+select OVE_LOADER(\n|$)")
    message(FATAL_ERROR "OVE_LINUX must select its loader dependency")
endif()

foreach(PROFILE IN ITEMS
        linux_interop
        linux_interop_diagnostic
        linux_interop_hardened
        linux_interop_minimal)
    set(PROFILE_CONFIG "${OVE_ROOT}/apps/c/${PROFILE}/app.yaml")
    file(READ "${PROFILE_CONFIG}" PROFILE_TEXT)
    if(NOT PROFILE_TEXT MATCHES "qualification\\.c")
        message(FATAL_ERROR
            "${PROFILE_CONFIG} does not include the shared qualification module")
    endif()
    foreach(SCENARIO_SOURCE IN ITEMS "network_smoke\\.c" "roundtrip\\.c")
        if(NOT PROFILE_TEXT MATCHES "${SCENARIO_SOURCE}")
            message(FATAL_ERROR
                "${PROFILE_CONFIG} omits shared scenario source ${SCENARIO_SOURCE}")
        endif()
    endforeach()
    if(PROFILE_TEXT MATCHES "CONFIG_OVE_(ARENA|LOADER|QUEUE|FS)(=|_MAX_OPEN_FILES)")
        message(FATAL_ERROR
            "${PROFILE_CONFIG} restates module plumbing selected elsewhere")
    endif()
endforeach()

message(STATUS "linux_interop ergonomics baseline is intact")
