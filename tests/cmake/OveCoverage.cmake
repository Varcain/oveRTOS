# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Reusable coverage pipeline for host-side CMocka/gcov test targets.
#
# ove_test_coverage_report(<name> <target> [<target> ...])
#   Adds a custom target `coverage` that:
#     1. Zeroes gcov counters in ${CMAKE_BINARY_DIR}
#     2. Runs each ${target} (all must be built with --coverage). Each
#        executable emits its own .gcda files under its own CMakeFiles/
#        subdir, so counters naturally merge when lcov captures the tree.
#        Passing multiple targets lets a single backend-level coverage
#        pass exercise multiple build configurations (e.g. heap-based and
#        zero-heap) against the same source tree.
#     3. Captures gcov data into ${CMAKE_BINARY_DIR}/coverage/coverage.info,
#        tagged with lcov --test-name <name> so merges across backends are
#        attributable.
#     4. Reduces to oveRTOS sources only via `lcov --extract` on the four
#        first-party directories (src, backends, bindings, include). An
#        allowlist beats a denylist here — it drops CMocka, system headers,
#        /usr/, _deps/, RTOS internals, and any future third-party code
#        without per-site patterns.
#     5. Emits HTML under ${CMAKE_BINARY_DIR}/coverage/html/ for standalone use.
#
#   The unfiltered .info file is also preserved so the top-level Makefile can
#   merge per-backend tracefiles into one combined report.

function(ove_test_coverage_report name)
    set(targets ${ARGN})
    if(NOT targets)
        message(FATAL_ERROR "ove_test_coverage_report: need at least one target")
    endif()
    find_program(LCOV_BIN lcov)
    find_program(GENHTML_BIN genhtml)
    if(NOT LCOV_BIN OR NOT GENHTML_BIN)
        message(STATUS "lcov/genhtml not found — `coverage` target disabled for ${target}")
        return()
    endif()

    if(NOT DEFINED OVE_ROOT)
        message(FATAL_ERROR "ove_test_coverage_report: OVE_ROOT must be set by the caller")
    endif()
    # Canonicalize — callers typically define OVE_ROOT as
    # `${CMAKE_CURRENT_SOURCE_DIR}/..`, which leaves `..` in the path. lcov
    # matches extract patterns against the literal SF: paths in the tracefile
    # (which are already canonical), so a pattern containing `tests/..` would
    # silently match nothing. Resolve first.
    get_filename_component(_ove_root_abs "${OVE_ROOT}" REALPATH)
    # The allowlist patterns. Quoted to survive the shell Make launches for
    # custom-target commands (each pattern is one argv entry once lcov runs).
    set(ove_extract_patterns
        "'${_ove_root_abs}/src/*'"
        "'${_ove_root_abs}/backends/*'"
        "'${_ove_root_abs}/bindings/*'"
        "'${_ove_root_abs}/include/*'")

    set(cov_dir ${CMAKE_BINARY_DIR}/coverage)

    # Build a COMMAND-interleaved list that runs each target between the
    # zero-counters and capture steps. Using list(APPEND ... COMMAND ...)
    # keeps the single-add_custom_target form — lcov captures after all
    # binaries have finished so each target's .gcda files are present.
    set(_run_cmds "")
    set(_deps "")
    foreach(t IN LISTS targets)
        list(APPEND _run_cmds COMMAND $<TARGET_FILE:${t}>)
        list(APPEND _deps ${t})
    endforeach()
    list(JOIN targets ", " _targets_pretty)

    add_custom_target(coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${cov_dir}
        COMMAND ${LCOV_BIN} --directory ${CMAKE_BINARY_DIR} --zerocounters
        ${_run_cmds}
        COMMAND ${LCOV_BIN} --directory ${CMAKE_BINARY_DIR}
                --capture --test-name ${name}
                --rc branch_coverage=1
                --output-file ${cov_dir}/coverage.info
                --ignore-errors mismatch,gcov,source,negative
        COMMAND ${LCOV_BIN} --extract ${cov_dir}/coverage.info
                ${ove_extract_patterns}
                --rc branch_coverage=1
                --output-file ${cov_dir}/coverage.filtered.info
                --ignore-errors unused,empty,inconsistent
        COMMAND ${GENHTML_BIN} ${cov_dir}/coverage.filtered.info
                --branch-coverage
                --output-directory ${cov_dir}/html
                --ignore-errors source,mismatch
        DEPENDS ${_deps}
        COMMENT "Running ${_targets_pretty} under gcov and producing HTML report"
        USES_TERMINAL
    )
endfunction()
