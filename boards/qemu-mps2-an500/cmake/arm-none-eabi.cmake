# QEMU MPS2-AN500 toolchain shim.
# Uses semihosting for stdio (rdimon.specs).  Forwards to the shared
# Cortex-M7 toolchain file at cmake/toolchains/arm-cortex-m7.cmake.
set(OVE_ARM_SEMIHOSTING ON)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/toolchains/arm-cortex-m7.cmake)
