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

set(FREERTOS_MPU_PATCH
    "${OVE_ROOT}/modules/lxp/ports/freertos/patches/0001-arm-cm4-mpu-drop-global-user-peripheral-map.patch")
file(READ "${FREERTOS_MPU_PATCH}" FREERTOS_MPU_PATCH_TEXT)

# Returning the peripheral slot expands xMPU_SETTINGS by one descriptor. Both
# initial task restore and PendSV must install that tail descriptor.
string(REGEX MATCHALL "portTOTAL_NUM_REGIONS_IN_TCB % 4UL"
    FREERTOS_TAIL_GUARDS "${FREERTOS_MPU_PATCH_TEXT}")
string(REGEX MATCHALL "ldmia r2!, \\{r4-r5\\}"
    FREERTOS_TAIL_LOADS "${FREERTOS_MPU_PATCH_TEXT}")
string(REGEX MATCHALL "stmia r0, \\{r4-r5\\}"
    FREERTOS_TAIL_STORES "${FREERTOS_MPU_PATCH_TEXT}")
list(LENGTH FREERTOS_TAIL_GUARDS FREERTOS_TAIL_GUARD_COUNT)
list(LENGTH FREERTOS_TAIL_LOADS FREERTOS_TAIL_LOAD_COUNT)
list(LENGTH FREERTOS_TAIL_STORES FREERTOS_TAIL_STORE_COUNT)
if(NOT FREERTOS_TAIL_GUARD_COUNT EQUAL 2 OR
   NOT FREERTOS_TAIL_LOAD_COUNT EQUAL 2 OR
   NOT FREERTOS_TAIL_STORE_COUNT EQUAL 2)
    message(FATAL_ERROR
        "${FREERTOS_MPU_PATCH} must restore the final task MPU descriptor in both context paths")
endif()
