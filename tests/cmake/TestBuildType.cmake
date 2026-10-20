# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

include(${OVE_ROOT}/cmake/OveCommon.cmake)

# CACHED is the type an earlier configure left in CMakeCache.txt; "" is what
# project() stores on a first configure.  The cache entry itself must change,
# or the next configure starts from the stale value again.
function(expect_build_type CACHED DEBUG EXPECTED)
    unset(CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "${CACHED}" CACHE STRING "Build type" FORCE)
    set(OVE_DEBUG ${DEBUG})
    ove_select_build_type()
    if(NOT "$CACHE{CMAKE_BUILD_TYPE}" STREQUAL "${EXPECTED}"
       OR NOT "${CMAKE_BUILD_TYPE}" STREQUAL "${EXPECTED}")
        message(FATAL_ERROR
            "cached '${CACHED}', OVE_DEBUG=${DEBUG}: expected ${EXPECTED}, "
            "got '${CMAKE_BUILD_TYPE}' (cache '$CACHE{CMAKE_BUILD_TYPE}')")
    endif()
endfunction()

# First configure.
expect_build_type("" OFF Release)
expect_build_type("" ON Debug)
# Debug turned off: a cached Debug must not keep the build at -O0.
expect_build_type(Debug OFF Release)
expect_build_type(debug OFF Release)
# Debug turned back on.
expect_build_type(Release ON Debug)
# An explicit non-Debug type is kept while debug is off.
expect_build_type(RelWithDebInfo OFF RelWithDebInfo)
expect_build_type(MinSizeRel OFF MinSizeRel)
