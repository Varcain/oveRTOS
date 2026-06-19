# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# embed_bin.cmake — embed a binary file as a C byte array.
#
#   cmake -DIN=<file> -DOUT=<header> -DSYM=<ident> -P embed_bin.cmake
#
# Writes a header declaring:
#   static const unsigned char <SYM>[] __attribute__((aligned(8))) = { ... };
#   static const unsigned long <SYM>_len = sizeof(<SYM>);

file(READ "${IN}" _hex HEX)
string(REGEX MATCHALL "[0-9a-f][0-9a-f]" _bytes "${_hex}")

set(_body "")
set(_i 0)
foreach(_b ${_bytes})
    string(APPEND _body "0x${_b},")
    math(EXPR _i "${_i} + 1")
    math(EXPR _wrap "${_i} % 16")
    if(_wrap EQUAL 0)
        string(APPEND _body "\n\t")
    else()
        string(APPEND _body " ")
    endif()
endforeach()

file(WRITE "${OUT}"
    "/* Generated from ${IN} — do not edit. */\n"
    "static const unsigned char ${SYM}[] __attribute__((aligned(8))) = {\n\t${_body}\n};\n"
    "static const unsigned long ${SYM}_len = sizeof(${SYM});\n")
