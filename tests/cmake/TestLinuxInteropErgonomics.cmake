# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

set(APP_SOURCE "${OVE_ROOT}/apps/c/linux_interop/src/app.c")
set(RT_SCOPE_SOURCE "${OVE_ROOT}/apps/c/linux_interop/src/rt_scope.c")

# Refactoring baseline captured on 2026-08-21. The STM32 full-profile images at
# this boundary were 315108 B (FreeRTOS), 356252 B (NuttX), and 337780 B
# (Zephyr). Those binary sizes are informational because backend/toolchain
# changes legitimately move them; the source ceilings below are enforced.
# Tighten the ceilings as demo, qualification, and board policy are separated.
set(APP_SOURCE_LINE_CEILING 1140)
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
assert_line_ceiling("${RT_SCOPE_SOURCE}" ${RT_SCOPE_SOURCE_LINE_CEILING})

file(READ "${APP_SOURCE}" APP_TEXT)

if(APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"](FreeRTOS\\.h|task\\.h|semphr\\.h|nuttx/|zephyr/)")
    message(FATAL_ERROR "linux_interop app must use only engine-neutral oveRTOS APIs")
endif()

if(APP_TEXT MATCHES
   "#[ \t]*include[ \t]*[<\"]lxp/|(^|[^A-Za-z0-9_])lxp_(run|host|dev|proc|syscall)[A-Za-z0-9_]*[ \t\r\n]*\\(")
    message(FATAL_ERROR "linux_interop app bypasses the public oveRTOS LXP facade")
endif()

# One direct system-reset register write is known debt and is frozen here until
# the system-exit iteration moves it behind a board API. No second target-register
# access may enter the example in the meantime.
string(REGEX MATCHALL
    "\\*\\(volatile[ ]+unsigned[ ]+int[ ]*\\*\\)0x[0-9A-Fa-f]+[uU]?"
    TARGET_REGISTER_WRITES "${APP_TEXT}")
list(LENGTH TARGET_REGISTER_WRITES TARGET_REGISTER_WRITE_COUNT)
if(NOT TARGET_REGISTER_WRITE_COUNT EQUAL 1 OR
   NOT TARGET_REGISTER_WRITES STREQUAL "*(volatile unsigned int *)0xE000ED0Cu")
    message(FATAL_ERROR
        "linux_interop target-register inventory changed; only the quarantined reset write is allowed")
endif()

message(STATUS "linux_interop ergonomics baseline is intact")
