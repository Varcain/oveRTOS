# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# ##############################################################################
# tests/sim/_nuttx_app.cmake
#
# Shared CMake fragment for all NuttX `ove_test` variant fixtures
# (nuttx-qemu, nuttx-qemu-zeroheap, renode-stm32f746-nuttx*). Each variant
# sets _OVE_ZERO_HEAP=ON|OFF then `include()`s this file from its
# nuttx_app/CMakeLists.txt. The variant's own `ove_config.h` is picked up
# from `${CMAKE_CURRENT_SOURCE_DIR}/..` (i.e. tests/sim/<variant>/).
#
# The CMake build emits objects under build/CMakeFiles/.../*.o (with
# -MMD-driven header dep tracking), so unlike the legacy Application.mk
# fixture it doesn't scatter .o/.gcno/.gcda into the OVE_DIR source tree.
# ##############################################################################

if(NOT CONFIG_EXTERNAL_OVE_TEST)
  return()
endif()

if(NOT OVE_DIR)
  message(FATAL_ERROR
    "OVE_DIR not set. Pass -DOVE_DIR=<oveRTOS root> at cmake configure time.")
endif()

if(NOT DEFINED _OVE_ZERO_HEAP)
  message(FATAL_ERROR
    "_OVE_ZERO_HEAP must be set ON/OFF before including _nuttx_app.cmake")
endif()
if(NOT _OVE_VARIANT_DIR)
  message(FATAL_ERROR
    "_OVE_VARIANT_DIR must point at the OVE-side tests/sim/<variant>/ "
    "dir (holds the variant's ove_config.h). The per-variant nuttx_app "
    "tree gets copied into the apps build dir, so CMAKE_CURRENT_SOURCE_DIR "
    "doesn't resolve back to OVE — pass it explicitly.")
endif()

set(_tests_root ${OVE_DIR}/tests)
set(_stub_dir ${_tests_root}/backends/stub)
set(_suite_dir ${_tests_root}/suites)
set(_nuttx_be ${OVE_DIR}/backends/nuttx)
set(_cmocka_dir ${OVE_DIR}/dl/cmocka)

set(_srcs
  # main.c MUST stay first: nuttx_add_application aliases main() in the
  # first listed source to <NAME>_main.
  ${CMAKE_CURRENT_SOURCE_DIR}/main.c

  # NuttX backend modules — pure POSIX, provided by NuttX kernel.
  ${_nuttx_be}/nuttx_thread.c
  ${_nuttx_be}/nuttx_heap_lock.c
  ${_nuttx_be}/nuttx_sync.c
  ${_nuttx_be}/nuttx_queue.c
  ${_nuttx_be}/nuttx_timer.c
  ${_nuttx_be}/nuttx_time.c
  ${_nuttx_be}/nuttx_eventgroup.c
  ${_nuttx_be}/nuttx_workqueue.c
  ${_nuttx_be}/nuttx_stream.c
  ${_nuttx_be}/nuttx_console.c
  ${_nuttx_be}/nuttx_shell.c

  # Core dispatcher modules.
  ${OVE_DIR}/src/ove_board.c
  ${OVE_DIR}/src/ove_gpio.c
  ${OVE_DIR}/src/ove_led.c
  ${OVE_DIR}/src/ove_app.c

  # Common backend + stubs for hardware-dependent modules.
  ${OVE_DIR}/backends/common/ove_audio_graph.c
  ${OVE_DIR}/backends/common/ove_audio_nodes.c
  ${_stub_dir}/stub_bsp.c
  ${_stub_dir}/stub_lvgl.c
  ${_stub_dir}/stub_gpio.c
  ${_stub_dir}/stub_board.c
  ${_stub_dir}/stub_watchdog.c
  ${_stub_dir}/stub_nvs.c
  ${_stub_dir}/stub_fs.c

  # Test suites.
  ${_suite_dir}/test_storage_bounds.c
  ${_suite_dir}/test_hw_stm32f746.c
  ${_suite_dir}/test_renode_stm32_obs.c
  ${_suite_dir}/test_renode_stm32_periph.c
  ${_suite_dir}/test_renode_stm32_net.c
  ${_suite_dir}/test_thread.c
  ${_suite_dir}/test_sync_mutex.c
  ${_suite_dir}/test_sync_sem.c
  ${_suite_dir}/test_sync_event.c
  ${_suite_dir}/test_sync_condvar.c
  ${_suite_dir}/test_sync_recursive.c
  ${_suite_dir}/test_queue.c
  ${_suite_dir}/test_timer.c
  ${_suite_dir}/test_time.c
  ${_suite_dir}/test_eventgroup.c
  ${_suite_dir}/test_workqueue.c
  ${_suite_dir}/test_stream.c
  ${_suite_dir}/test_public_create.c
  ${_suite_dir}/test_console.c
  ${_suite_dir}/test_watchdog.c
  ${_suite_dir}/test_nvs.c
  ${_suite_dir}/test_shell.c
  ${_suite_dir}/test_audio.c
  ${_suite_dir}/test_bsp.c
  ${_suite_dir}/test_board.c
  ${_suite_dir}/test_gpio.c
  ${_suite_dir}/test_led.c
  ${_suite_dir}/test_fs.c
  ${_suite_dir}/test_lvgl.c
  ${_suite_dir}/test_app.c

  # CMocka (built as part of this app).
  ${_cmocka_dir}/src/cmocka.c
)

if(_OVE_ZERO_HEAP)
  list(APPEND _srcs ${OVE_DIR}/backends/common/ove_heap_lock.c)
endif()

set(_compile_flags
  -Wno-unused-parameter
  -Wno-missing-field-initializers
)
set(_definitions "")

if(_OVE_ZERO_HEAP)
  list(APPEND _definitions CONFIG_OVE_ZERO_HEAP=1)
endif()

# Coverage — opted in via -DOVE_COVERAGE=ON. Only the app-side .c files
# are instrumented here; libgcov.a is pulled into the final kernel link
# by NuttX's CONFIG_COVERAGE_TOOLCHAIN=y (set via the variant's
# nuttx_test_coverage_defconfig).
if(OVE_COVERAGE)
  list(APPEND _compile_flags --coverage -fprofile-abs-path)
  list(APPEND _definitions OVE_COVERAGE=1)
endif()

# Include order: the variant's own dir (ove_config.h overrides) FIRST so
# it wins over the include/ove_config.h shipped with the library.
set(_include_dirs
  ${_OVE_VARIANT_DIR}
  ${OVE_DIR}/include
  ${_nuttx_be}/include
  ${OVE_DIR}/backends/common
  ${_tests_root}
  ${_cmocka_dir}/include
  ${_cmocka_dir}/src
)

nuttx_add_application(
  NAME ${CONFIG_EXTERNAL_OVE_TEST_PROGNAME}
  SRCS ${_srcs}
  STACKSIZE ${CONFIG_EXTERNAL_OVE_TEST_STACKSIZE}
  PRIORITY ${CONFIG_EXTERNAL_OVE_TEST_PRIORITY}
  COMPILE_FLAGS ${_compile_flags}
  DEFINITIONS ${_definitions}
  INCLUDE_DIRECTORIES ${_include_dirs}
)

# Zero-heap mode traps kernel-mm allocations after init by --wrap'ing
# malloc/free/etc.  These are LINK options, so they must attach to the
# final `nuttx` executable target, not the apps_<NAME> archive.
#
# This file is evaluated twice: once in the kernel's `preapps`
# add_subdirectory pass (before `add_executable(nuttx)` runs, used only
# to seed Kconfig) and once in the `apps` pass (after the nuttx target
# exists). Guarding on `if(TARGET nuttx)` skips the first pass cleanly;
# the second pass applies the wrappers to the kernel link.
if(_OVE_ZERO_HEAP AND TARGET nuttx)
  target_link_options(nuttx PRIVATE
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
    -Wl,--wrap=zalloc -Wl,--wrap=memalign -Wl,--wrap=free
    -Wl,--wrap=kmm_malloc -Wl,--wrap=kmm_calloc
    -Wl,--wrap=kmm_realloc -Wl,--wrap=kmm_zalloc
    -Wl,--wrap=kmm_memalign -Wl,--wrap=kmm_free
  )
endif()

# Dispatch-overhead symbol audit on the final `nuttx` ELF (Phase 3 of
# moat-hardening).  NuttX's flat-build emits the final ELF in a sibling
# cmake project (nuttx-cmake) that this app dir cannot attach POST_BUILD
# steps to — same scope problem the zero-heap wrap above solves with
# target_link_options on the `nuttx` target.  Use ELF_PATH for the
# file-level dependency hook.  Zero-heap variants also enforce
# NO_PANIC_SYMBOLS to catch Rust/Zig panic-formatting that would pull in
# allocating fmt paths.  Symbol audit only — no hotpath disasm on test
# ELFs (test scaffolding is not the production hot path).
#
# CMAKE_BINARY_DIR is the kernel CMake project's root either way:
# the production flat-build configures with -B build/nuttx-cmake (so
# CMAKE_BINARY_DIR ends in nuttx-cmake/), while the sim test build
# configures with -B build/ directly. In both cases add_executable(nuttx)
# emits the ELF at ${CMAKE_BINARY_DIR}/nuttx.
if(TARGET nuttx)
  include(${OVE_DIR}/cmake/OveZeroOverheadAudit.cmake)
  set(_audit_args BINDING c ELF_PATH ${CMAKE_BINARY_DIR}/nuttx)
  if(_OVE_ZERO_HEAP)
    list(APPEND _audit_args NO_PANIC_SYMBOLS)
  endif()
  ove_assert_no_dispatch_overhead(${CONFIG_EXTERNAL_OVE_TEST_PROGNAME}
    ${_audit_args})
endif()
