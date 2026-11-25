# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Locate Rust libraries for the standalone compiler checks."""
import json
import subprocess
from pathlib import Path


def cargo_libs(command, env):
    out = subprocess.run(command + ["--message-format=json-render-diagnostics"], env=env,
                         stdout=subprocess.PIPE, text=True, timeout=240, check=True).stdout
    libs, dirs = {}, {}
    for line in out.splitlines():
        message = json.loads(line) if line.startswith("{") else {}
        if message.get("reason") != "compiler-artifact":
            continue
        crate = message["target"]["name"]
        for filename in message["filenames"]:
            if filename.endswith((".rlib", ".so")):
                dirs[Path(filename).parent] = None
            if filename.endswith(".rlib"):
                if crate in ("ove", "embedded_hal_async") and crate in libs:
                    raise ValueError(f"Multiple Cargo artifacts for {crate}: {libs[crate]}, {filename}")
                libs[crate] = filename
    return libs, [flag for directory in dirs for flag in ("-L", f"dependency={directory}")]
