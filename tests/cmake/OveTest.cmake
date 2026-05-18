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
    test_init_no_alloc.c
)

# Suite that needs a filesystem backend.
set(OVE_TEST_FS_SUITES
    test_fs.c
)

# Stub-only suites — host-side unit tests that don't need an RTOS.
set(OVE_TEST_STUB_ONLY_SUITES
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
