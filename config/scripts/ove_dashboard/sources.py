# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import json
import os
import struct
import sys

from . import frames
from .protocol import FRAME_FILE_LIST, FRAME_FILE_RESP

OVE_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
)

# ── Source file list (built from CMake build objects) ────────────────

_project_files = []  # list of {"path": abs_path, "short": display, "cat": category}
_project_file_list_frame = None  # cached payload for FRAME_FILE_LIST


def build_project_file_list(build_dir, ove_dir):
    """Scan CMake build dir for compiled .c.obj files and categorize."""
    global _project_files, _project_file_list_frame
    if not build_dir or not os.path.isdir(build_dir):
        return

    # Scan all .c.obj files in the build tree.  Works with both
    # FreeRTOS (single CMakeFiles/firmware.elf.dir/) and Zephyr
    # (objects scattered across app/, zephyr/kernel/, etc.).
    # Object paths encode the source path — extract it and filter
    # to files under ove_dir (skip RTOS/SDK internals).

    files = []
    seen = set()
    for root, _, names in os.walk(build_dir):
        for name in names:
            # CMake uses .c.obj (cross) or .c.o (native)
            if name.endswith(".c.obj"):
                ext = ".c.obj"
            elif name.endswith(".c.o"):
                ext = ".c.o"
            else:
                continue
            obj_path = os.path.join(root, name)
            # Extract source path from the obj dir structure.
            # CMake encodes the full source path in the obj directory.
            rel = os.path.relpath(obj_path, build_dir)
            src = rel[:-(len(ext) - 2)]  # strip extension, keep .c
            # Strip CMake dir prefixes
            # FreeRTOS: CMakeFiles/firmware.elf.dir/home/user/.../file.c
            # Zephyr:   app/CMakeFiles/app.dir/home/user/.../file.c
            idx = src.find("/home/")
            if idx >= 0:
                abs_path = src[idx:]  # "/home/user/.../file.c"
            else:
                # Relative source (main.c, system_*.c) — resolve
                # against workspace dir
                ws_dir = build_dir
                if "/build/" in ws_dir:
                    ws_dir = ws_dir.split("/build/")[0]
                abs_path = os.path.join(ws_dir, src.split("/")[-1])

            if not os.path.isfile(abs_path):
                continue
            abs_path = os.path.realpath(abs_path)
            if abs_path in seen:
                continue
            seen.add(abs_path)

            # Only include files under ove_dir (skip Zephyr kernel,
            # SDK, and driver internals).
            if ove_dir and not abs_path.startswith(ove_dir):
                continue

            # Categorize
            short = os.path.relpath(abs_path, ove_dir) if ove_dir else abs_path
            if "/apps/" in abs_path:
                cat = "App"
            elif "/backends/" in abs_path:
                cat = "Backend"
            elif "/src/ove_" in abs_path:
                cat = "Core"
            elif "/sim/" in abs_path:
                cat = "Sim"
            elif "/boards/" in abs_path or short.endswith("main.c") \
                    or "system_" in short:
                cat = "Board"
            elif ("FreeRTOS" in abs_path or "freertos" in abs_path.lower()
                  or "/nuttx/" in abs_path.lower()
                  or "/dl/nuttx" in abs_path.lower()
                  or "/zephyr/" in abs_path.lower()
                  or "/dl/zephyr" in abs_path.lower()):
                cat = "RTOS"
            elif "/dl/lvgl" in abs_path or "/lvgl/" in abs_path:
                cat = "LVGL"
            elif "/tests/" in abs_path or "/stub" in abs_path:
                cat = "Stubs"
            else:
                cat = "Other"

            files.append({"path": abs_path, "short": short, "cat": cat})

    # Sort: App first, then Core, Backend, Board, Sim, RTOS, LVGL, Stubs
    cat_order = {"App": 0, "Core": 1, "Backend": 2, "Board": 3,
                 "Sim": 4, "RTOS": 5, "LVGL": 6, "Stubs": 7, "Other": 8}
    files.sort(key=lambda f: (cat_order.get(f["cat"], 9), f["short"]))

    _project_files = files

    # Build cached FRAME_FILE_LIST payload
    file_list_json = json.dumps(
        [{"path": f["path"], "short": f["short"], "cat": f["cat"]}
         for f in files]).encode("utf-8")
    _project_file_list_frame = struct.pack("<I", FRAME_FILE_LIST) + file_list_json

    sys.stderr.write(f"[files] {len(files)} source files indexed\n")
    sys.stderr.flush()

def handle_file_request(fdata):
    """Handle FRAME_FILE_REQ: read and send a full source file."""
    # Payload: UTF-8 file path
    path = fdata.decode("utf-8", "replace").strip()
    if not path:
        return
    # Security: only serve files in the project file list
    allowed = any(f["path"] == path for f in _project_files)
    if not allowed:
        return
    try:
        with open(path, "r", errors="replace") as f:
            content = f.read()
    except OSError:
        content = "// File not found: " + path
    payload = struct.pack("<I", FRAME_FILE_RESP)
    # [uint32 frame_type][uint16 path_len][path][content]
    path_bytes = path.encode("utf-8")
    payload += struct.pack("<H", len(path_bytes))
    payload += path_bytes
    payload += content.encode("utf-8")
    frames.post_frame(payload)
