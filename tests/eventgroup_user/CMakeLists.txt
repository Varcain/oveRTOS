# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
get_filename_component(OVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include(${OVE_ROOT}/cmake/OveZephyrSyscalls.cmake)
ove_zephyr_register_eventgroup_syscalls()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_eventgroup_user)
get_filename_component(OVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
target_sources(app PRIVATE main.c "${OVE_ROOT}/backends/zephyr/zephyr_eventgroup.c")
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${OVE_ROOT}/include" "${OVE_ROOT}/backends/zephyr/include"
    "${OVE_ROOT}/backends/common" "${OVE_ROOT}/tests")
target_compile_options(app PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
