#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Extract ove storage type sizes and alignments from a compiled object file.

Usage: extract_storage_sizes.py <object_file> <output_file>

Compiles a C file with sizeof/alignof arrays, then parses the nm output
to produce KEY=VALUE pairs suitable for passing as env vars to cargo.

WASM builds via emscripten produce LLVM bitcode / wasm object files
that the host `nm` can't parse ("file format not recognized").  Falls
back to `llvm-nm` automatically when the system `nm` fails for that
reason.  An explicit `NM` env var (e.g. `NM=llvm-nm`) overrides both.
"""

import os
import re
import shutil
import subprocess
import sys


def _run_nm(nm_bin, obj_file):
    return subprocess.run(
        [nm_bin, "--print-size", obj_file],
        capture_output=True, text=True
    )


def _looks_like_unknown_format(stderr):
    s = (stderr or "").lower()
    return ("file format not recognized" in s
            or "unknown file type" in s
            or "no such file" not in s and "invalid wasm" in s)


def _resolve_llvm_nm():
    """Return a usable llvm-nm path, preferring an emscripten-bundled
    binary if EMSDK / EM_LLVM_ROOT is set; otherwise falls back to
    PATH lookup."""
    for env_key in ("EM_LLVM_ROOT", "EMSDK"):
        root = os.environ.get(env_key)
        if not root:
            continue
        for cand in (
            os.path.join(root, "bin", "llvm-nm"),
            os.path.join(root, "upstream", "bin", "llvm-nm"),
        ):
            if os.path.isfile(cand) and os.access(cand, os.X_OK):
                return cand
    return shutil.which("llvm-nm")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <object_file> <output_file>", file=sys.stderr)
        sys.exit(1)

    obj_file = sys.argv[1]
    out_file = sys.argv[2]

    # Pick the nm binary: explicit override, host nm, then llvm-nm fallback.
    nm_override = os.environ.get("NM")
    candidates = [nm_override] if nm_override else ["nm"]
    if not nm_override:
        llvm_nm = _resolve_llvm_nm()
        if llvm_nm:
            candidates.append(llvm_nm)

    last_err = ""
    result = None
    for nm_bin in candidates:
        result = _run_nm(nm_bin, obj_file)
        if result.returncode == 0:
            break
        last_err = result.stderr
        # Only fall through to the next candidate when the failure is a
        # format-recognition issue — otherwise the file is broken and
        # llvm-nm wouldn't help either.
        if not _looks_like_unknown_format(last_err):
            break

    if result is None or result.returncode != 0:
        print(f"nm failed: {last_err}", file=sys.stderr)
        sys.exit(1)

    # Parse lines like: 00000000 000000c4 B _sizeof_ove_thread_storage_t
    pattern = re.compile(
        r'[0-9a-f]+\s+([0-9a-f]+)\s+[A-Z]\s+_(sizeof|alignof)_(\w+)'
    )

    sizes = {}
    for line in result.stdout.splitlines():
        m = pattern.match(line)
        if m:
            value = int(m.group(1), 16)
            kind = m.group(2).upper()  # SIZEOF or ALIGNOF
            type_name = m.group(3).upper()  # e.g. OVE_THREAD_STORAGE_T
            # Remove _T suffix for cleaner env var names
            type_name = re.sub(r'_T$', '', type_name)
            key = f"{kind}_{type_name}"
            sizes[key] = value

    if not sizes:
        print("warning: no storage sizes found in object file", file=sys.stderr)

    with open(out_file, 'w') as f:
        for key, value in sorted(sizes.items()):
            f.write(f"{key}={value}\n")


if __name__ == '__main__':
    main()
