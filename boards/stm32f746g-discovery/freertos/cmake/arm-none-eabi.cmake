# STM32F746G-Discovery FreeRTOS toolchain shim.
# Real hardware — no semihosting (nosys.specs).  Forwards to the shared
# Cortex-M7 toolchain file at cmake/toolchains/arm-cortex-m7.cmake.
set(OVE_ARM_SEMIHOSTING OFF)
include(${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/toolchains/arm-cortex-m7.cmake)
