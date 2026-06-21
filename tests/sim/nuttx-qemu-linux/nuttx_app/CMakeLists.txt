# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Self-contained minimal app for the isolated Linux-personality on-target test.
# Unlike tests/sim/_nuttx_app.cmake (which links the full ove_test suite), this
# links only what the bFLT/SVC-trap test needs: the arena, the loader, the
# Linux syscall dispatch, the NuttX SVC trap seam, the test, and CMocka.

if(NOT CONFIG_EXTERNAL_OVE_TEST)
  return()
endif()
if(NOT OVE_DIR)
  message(FATAL_ERROR "OVE_DIR not set. Pass -DOVE_DIR=<oveRTOS root>.")
endif()
if(NOT _OVE_VARIANT_DIR)
  message(FATAL_ERROR "_OVE_VARIANT_DIR not set (holds the variant ove_config.h).")
endif()

set(_nuttx_be ${OVE_DIR}/backends/nuttx)
set(_cmocka_dir ${OVE_DIR}/dl/cmocka)

set(_srcs
  # main.c MUST stay first: nuttx_add_application aliases its main() to
  # <NAME>_main (the INIT_ENTRYPOINT).
  ${CMAKE_CURRENT_SOURCE_DIR}/main.c

  # Reusable layers + the Linux personality (engine-agnostic dispatch + the
  # NuttX SVC trap seam).
  ${OVE_DIR}/backends/common/ove_arena.c
  ${OVE_DIR}/loader/ove_loader.c
  ${OVE_DIR}/linux/ove_linux_syscall.c
  ${_nuttx_be}/nuttx_lnx_trap.c

  # The on-target test + CMocka.
  ${OVE_DIR}/tests/ontarget/test_linux_target.c
  ${_cmocka_dir}/src/cmocka.c
)

set(_include_dirs
  ${_OVE_VARIANT_DIR}
  ${OVE_DIR}/include
  ${_nuttx_be}/include
  ${OVE_DIR}/backends/common
  ${OVE_DIR}/tests
  ${_cmocka_dir}/include
  ${_cmocka_dir}/src
)

nuttx_add_application(
  NAME ${CONFIG_EXTERNAL_OVE_TEST_PROGNAME}
  SRCS ${_srcs}
  STACKSIZE ${CONFIG_EXTERNAL_OVE_TEST_STACKSIZE}
  PRIORITY ${CONFIG_EXTERNAL_OVE_TEST_PRIORITY}
  COMPILE_FLAGS -Wno-unused-parameter -Wno-missing-field-initializers
  INCLUDE_DIRECTORIES ${_include_dirs}
)
