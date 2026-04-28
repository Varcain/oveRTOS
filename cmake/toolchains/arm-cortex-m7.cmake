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

# C++ header ordering for picolibc + arm-gnu-toolchain libstdc++.
#
# arm-gnu-toolchain ships libstdc++ built against newlib, so libstdc++
# headers like <cstdlib> use `#include_next <stdlib.h>` to chain into
# the C library's stdlib.h.  With `-isystem <picolibc>/include` alone,
# picolibc lands at position #1 of the system search list, *before* the
# libstdc++ dirs (`include/c++/<ver>`).  `#include_next` from <cstdlib>
# (in c++/<ver>) skips earlier paths and resolves <stdlib.h> in the
# default arm-none-eabi/include — newlib's stdlib.h, which then pulls
# sys/reent.h whose `_READ_WRITE_*` macros aren't defined when picolibc's
# sys/_types.h has already been used.  Result: cstdlib fails to compile,
# and a successful link still references _impure_ptr from newlib paths.
#
# Fix: explicitly inject the libstdc++ dirs via -isystem *before*
# picolibc.  GCC processes -isystem in declaration order, so the layout
# becomes c++/<ver>, c++/<ver>/<multilib>, c++/<ver>/backward, picolibc,
# then the default arm-none-eabi paths (deduplicated).  `#include_next
# <stdlib.h>` from cstdlib now lands on picolibc's header before reaching
# the toolchain's newlib stdlib.h.
separate_arguments(_ove_cpu_args UNIX_COMMAND "${CPU_FLAGS}")
execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" ${_ove_cpu_args} -E -v -x c++ -
    INPUT_FILE /dev/null
    OUTPUT_QUIET
    ERROR_VARIABLE _ove_cxx_search
    RESULT_VARIABLE _ove_cxx_search_rc)
if(NOT _ove_cxx_search_rc EQUAL 0)
    message(FATAL_ERROR
        "Failed to query C++ compiler search paths (rc=${_ove_cxx_search_rc}): "
        "${_ove_cxx_search}")
endif()
string(REGEX MATCH "#include <\\.\\.\\.> search starts here:([^#]+)End of search list" _ove_cxx_search_block "${_ove_cxx_search}")
set(_ove_cxx_isystem "")
string(REPLACE "\n" ";" _ove_cxx_lines "${CMAKE_MATCH_1}")
foreach(_line IN LISTS _ove_cxx_lines)
    string(STRIP "${_line}" _dir)
    if(_dir MATCHES "/c\\+\\+/")
        get_filename_component(_dir "${_dir}" ABSOLUTE)
        set(_ove_cxx_isystem "${_ove_cxx_isystem} -isystem ${_dir}")
    endif()
endforeach()
if(_ove_cxx_isystem STREQUAL "")
    message(FATAL_ERROR
        "Could not extract libstdc++ include dirs from compiler search paths")
endif()
set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_CXX_FLAGS_INIT}${_ove_cxx_isystem} -isystem ${OVE_PICOLIBC_PREFIX}/include")

# Visibility for picolibc/libstdc++ coexistence.
#
# `-std=gnu++17` predefines `_GNU_SOURCE`, which forces picolibc's
# features.h to set `_DEFAULT_SOURCE=1` and therefore `__MISC_VISIBLE=1`.
# Under `__MISC_VISIBLE`, picolibc's <math.h> declares `extern int isinf
# (double)` / `extern int isnan (double)` in the global namespace.
# libstdc++ <cmath> then declares `std::isinf` as `constexpr bool(double)`
# and brings the picolibc declaration in via `using ::isinf`, which gcc
# rejects as a redeclaration with conflicting return type.  Undefining
# `_GNU_SOURCE` and pinning `_POSIX_C_SOURCE` keeps `__MISC_VISIBLE=0`,
# so picolibc skips those declarations and the C++ build compiles cleanly
# without losing GNU extensions outside this visibility band.
set(CMAKE_CXX_FLAGS_INIT
    "${CMAKE_CXX_FLAGS_INIT} -U_GNU_SOURCE -D_POSIX_C_SOURCE=200809L")

# `--defsym=_impure_ptr=0` resolves a libstdc++ link reference.  The C++
# runtime shipped with arm-gnu-toolchain (assert_fail.cc / vterminate.cc)
# was built against newlib and references newlib's per-thread `_impure_ptr`
# from cold error-handler paths (assertion failures, uncaught exception
# verbose terminate).  Picolibc has no equivalent symbol; defining it as
# 0 satisfies the relocations.  These paths are not exercised under normal
# operation, so a null `_impure_ptr` never gets dereferenced — and if one
# of them did fire, we'd be aborting anyway.
if(OVE_ARM_SEMIHOSTING)
    set(CMAKE_EXE_LINKER_FLAGS_INIT
        "${CPU_FLAGS} --specs=${OVE_PICOLIBC_SPECS} --oslib=semihost --crt0=hosted -Wl,--defsym=_impure_ptr=0")
else()
    set(CMAKE_EXE_LINKER_FLAGS_INIT
        "${CPU_FLAGS} --specs=${OVE_PICOLIBC_SPECS} -Wl,--defsym=_impure_ptr=0")
endif()

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
