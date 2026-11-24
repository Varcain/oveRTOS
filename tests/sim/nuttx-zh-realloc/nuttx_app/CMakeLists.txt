# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CONFIG_EXTERNAL_OVE_TEST)
    return()
endif()
set(_backend ${OVE_DIR}/backends/nuttx)
nuttx_add_application(NAME ${CONFIG_EXTERNAL_OVE_TEST_PROGNAME}
    SRCS ${CMAKE_CURRENT_SOURCE_DIR}/main.c ${OVE_DIR}/backends/common/ove_heap_lock.c
         ${_backend}/nuttx_heap_lock.c ${_backend}/nuttx_zh_reserved_heap.c
    STACKSIZE ${CONFIG_EXTERNAL_OVE_TEST_STACKSIZE}
    PRIORITY ${CONFIG_EXTERNAL_OVE_TEST_PRIORITY} DEFINITIONS CONFIG_OVE_ZERO_HEAP=1
    INCLUDE_DIRECTORIES ${_OVE_VARIANT_DIR} ${OVE_DIR}/include
    ${_backend}/include ${OVE_DIR}/backends/common ${OVE_DIR}/tests)
