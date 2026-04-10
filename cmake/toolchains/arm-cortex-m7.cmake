# ============================================================================
# ARM Cortex-M7 toolchain file (hard-float, FPv5-SP)
# ============================================================================
#
# Shared between QEMU MPS2-AN500 (semihosting) and STM32F7 boards (nosys).
# Boards should NOT include this file directly — instead, provide a small
# shim (e.g. boards/<board>/cmake/arm-none-eabi.cmake) that sets
# OVE_ARM_SEMIHOSTING and forwards to this file.  This is required because
# CMake processes toolchain files before project() runs, so ${OVE_DIR} is
# not yet defined and board-level shims must compute paths relatively.
#
# Shim variable:
#   OVE_ARM_SEMIHOSTING   ON  → link with --specs=rdimon.specs (QEMU semihosting)
#                         OFF → link with -specs=nosys.specs (bare-metal)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Toolchain path resolution (priority order):
# 1. OVE_TOOLCHAIN_DIR from build system (downloaded or custom)
# 2. ARM_TOOLCHAIN env var (set in .env)
# 3. Fall back to PATH
if(OVE_TOOLCHAIN_DIR AND EXISTS "${OVE_TOOLCHAIN_DIR}/bin/arm-none-eabi-gcc")
    set(TOOLCHAIN_PREFIX "${OVE_TOOLCHAIN_DIR}/bin/arm-none-eabi-")
elseif(DEFINED ENV{ARM_TOOLCHAIN})
    set(TOOLCHAIN_PREFIX "$ENV{ARM_TOOLCHAIN}/bin/arm-none-eabi-")
else()
    set(TOOLCHAIN_PREFIX "arm-none-eabi-")
endif()

set(CMAKE_C_COMPILER "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_AR "${TOOLCHAIN_PREFIX}ar")
set(CMAKE_OBJCOPY "${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_OBJDUMP "${TOOLCHAIN_PREFIX}objdump")
set(CMAKE_SIZE "${TOOLCHAIN_PREFIX}size")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# CPU flags — Cortex-M7 with hardware FPv5-SP FPU.
# -mcpu=cortex-m7 is canonical (GCC prefers it over -march for tuning).
set(CPU_FLAGS "-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16")

set(CMAKE_C_FLAGS_INIT "${CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS}")

# Linker spec: semihosting (QEMU) or nosys (real hardware).
if(OVE_ARM_SEMIHOSTING)
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} --specs=rdimon.specs")
else()
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} -specs=nosys.specs")
endif()

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
