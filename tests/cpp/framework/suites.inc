/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Authoritative list of C++ test suites. X-macro style: the caller defines
 * OVE_CPP_SUITE(name, label) and includes this header. It is the single
 * source of truth for three otherwise-duplicated places — the runtime
 * dispatch (cpp_main.cpp), the runner declarations (framework/ove_test.hpp),
 * and the compiled source list (CMakeLists.txt, via
 * ove_test_cpp_suites() in tests/cmake/OveTest.cmake, which also fails
 * configure if a suites/test_<name>.cpp is unlisted/missing/duplicated).
 *
 * Each entry maps to a suites/test_<name>.cpp defining
 * `int test_cpp_<name>_run(void);`.
 *
 *   #define OVE_CPP_SUITE(n, l) \
 *       printf("=== C++ " l " Tests ===\n"); failures += test_cpp_##n##_run();
 *   #include "framework/suites.inc"
 *   #undef OVE_CPP_SUITE
 */

#ifndef OVE_CPP_SUITE
#define OVE_CPP_SUITE(name, label)
#define _OVE_CPP_SUITES_INC_DEFAULTED
#endif

OVE_CPP_SUITE(mutex,           "Mutex")
OVE_CPP_SUITE(recursive_mutex, "Recursive Mutex")
OVE_CPP_SUITE(semaphore,       "Semaphore")
OVE_CPP_SUITE(event,           "Event")
OVE_CPP_SUITE(condvar,         "CondVar")
OVE_CPP_SUITE(queue,           "Queue")
OVE_CPP_SUITE(timer,           "Timer")
OVE_CPP_SUITE(thread,          "Thread")
OVE_CPP_SUITE(thread_stop,     "Thread Stop")
OVE_CPP_SUITE(eventgroup,      "EventGroup")
OVE_CPP_SUITE(lockguard,       "LockGuard")
OVE_CPP_SUITE(app,             "App")
OVE_CPP_SUITE(console,         "Console")
OVE_CPP_SUITE(time,            "Time")
OVE_CPP_SUITE(watchdog,        "Watchdog")
OVE_CPP_SUITE(nvs,             "NVS")
OVE_CPP_SUITE(shell,           "Shell")
OVE_CPP_SUITE(bsp,             "BSP")
OVE_CPP_SUITE(board,           "Board")
OVE_CPP_SUITE(gpio,            "GPIO")
OVE_CPP_SUITE(led,             "LED")
OVE_CPP_SUITE(audio,           "Audio")
OVE_CPP_SUITE(fs,              "Filesystem")
OVE_CPP_SUITE(stream,          "Stream")
OVE_CPP_SUITE(workqueue,       "Workqueue")
OVE_CPP_SUITE(static_init,     "Static Init")
OVE_CPP_SUITE(infer,           "Infer")
OVE_CPP_SUITE(error,           "Error")
OVE_CPP_SUITE(net,             "Net")

#ifdef _OVE_CPP_SUITES_INC_DEFAULTED
#undef OVE_CPP_SUITE
#undef _OVE_CPP_SUITES_INC_DEFAULTED
#endif
