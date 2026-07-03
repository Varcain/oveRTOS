# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# OveTest.cmake — shared helpers for oveRTOS test targets.
#
# Rather than copy the list of suite sources into every CMakeLists.txt
# under tests/sim/*, define the list once here and let each variant pull
# in the subset it can actually run.
#
# The suite categories mirror framework/suites.inc:
#   COMMON — runs on every backend (thread, sync, queue, timer, ...)
#   FS     — needs a filesystem backend
#   STUB   — host-only suites (networking helpers, I2C/SPI/UART, PM, ...)
#
# Usage:
#   include(${TESTS_ROOT}/cmake/OveTest.cmake)
#   ove_test_suite_sources(ALL_SUITES ${SUITE_DIR})
#   target_sources(my_test PRIVATE ${ALL_SUITES})

# Suites runnable on every backend (including bare-metal QEMU).
set(OVE_TEST_COMMON_SUITES
    test_storage_bounds.c
    test_hw_stm32f746.c
    test_renode_stm32_obs.c
    test_renode_stm32_periph.c
    test_renode_stm32_net.c
    test_thread.c
    test_thread_stop.c
    test_sync_mutex.c
    test_sync_sem.c
    test_sync_event.c
    test_sync_condvar.c
    test_sync_recursive.c
    test_queue.c
    test_timer.c
    test_time.c
    test_timeout_ns.c
    test_deadline_until.c
    test_eventgroup.c
    test_workqueue.c
    test_stream.c
    test_public_create.c
    test_console.c
    test_watchdog.c
    test_nvs.c
    test_shell.c
    test_audio.c
    test_bsp.c
    test_board.c
    test_gpio.c
    test_led.c
    test_lvgl.c
    test_app.c
    test_async.c
    test_init_no_alloc.c
)

# Suite that needs a filesystem backend.
set(OVE_TEST_FS_SUITES
    test_fs.c
)

# Stub-only suites — host-side unit tests that don't need an RTOS.
set(OVE_TEST_STUB_ONLY_SUITES
    test_arena.c
    test_loader.c
    test_protected.c
    test_sandbox.c
    test_linux_syscall.c
    test_linux_dev.c
    test_thread_stop_isolation.c
    test_static_define.c
    test_infer.c
    test_net_mqtt.c
    test_net_httpd.c
    test_net_sntp.c
    test_net_loopback.c
    test_i2c.c
    test_spi.c
    test_uart.c
    test_pm.c
)

# ove_test_common_suite_sources(<out_var> <suite_dir>)
#   Fill <out_var> with absolute paths for the common suite sources.
function(ove_test_common_suite_sources out_var suite_dir)
    set(result "")
    foreach(src ${OVE_TEST_COMMON_SUITES})
        list(APPEND result "${suite_dir}/${src}")
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# ove_test_fs_suite_sources(<out_var> <suite_dir>)
function(ove_test_fs_suite_sources out_var suite_dir)
    set(result "")
    foreach(src ${OVE_TEST_FS_SUITES})
        list(APPEND result "${suite_dir}/${src}")
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# ove_test_stub_only_suite_sources(<out_var> <suite_dir>)
function(ove_test_stub_only_suite_sources out_var suite_dir)
    set(result "")
    foreach(src ${OVE_TEST_STUB_ONLY_SUITES})
        list(APPEND result "${suite_dir}/${src}")
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# ove_test_all_suite_sources(<out_var> <suite_dir>)
#   Convenience — common + FS + stub-only (used by the stub target).
function(ove_test_all_suite_sources out_var suite_dir)
    ove_test_common_suite_sources(_common "${suite_dir}")
    ove_test_fs_suite_sources(_fs "${suite_dir}")
    ove_test_stub_only_suite_sources(_stub "${suite_dir}")
    set(${out_var} ${_common} ${_fs} ${_stub} PARENT_SCOPE)
endfunction()


# ove_test_validate_suite_membership(<suite_dir>)
#   Configure-time guard against drift between the three category lists
#   above and the actual test_*.c files in <suite_dir>.  Fails the
#   build with a named diagnostic on:
#     - a file listed in more than one category,
#     - a category entry that doesn't exist on disk,
#     - a `test_*.c` file on disk that no category lists.
#   Call once from tests/CMakeLists.txt after the include of this file.
function(ove_test_validate_suite_membership suite_dir)
    # Union of the three category lists, with intra-set duplicate
    # detection (a file appearing twice in COMMON would not surface
    # otherwise — list(APPEND) is happy to duplicate).
    set(_seen "")
    set(_dups "")
    foreach(_category COMMON FS STUB_ONLY)
        foreach(_file ${OVE_TEST_${_category}_SUITES})
            if(_file IN_LIST _seen)
                list(APPEND _dups "${_file}")
            else()
                list(APPEND _seen "${_file}")
            endif()
        endforeach()
    endforeach()

    # On-disk set (basenames, sorted for stable diagnostics).
    file(GLOB _on_disk RELATIVE "${suite_dir}" "${suite_dir}/test_*.c")
    list(SORT _on_disk)
    list(SORT _seen)

    set(_missing_on_disk "")
    foreach(_f ${_seen})
        if(NOT EXISTS "${suite_dir}/${_f}")
            list(APPEND _missing_on_disk "${_f}")
        endif()
    endforeach()

    set(_uncategorized "")
    foreach(_f ${_on_disk})
        if(NOT _f IN_LIST _seen)
            list(APPEND _uncategorized "${_f}")
        endif()
    endforeach()

    if(_dups OR _missing_on_disk OR _uncategorized)
        set(_msg "OveTest: test-suite categorisation mismatch in ${suite_dir}\n")
        if(_dups)
            string(APPEND _msg "  Listed in >1 category: ${_dups}\n")
        endif()
        if(_missing_on_disk)
            string(APPEND _msg "  Listed but not on disk:  ${_missing_on_disk}\n")
        endif()
        if(_uncategorized)
            string(APPEND _msg "  On disk but uncategorised: ${_uncategorized}\n")
        endif()
        string(APPEND _msg "Update the OVE_TEST_*_SUITES lists in tests/cmake/OveTest.cmake.")
        message(FATAL_ERROR "${_msg}")
    endif()
endfunction()


# ove_test_validate_suites_inc_parity(<suites_inc>)
#   Cross-check that the per-suite categories in framework/suites.inc
#   (OVE_SUITE / OVE_SUITE_FS / OVE_SUITE_STUB) agree with the
#   OVE_TEST_{COMMON,FS,STUB_ONLY}_SUITES lists above.  These are two
#   independent sources of truth: suites.inc drives the *runtime* dispatch
#   (which test_*_run() the firmware calls per backend) while the cmake
#   lists drive *compilation*.  If a suite is COMMON in one and STUB in the
#   other it silently misbehaves — e.g. COMMON in cmake (compiled on QEMU)
#   but STUB in suites.inc (never dispatched there) ⇒ the suite is dead on
#   QEMU/Renode with no error.  ove_test_validate_suite_membership only
#   guards cmake-vs-disk; this guards cmake-vs-suites.inc.  Fails configure
#   on any divergence.
function(ove_test_validate_suites_inc_parity suites_inc)
    if(NOT EXISTS "${suites_inc}")
        message(FATAL_ERROR "OveTest: suites.inc not found at ${suites_inc}")
    endif()

    # Pull the real suite entries (anchored at column 0 so the macro
    # #defines, the `_OVE_SUITES_INC_DEFAULTED_*` guards and the comment
    # examples are ignored).  More-specific macros first.
    file(STRINGS "${suites_inc}" _stub_lines REGEX "^OVE_SUITE_STUB\\(")
    file(STRINGS "${suites_inc}" _fs_lines   REGEX "^OVE_SUITE_FS\\(")
    file(STRINGS "${suites_inc}" _all_lines  REGEX "^OVE_SUITE")

    set(_inc_stub "")
    foreach(_l ${_stub_lines})
        string(REGEX MATCH "^OVE_SUITE_STUB\\(([a-z0-9_]+)" _ "${_l}")
        if(CMAKE_MATCH_1)
            list(APPEND _inc_stub "test_${CMAKE_MATCH_1}.c")
        endif()
    endforeach()
    set(_inc_fs "")
    foreach(_l ${_fs_lines})
        string(REGEX MATCH "^OVE_SUITE_FS\\(([a-z0-9_]+)" _ "${_l}")
        if(CMAKE_MATCH_1)
            list(APPEND _inc_fs "test_${CMAKE_MATCH_1}.c")
        endif()
    endforeach()
    # COMMON = OVE_SUITE(...) lines, i.e. ^OVE_SUITE followed directly by `(`.
    set(_inc_common "")
    foreach(_l ${_all_lines})
        if("${_l}" MATCHES "^OVE_SUITE\\(([a-z0-9_]+)")
            list(APPEND _inc_common "test_${CMAKE_MATCH_1}.c")
        endif()
    endforeach()

    set(_problems "")
    foreach(_cat COMMON FS STUB_ONLY)
        if(_cat STREQUAL "COMMON")
            set(_inc ${_inc_common})
        elseif(_cat STREQUAL "FS")
            set(_inc ${_inc_fs})
        else()
            set(_inc ${_inc_stub})
        endif()
        set(_cm ${OVE_TEST_${_cat}_SUITES})
        list(SORT _inc)
        list(SORT _cm)
        foreach(_f ${_inc})
            if(NOT _f IN_LIST _cm)
                list(APPEND _problems "${_f}: suites.inc=${_cat} but missing from cmake ${_cat}")
            endif()
        endforeach()
        foreach(_f ${_cm})
            if(NOT _f IN_LIST _inc)
                list(APPEND _problems "${_f}: cmake ${_cat} but not suites.inc ${_cat}")
            endif()
        endforeach()
    endforeach()

    if(_problems)
        string(REPLACE ";" "\n  " _pp "${_problems}")
        message(FATAL_ERROR
            "OveTest: framework/suites.inc <-> OveTest.cmake category mismatch:\n  ${_pp}\n"
            "Keep the OVE_SUITE/OVE_SUITE_FS/OVE_SUITE_STUB categories in suites.inc "
            "in sync with the OVE_TEST_{COMMON,FS,STUB_ONLY}_SUITES lists.")
    endif()
endfunction()


# ove_test_cpp_suites(<suites_inc> <suite_dir> <out_sources_var>)
#   Single-source the C++ test suite list from <suites_inc> — the
#   OVE_CPP_SUITE(name, label) x-macro file that also drives the runtime
#   dispatch (cpp_main.cpp) and runner declarations (framework/ove_test.hpp).
#   Parses the suite names, validates them against the test_*.cpp files in
#   <suite_dir>, and returns the matching ${suite_dir}/test_<name>.cpp list in
#   <out_sources_var>.  Fails configure (FATAL_ERROR) when a suite is listed
#   more than once, listed but has no test_<name>.cpp on disk, or a
#   test_*.cpp on disk is not listed in the .inc.  This is the C++ analogue
#   of ove_test_validate_suite_membership (single category — all C++ suites
#   are runnable).
function(ove_test_cpp_suites suites_inc suite_dir out_sources_var)
    if(NOT EXISTS "${suites_inc}")
        message(FATAL_ERROR "OveTest: C++ suites.inc not found at ${suites_inc}")
    endif()

    # Parse OVE_CPP_SUITE(<name>, ...) entries (anchored at column 0 so the
    # macro #defines, the defaulted guards and comment examples are ignored).
    file(STRINGS "${suites_inc}" _lines REGEX "^OVE_CPP_SUITE\\(")
    set(_names "")
    set(_dups "")
    foreach(_l ${_lines})
        string(REGEX MATCH "^OVE_CPP_SUITE\\(([a-z0-9_]+)" _ "${_l}")
        if(CMAKE_MATCH_1)
            if(CMAKE_MATCH_1 IN_LIST _names)
                list(APPEND _dups "${CMAKE_MATCH_1}")
            else()
                list(APPEND _names "${CMAKE_MATCH_1}")
            endif()
        endif()
    endforeach()

    # Expected sources from the .inc + listed-but-missing detection.
    set(_sources "")
    set(_missing_on_disk "")
    foreach(_n ${_names})
        if(EXISTS "${suite_dir}/test_${_n}.cpp")
            list(APPEND _sources "${suite_dir}/test_${_n}.cpp")
        else()
            list(APPEND _missing_on_disk "test_${_n}.cpp")
        endif()
    endforeach()

    # On-disk test_*.cpp not listed in the .inc.
    file(GLOB _on_disk RELATIVE "${suite_dir}" "${suite_dir}/test_*.cpp")
    set(_uncategorized "")
    foreach(_f ${_on_disk})
        string(REGEX REPLACE "^test_(.+)\\.cpp$" "\\1" _n "${_f}")
        if(NOT _n IN_LIST _names)
            list(APPEND _uncategorized "${_f}")
        endif()
    endforeach()

    if(_dups OR _missing_on_disk OR _uncategorized)
        set(_msg "OveTest: C++ test-suite mismatch (${suites_inc} <-> ${suite_dir})\n")
        if(_dups)
            string(APPEND _msg "  Duplicated in suites.inc: ${_dups}\n")
        endif()
        if(_missing_on_disk)
            string(APPEND _msg "  Listed but no file on disk:  ${_missing_on_disk}\n")
        endif()
        if(_uncategorized)
            string(APPEND _msg "  On disk but not in suites.inc: ${_uncategorized}\n")
        endif()
        string(APPEND _msg
            "Add an OVE_CPP_SUITE(<name>, \"<Label>\") line to "
            "tests/cpp/framework/suites.inc for each suites/test_<name>.cpp.")
        message(FATAL_ERROR "${_msg}")
    endif()

    set(${out_sources_var} ${_sources} PARENT_SCOPE)
endfunction()
