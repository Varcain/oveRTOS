# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Pin the Zephyr-side half of the copied-text W^X contract.  This deliberately
# checks the canonical template rather than a developer's potentially stale
# generated output.

if(NOT DEFINED OVE_ROOT)
    message(FATAL_ERROR "OVE_ROOT is required")
endif()

set(PRJ_TEMPLATE "${OVE_ROOT}/config/templates/prj.conf.j2")
file(READ "${PRJ_TEMPLATE}" PRJ_TEXT)

string(REGEX MATCHALL "CONFIG_EXECUTE_XOR_WRITE=y" WX_ENABLED "${PRJ_TEXT}")
list(LENGTH WX_ENABLED WX_ENABLED_COUNT)
if(NOT WX_ENABLED_COUNT EQUAL 1)
    message(FATAL_ERROR
        "${PRJ_TEMPLATE} must enable CONFIG_EXECUTE_XOR_WRITE exactly once")
endif()

if(PRJ_TEXT MATCHES "CONFIG_EXECUTE_XOR_WRITE=n")
    message(FATAL_ERROR
        "${PRJ_TEMPLATE} must not disable CONFIG_EXECUTE_XOR_WRITE in any profile")
endif()
