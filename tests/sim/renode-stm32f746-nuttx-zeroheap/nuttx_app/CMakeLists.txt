# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# renode-stm32f746-nuttx-zeroheap ove_test app — see tests/sim/_nuttx_app.cmake.
# _OVE_VARIANT_DIR comes from the CLI's -D arg.
set(_OVE_ZERO_HEAP ON)
include(${OVE_DIR}/tests/sim/_nuttx_app.cmake)
