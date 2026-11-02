# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.18)
project(ove_nuttx_startup C)

get_filename_component(OVE_DIR ../.. ABSOLUTE)
set(OVE_NUTTX_DIR "${OVE_DIR}/dl/nuttx" CACHE PATH "Pinned native NuttX source tree")
set(_native "${OVE_NUTTX_DIR}/libs/libc/misc/lib_cxx_initialize.c")
if(NOT EXISTS "${_native}")
    message(FATAL_ERROR "Fetch the pinned NuttX sources before running this test")
endif()
option(OVE_SANITIZE "Enable host address and undefined behavior sanitizers" OFF)
if(OVE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
        -fno-sanitize-recover=all -fno-pie)
    add_link_options(-fsanitize=address,undefined -no-pie)
endif()
add_compile_options(-std=c11 -Wall -Wextra -Werror)

set(CONFIG_BUILD_FLAT TRUE)
set(CONFIG_HAVE_CXXINITIALIZE TRUE)
add_library(c STATIC "${_native}")
target_include_directories(c PRIVATE sdk)
set_property(SOURCE "${_native}" APPEND PROPERTY COMPILE_DEFINITIONS
    FIXTURE_NATIVE_UNIT=1 PRESERVED_DEFINITION=17)
add_executable(nuttx fixture.c)
target_include_directories(nuttx PRIVATE sdk)
target_link_libraries(nuttx PRIVATE c)
include(${OVE_DIR}/cmake/OveNuttXStartup.cmake)
ove_nuttx_guard_cxx_initialization()
ove_nuttx_guard_cxx_initialization()

find_package(Threads REQUIRED)
add_executable(guard_concurrent concurrent.c
    ${OVE_DIR}/backends/nuttx/nuttx_cxx_initialize.c)
target_include_directories(guard_concurrent PRIVATE sdk)
target_link_libraries(guard_concurrent PRIVATE Threads::Threads)

enable_testing()
foreach(_case recursive acyclic empty nulls)
    add_test(NAME constructors_${_case} COMMAND nuttx ${_case})
    set_tests_properties(constructors_${_case} PROPERTIES TIMEOUT 15)
endforeach()
add_test(NAME constructors_concurrent COMMAND guard_concurrent)
set_tests_properties(constructors_concurrent PROPERTIES TIMEOUT 15)
