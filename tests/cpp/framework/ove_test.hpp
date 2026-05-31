/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_TEST_CPP_HPP
#define OVE_TEST_CPP_HPP

/*
 * ⚠ cmocka + C++ RAII: cmocka's assert_*() macros report failure via
 * longjmp() (see <setjmp.h> below).  A longjmp does NOT unwind the C++ stack,
 * so any stack RAII object live at the point of a *failing* assertion has its
 * destructor SKIPPED — e.g. a `ove::LockGuard`/`ove::Mutex` is left
 * locked/leaked, and a destructor-based cleanup in the test body never runs.
 *
 * Impact is failure-path only (the test has already failed), so it does not
 * cause false passes, but it can muddy leak reports and, if a *shared* object
 * is left in a bad state, perturb a later test.  Guidance: keep asserts out of
 * scopes that hold RAII cleanup you depend on, or move cleanup into a cmocka
 * teardown (which DOES run after a failed assert).  Do not rely on stack
 * destructors for correctness across an assertion.
 */

/* Pull in <chrono> (via ove.hpp) BEFORE cmocka so std::chrono internals
 * don't collide with cmocka's bare `fail()` macro. */
#include "ove/ove.hpp"

extern "C" {
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
}

#include <atomic>
#include <type_traits>

#include "../../framework/ove_test_common.h"

static inline void test_msleep(uint32_t ms)
{
	ove::this_thread::sleep_ms(ms);
}

template <size_t StackSize = 4096>
inline ove::Thread<StackSize> make_test_thread(const char *name, ove_thread_fn entry,
					       void *arg = nullptr,
					       ove_prio_t prio = OVE_PRIO_NORMAL)
{
	return ove::Thread<StackSize>(entry, arg, prio, name);
}

/* C++ test suite runner declarations */
int test_cpp_mutex_run(void);
int test_cpp_recursive_mutex_run(void);
int test_cpp_semaphore_run(void);
int test_cpp_event_run(void);
int test_cpp_condvar_run(void);
int test_cpp_queue_run(void);
int test_cpp_timer_run(void);
int test_cpp_thread_run(void);
int test_cpp_thread_stop_run(void);
int test_cpp_eventgroup_run(void);
int test_cpp_lockguard_run(void);
int test_cpp_app_run(void);
int test_cpp_console_run(void);
int test_cpp_time_run(void);
int test_cpp_watchdog_run(void);
int test_cpp_nvs_run(void);
int test_cpp_shell_run(void);
int test_cpp_bsp_run(void);
int test_cpp_board_run(void);
int test_cpp_gpio_run(void);
int test_cpp_led_run(void);
int test_cpp_audio_run(void);
int test_cpp_fs_run(void);
int test_cpp_stream_run(void);
int test_cpp_workqueue_run(void);
int test_cpp_static_init_run(void);
int test_cpp_infer_run(void);
int test_cpp_error_run(void);

#endif /* OVE_TEST_CPP_HPP */
