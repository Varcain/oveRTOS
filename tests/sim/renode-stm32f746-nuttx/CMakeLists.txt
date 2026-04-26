# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# This file is intentionally empty.
#
# NuttX QEMU tests use the NuttX build system (Make-based), not CMake.
# The Makefile target 'test-qemu-nuttx' handles the full NuttX build flow:
#   1. Fetch NuttX kernel + apps (nuttx-12.12.0)
#   2. Copy to build tree
#   3. Register test runner as external NuttX app
#   4. Configure for mps2-an500:nsh
#   5. Build with NuttX make
#   6. Run the resulting ELF in QEMU
#
# The test application is defined in:
#   tests/sim/nuttx-qemu/nuttx_app/Makefile   (NuttX Application.mk)
#   tests/sim/nuttx-qemu/nuttx_app/Kconfig    (NuttX Kconfig)
#   tests/sim/nuttx-qemu/nuttx_app/main.c     (entry point → test runner)
