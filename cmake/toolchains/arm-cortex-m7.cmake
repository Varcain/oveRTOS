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

# Picolibc include dir for compile flags so #include <stdio.h> etc. resolve
# to picolibc's headers (not newlib's, which would drag in _impure_ptr /
# _ctype_ references at object level).  We add `-isystem` rather than
# `--specs=picolibc.specs` here because picolibc.specs uses
# `%rename link picolibc_link` — passing it twice (compile + link) trips
# "already defined" in gcc spec parsing.  `-isystem` only adjusts header
# search; libc/crt0 selection happens at link via --specs= below.

# Libc: picolibc, built in-tree against this toolchain.  The arm-gnu-toolchain
# 15.x distribution ships only newlib (nano.specs, rdimon.specs, nosys.specs);
# we vendor picolibc (manifest.yaml > libraries.picolibc) and build it once
# via meson into output/picolibc-install/<tag>-<hash>/, which produces the
# picolibc.specs we link against here.  The build runs at configure time
# inside this toolchain file so subsequent CMake commands can reference the
# generated absolute path.  See cmake/PicolibcBuild.cmake.
#
# Why we stop using newlib's specs files:
#   --specs=rdimon.specs   pulled newlib + libgloss (rdimon semihost stubs);
#                          replaced by `--oslib=semihost --crt0=hosted` under
#                          picolibc, which routes _write/_exit through the
#                          same ARM semihosting calls without libgloss.
#   --specs=nosys.specs    pulled newlib + libgloss-nosys (errno-returning
#                          stubs); replaced by plain picolibc.specs — board
#                          provides its own _write/_sbrk in syscalls.c just
#                          like before.
#
# Compute OVE_DIR from the toolchain file path so this works before
# project() runs.  Toolchain file lives at:
#     <OVE_DIR>/cmake/toolchains/arm-cortex-m7.cmake
get_filename_component(_ove_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OVE_DIR "${_ove_dir}" CACHE INTERNAL "oveRTOS repo root")

include("${_ove_dir}/cmake/PicolibcBuild.cmake")

set(PICOLIBC_TAG "1.8.10")
set(PICOLIBC_TOOLCHAIN_PREFIX "${TOOLCHAIN_PREFIX}")
set(PICOLIBC_CPU_FLAGS "${CPU_FLAGS}")
ove_build_picolibc()

set(CMAKE_C_FLAGS_INIT
    "${CMAKE_C_FLAGS_INIT} -isystem ${OVE_PICOLIBC_PREFIX}/include")
set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_CXX_FLAGS_INIT} -isystem ${OVE_PICOLIBC_PREFIX}/include")

if(OVE_ARM_SEMIHOSTING)
    set(CMAKE_EXE_LINKER_FLAGS_INIT
        "${CPU_FLAGS} --specs=${OVE_PICOLIBC_SPECS} --oslib=semihost --crt0=hosted")
else()
    set(CMAKE_EXE_LINKER_FLAGS_INIT
        "${CPU_FLAGS} --specs=${OVE_PICOLIBC_SPECS}")
endif()

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
