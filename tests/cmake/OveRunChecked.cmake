# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
# Run one coverage executable and require its successful completion summary.

if(NOT DEFINED TIMEOUT)
    set(TIMEOUT 600)
endif()

execute_process(
    COMMAND "${EXE}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT "${TIMEOUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
)
message("${output}")

if(NOT result STREQUAL "0")
    message(FATAL_ERROR "${EXE} failed: ${result}")
endif()
if(NOT output MATCHES "(^|\n)=== Summary: 0 test group\\(s\\) had failures ===(\n|$)")
    message(FATAL_ERROR "${EXE} did not print a zero-failure summary")
endif()
