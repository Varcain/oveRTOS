#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# embed_bin.py — embed a binary file as a C byte array (O(n) replacement for the
# pure-CMake byte loop, which is O(n^2) on string(APPEND) and takes >10 min for the
# ~900 KB FDPIC rootfs cpio). Invoked by embed_bin.cmake.
#
#   embed_bin.py <in> <out> <sym> [section]
#
# Output matches the historical embed_bin.cmake format exactly:
#   static const unsigned char <sym>[] __attribute__((aligned(8)[, section("..")])) = { ... };
#   static const unsigned long <sym>_len = sizeof(<sym>);
import sys

inp, out, sym = sys.argv[1], sys.argv[2], sys.argv[3]
section = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else ""
sect_attr = ', section("%s")' % section if section else ""

with open(inp, "rb") as f:
    data = f.read()

lines = [
    "/* Generated from %s — do not edit. */" % inp,
    "/* clang-format off */",
    "static const unsigned char %s[] __attribute__((aligned(8)%s)) = {" % (sym, sect_attr),
]
for i in range(0, len(data), 16):
    lines.append("\t" + " ".join("0x%02x," % b for b in data[i:i + 16]))
lines.append("};")
lines.append("/* clang-format on */")
lines.append("static const unsigned long %s_len = sizeof(%s);" % (sym, sym))

with open(out, "w") as f:
    f.write("\n".join(lines) + "\n")
