# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

set(APP_SOURCE "${OVE_ROOT}/apps/c/linux_interop/src/app.c")
set(QUALIFICATION_SOURCE
    "${OVE_ROOT}/apps/c/linux_interop/src/qualification.c")
set(RT_SCOPE_SOURCE "${OVE_ROOT}/apps/c/linux_interop/src/rt_scope.c")
set(MODULE_CONFIG "${OVE_ROOT}/config/Config.in.modules")

# Refactoring baseline captured on 2026-08-21. The STM32 full-profile images at
# this boundary were 315108 B (FreeRTOS), 356252 B (NuttX), and 337780 B
# (Zephyr). Those binary sizes are informational because backend/toolchain
# changes legitimately move them; the source ceilings below are enforced.
# Tighten the ceilings as demo, qualification, and board policy are separated.
set(APP_SOURCE_LINE_CEILING 600)
set(QUALIFICATION_SOURCE_LINE_CEILING 540)
set(RT_SCOPE_SOURCE_LINE_CEILING 1072)

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
assert_line_ceiling("${QUALIFICATION_SOURCE}"
                    ${QUALIFICATION_SOURCE_LINE_CEILING})
assert_line_ceiling("${RT_SCOPE_SOURCE}" ${RT_SCOPE_SOURCE_LINE_CEILING})

file(READ "${APP_SOURCE}" APP_TEXT)
file(READ "${QUALIFICATION_SOURCE}" QUALIFICATION_TEXT)
set(HOST_APP_TEXT "${APP_TEXT}\n${QUALIFICATION_TEXT}")

if(HOST_APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"](FreeRTOS\\.h|task\\.h|semphr\\.h|nuttx/|zephyr/)")
    message(FATAL_ERROR
        "linux_interop host code must use only engine-neutral oveRTOS APIs")
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
if(NOT APP_TEXT MATCHES
   "GUEST_ENTRYPOINT[ \t]+\"/usr/libexec/ove-interop-guest\"")
    message(FATAL_ERROR
        "linux_interop app must launch the stable rootfs guest entrypoint")
endif()

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
    if(PROFILE_TEXT MATCHES "CONFIG_OVE_(ARENA|LOADER|QUEUE)")
        message(FATAL_ERROR
            "${PROFILE_CONFIG} restates module plumbing selected elsewhere")
    endif()
endforeach()

message(STATUS "linux_interop ergonomics baseline is intact")
