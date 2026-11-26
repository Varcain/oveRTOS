# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Binary frame types and subtypes for the browser dashboard."""

# ── Frame types ────────────────────────────────────────────────────

FRAME_FB         = 0x01
FRAME_AUDIO      = 0x02
FRAME_EVENT      = 0x03
FRAME_CMD        = 0x04
FRAME_STATE      = 0x05
FRAME_LOG        = 0x06
FRAME_INPUT      = 0x07
FRAME_DEBUG_CMD  = 0x08
FRAME_DEBUG_RESP = 0x09
FRAME_THREAD     = 0x0A
FRAME_FILE_LIST  = 0x0B
FRAME_FILE_REQ   = 0x0C
FRAME_FILE_RESP  = 0x0D
FRAME_TRACE      = 0x0E
FRAME_PROFILE    = 0x0F
FRAME_SERIAL     = 0x10  # Raw console UART output bytes (bridge → page)

# Profiler frame subtypes (bridge-synthesised).
PROFILE_SUB_SAMPLES = 1
PROFILE_SUB_SYMBOLS = 2
PROFILE_SUB_CAPS    = 3

# Debug command types (dashboard → bridge)
DBG_CMD_CONTINUE    = 0x01
DBG_CMD_PAUSE       = 0x02
DBG_CMD_STEP_OVER   = 0x03
DBG_CMD_STEP_INTO   = 0x04
DBG_CMD_STEP_OUT    = 0x05
DBG_CMD_RESET       = 0x06
DBG_CMD_BACKTRACE   = 0x07
DBG_CMD_REGISTERS   = 0x08
DBG_CMD_DISASSEMBLE = 0x09
DBG_CMD_BP_SET      = 0x0A
DBG_CMD_BP_CLEAR    = 0x0B
DBG_CMD_SOURCE      = 0x0C

# Debug response types (bridge → dashboard)
DBG_RESP_STATE      = 0x00
DBG_RESP_BACKTRACE  = 0x07
DBG_RESP_REGISTERS  = 0x08
DBG_RESP_DISASSEMBLY = 0x09
DBG_RESP_BREAKPOINT = 0x0A
DBG_RESP_SOURCE     = 0x0C
