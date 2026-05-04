# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# ETL (Embedded Template Library) — header-only fixed-capacity STL-like
# containers (vector, string, map, list, deque, …) for C++ apps.
#
# ETL is header-only and always usable in both heap and zeroheap modes:
# every non-intrusive container is fixed-capacity, so the heap is never
# touched.  Configured to integrate cleanly with oveRTOS's
# `-fno-exceptions -fno-rtti` C++ defaults: ETL_NO_EXCEPTIONS routes
# capacity overflow / out-of-range / illegal-state through ETL's
# error-handler hook instead of `throw`, matching the rest of the
# binding's "errors are values" posture.
#
# Public macro:
#
#   ove_use_etl()
#       Adds the ETL include directory globally.  No-op if ETL is not
#       vendored under ${OVE_DL_DIR}/etl/include (e.g. C-only builds).
#
# Apps and bindings consume ETL via `#include <etl/vector.h>` etc.;
# nothing else is required.

macro(ove_use_etl)
    set(_OVE_ETL_INCLUDE "${OVE_DL_DIR}/etl/include")

    if(EXISTS "${_OVE_ETL_INCLUDE}/etl/vector.h")
        include_directories(${_OVE_ETL_INCLUDE})

        # Match oveRTOS's `-fno-exceptions -fno-rtti` defaults: ETL routes
        # capacity-full / out-of-range / illegal-state through a global
        # error-handler hook instead of throwing.  Without this define ETL
        # falls back to `throw etl::*_exception(...)`, which fails to link
        # under `-fno-exceptions`.
        add_compile_definitions($<$<COMPILE_LANGUAGE:CXX>:ETL_NO_EXCEPTIONS>)

        message(STATUS "  ETL: ${_OVE_ETL_INCLUDE}")
    else()
        message(STATUS
            "  ETL: not vendored at ${_OVE_ETL_INCLUDE} "
            "(run 'ove download' if your app uses ETL containers)")
    endif()
endmacro()
