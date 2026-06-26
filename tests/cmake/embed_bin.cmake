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

# A pure-CMake byte loop is O(n^2) on string(APPEND) and takes >10 minutes for the ~900 KB FDPIC
# rootfs cpio (re-run on every cpio change). Delegate to a tiny O(n) Python generator (<1 s) that
# emits byte-identical output. The optional -DSECTION=<name> places the array in a named section
# (e.g. an executable .text subsection so an MPU engine's UNPRIVILEGED threads can execute embedded
# code in-place — the FDPIC text-sharing maps libc.so's RO text straight out of the embedded cpio).
set(_section "")
if(DEFINED SECTION AND NOT SECTION STREQUAL "")
    set(_section "${SECTION}")
endif()
find_program(_EMBED_PY NAMES python3 python REQUIRED)
execute_process(
    COMMAND "${_EMBED_PY}" "${CMAKE_CURRENT_LIST_DIR}/embed_bin.py" "${IN}" "${OUT}" "${SYM}" "${_section}"
    RESULT_VARIABLE _embed_rc)
if(NOT _embed_rc EQUAL 0)
    message(FATAL_ERROR "embed_bin.py failed (${_embed_rc}) for ${IN}")
endif()
