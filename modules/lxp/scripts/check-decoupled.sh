#!/bin/sh
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CI guard: fail if the lxp module regains any oveRTOS coupling. The module must
# depend on NOTHING outside its own include/lxp/ (+ the C standard library). This
# checks the source-level contract; the definitive proof is that
# `cmake -S modules/lxp` builds liblxp.a with no oveRTOS tree on the path.
set -eu
cd "$(dirname "$0")/.."

# Any #include of an oveRTOS header, or a board_desc.h, is a coupling regression.
# (Plain "ove_*" mentions in comments/docs are fine — this matches include lines
#  and the CONFIG_OVE_ / board_desc.h build coupling only.)
bad=$(grep -rEn '#[[:space:]]*include[[:space:]]*"(ove/|ove_config|board_desc)' src include 2>/dev/null || true)
# Real CONFIG_OVE_ preprocessor conditionals (not comment mentions of them).
cfg=$(grep -rEn '(defined[[:space:]]*\([[:space:]]*CONFIG_OVE_|#[[:space:]]*if(n?def)?[[:space:]]+CONFIG_OVE_)' src include 2>/dev/null || true)

if [ -n "$bad" ] || [ -n "$cfg" ]; then
	echo "FAIL: lxp module regained an oveRTOS dependency:"
	[ -n "$bad" ] && echo "$bad"
	[ -n "$cfg" ] && echo "$cfg"
	exit 1
fi
echo "OK: lxp is decoupled — no oveRTOS headers / CONFIG_OVE_ / board_desc.h in src or include."
