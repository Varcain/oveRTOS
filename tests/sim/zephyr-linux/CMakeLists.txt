# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Isolated Zephyr CONFIG_USERSPACE variant for the Linux-personality on-target
# bring-up. Kept separate from the full ove_test Zephyr app: the personality
# runs a loaded program as an unprivileged user thread and traps its svc, which
# wants a minimal, dedicated firmware.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_test_zephyr_linux)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
