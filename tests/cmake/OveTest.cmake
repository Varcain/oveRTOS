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
    test_sync_mutex.c
    test_sync_sem.c
    test_sync_event.c
    test_sync_condvar.c
    test_sync_recursive.c
    test_queue.c
    test_timer.c
    test_time.c
    test_eventgroup.c
    test_workqueue.c
    test_stream.c
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
)

# Suite that needs a filesystem backend.
set(OVE_TEST_FS_SUITES
    test_fs.c
)

# Stub-only suites — host-side unit tests that don't need an RTOS.
set(OVE_TEST_STUB_ONLY_SUITES
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
