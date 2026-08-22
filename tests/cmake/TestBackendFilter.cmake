# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

include("${OVE_ROOT}/cmake/OveHelpers.cmake")

set(BACKENDS
    "${OVE_ROOT}/backends/freertos/freertos_console.c"
    "${OVE_ROOT}/backends/nuttx/nuttx_console.c"
    "${OVE_ROOT}/backends/common/lxp_ove_console.c"
    "${OVE_ROOT}/backends/common/lxp_ove_console_native.c"
    "${OVE_ROOT}/src/ove_console.c")
_ove_filter_backend_list(BACKENDS CONSOLE)

foreach(REMOVED IN ITEMS freertos/freertos_console.c nuttx/nuttx_console.c)
    list(FIND BACKENDS "${OVE_ROOT}/backends/${REMOVED}" POSITION)
    if(NOT POSITION EQUAL -1)
        message(FATAL_ERROR "engine backend survived replacement: ${REMOVED}")
    endif()
endforeach()

foreach(RETAINED IN ITEMS
        "${OVE_ROOT}/backends/common/lxp_ove_console.c"
        "${OVE_ROOT}/backends/common/lxp_ove_console_native.c"
        "${OVE_ROOT}/src/ove_console.c")
    list(FIND BACKENDS "${RETAINED}" POSITION)
    if(POSITION EQUAL -1)
        message(FATAL_ERROR "shared composition was removed: ${RETAINED}")
    endif()
endforeach()
