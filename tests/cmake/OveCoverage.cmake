# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Reusable coverage pipeline for host-side CMocka/gcov test targets.
#
# ove_test_coverage_report(<name> <target>)
#   Adds a custom target `coverage` that:
#     1. Zeroes gcov counters in ${CMAKE_BINARY_DIR}
#     2. Runs ${target} (which must be built with --coverage)
#     3. Captures gcov data into ${CMAKE_BINARY_DIR}/coverage/coverage.info,
#        tagged with lcov --test-name <name> so merges across backends are
#        attributable.
#     4. Filters out system headers, fetched deps, and the tests/ tree.
#     5. Emits HTML under ${CMAKE_BINARY_DIR}/coverage/html/ for standalone use.
#
#   The unfiltered .info file is also preserved so the top-level Makefile can
#   merge per-backend tracefiles into one combined report.

function(ove_test_coverage_report name target)
    find_program(LCOV_BIN lcov)
    find_program(GENHTML_BIN genhtml)
    if(NOT LCOV_BIN OR NOT GENHTML_BIN)
        message(STATUS "lcov/genhtml not found — `coverage` target disabled for ${target}")
        return()
    endif()

    set(cov_dir ${CMAKE_BINARY_DIR}/coverage)
    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${cov_dir}
        COMMAND ${LCOV_BIN} --directory ${CMAKE_BINARY_DIR} --zerocounters
        COMMAND $<TARGET_FILE:${target}>
        COMMAND ${LCOV_BIN} --directory ${CMAKE_BINARY_DIR}
                --capture --test-name ${name}
                --output-file ${cov_dir}/coverage.info
                --ignore-errors mismatch,gcov,source
        COMMAND ${LCOV_BIN} --remove ${cov_dir}/coverage.info
                "'/usr/*'" "'*/_deps/*'" "'*/tests/*'"
                --output-file ${cov_dir}/coverage.filtered.info
                --ignore-errors unused
        COMMAND ${GENHTML_BIN} ${cov_dir}/coverage.filtered.info
                --output-directory ${cov_dir}/html
                --ignore-errors source,mismatch
        DEPENDS ${target}
        COMMENT "Running ${target} under gcov and producing HTML report"
        USES_TERMINAL
    )
endfunction()
