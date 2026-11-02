# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CONFIG_EXTERNAL_OVE_TEST)
    return()
endif()
nuttx_add_application(
    NAME ${CONFIG_EXTERNAL_OVE_TEST_PROGNAME}
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/main.c
        ${OVE_DIR}/backends/nuttx/nuttx_thread.c
        ${OVE_DIR}/backends/nuttx/nuttx_time.c
    STACKSIZE ${CONFIG_EXTERNAL_OVE_TEST_STACKSIZE}
    PRIORITY ${CONFIG_EXTERNAL_OVE_TEST_PRIORITY}
    INCLUDE_DIRECTORIES ${_OVE_VARIANT_DIR}
        ${OVE_DIR}/include ${OVE_DIR}/backends/nuttx/include
        ${OVE_DIR}/backends/common ${OVE_DIR}/tests)

if(OVE_CTOR_ZERO_HEAP)
    target_compile_definitions(apps_${CONFIG_EXTERNAL_OVE_TEST_PROGNAME}
        PRIVATE CONFIG_OVE_ZERO_HEAP=1)
endif()
include(${OVE_DIR}/cmake/OveNuttXStartup.cmake)
ove_nuttx_guard_cxx_initialization()
