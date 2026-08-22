# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

set(COMMON_FILE "${OVE_ROOT}/cmake/OveCommon.cmake")
file(READ "${COMMON_FILE}" COMMON_TEXT)

if(COMMON_TEXT MATCHES "(^|\n)[ \t]*project[ \t]*\\(")
    message(FATAL_ERROR
        "OveCommon.cmake must not hide the top-level project() declaration")
endif()
if(NOT COMMON_TEXT MATCHES
   "if\\(OVE_APP_LANG STREQUAL \"cpp\" OR OVE_INFER\\)[^\n]*\n[ \t]*enable_language\\(CXX\\)")
    message(FATAL_ERROR
        "ove_setup_project must retain conditional C++ enablement")
endif()

foreach(BOARD IN ITEMS stm32f746g-discovery qemu-mps2-an500)
    set(BOARD_FILE "${OVE_ROOT}/boards/${BOARD}/freertos/CMakeLists.txt")
    file(READ "${BOARD_FILE}" BOARD_TEXT)

    string(FIND "${BOARD_TEXT}" "project(firmware C ASM)" PROJECT_POSITION)
    string(FIND "${BOARD_TEXT}" "include(" INCLUDE_POSITION)
    string(FIND "${BOARD_TEXT}" "ove_setup_project()" SETUP_POSITION)
    if(PROJECT_POSITION EQUAL -1)
        message(FATAL_ERROR
            "${BOARD_FILE} lacks a direct C/ASM project declaration")
    endif()
    if(INCLUDE_POSITION EQUAL -1 OR PROJECT_POSITION GREATER INCLUDE_POSITION)
        message(FATAL_ERROR
            "${BOARD_FILE} must declare project() before shared includes")
    endif()
    if(SETUP_POSITION EQUAL -1 OR PROJECT_POSITION GREATER SETUP_POSITION)
        message(FATAL_ERROR
            "${BOARD_FILE} must declare project() before ove_setup_project()")
    endif()
    if(BOARD_TEXT MATCHES "ove_setup_project[ \t]*\\([^)]")
        message(FATAL_ERROR
            "${BOARD_FILE} duplicates the project name in ove_setup_project")
    endif()
endforeach()

message(STATUS "FreeRTOS project bootstrap ownership is explicit")
