# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove doctor` — environment health check.

Verifies that everything an oveRTOS user needs is present and reachable
on the host. Exit code: 0 on green/yellow, 1 on red. JSON output via
`--json` for CI consumption.
"""

import json
import logging
import os
import shutil
import subprocess
import sys

from .workspace import Workspace

logger = logging.getLogger("ove")

_OK = "OK"
_WARN = "WARN"
_FAIL = "FAIL"


def _run(cmd):
    """Return (rc, stdout) with no stderr noise; (1, '') on missing binary."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=8)
        return r.returncode, (r.stdout or "").strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return 1, ""


def _check_binary(name, version_cmd=None, required=True):
    path = shutil.which(name)
    if not path:
        return {
            "name": name,
            "status": _FAIL if required else _WARN,
            "detail": "not found in PATH",
        }
    version = ""
    if version_cmd:
        rc, out = _run(version_cmd)
        if rc == 0 and out:
            version = out.splitlines()[0]
    return {"name": name, "status": _OK, "path": path, "version": version}


def _check_python_version():
    v = sys.version_info
    ok = v >= (3, 9)
    return {
        "name": "python",
        "status": _OK if ok else _FAIL,
        "version": f"{v.major}.{v.minor}.{v.micro}",
        "detail": "" if ok else "need >= 3.9",
    }


def _check_python_pkgs():
    results = []
    for mod, required in [("jinja2", True), ("yaml", True),
                          ("jsonschema", False)]:
        try:
            __import__(mod)
            results.append({"name": f"py:{mod}", "status": _OK})
        except ImportError:
            results.append({
                "name": f"py:{mod}",
                "status": _FAIL if required else _WARN,
                "detail": "missing — re-run 'make .venv'",
            })
    return results


def _check_workspace():
    try:
        ws = Workspace()
    except SystemExit:
        return {"name": "workspace", "status": _WARN,
                "detail": "no .config — run 'ove defconfig <name>' first"}
    info = {"name": "workspace", "status": _OK,
            "ove_dir": ws.ove_dir,
            "venv": os.path.isdir(ws.venv_dir),
            "config": os.path.isfile(ws.config_path)}
    if not info["venv"]:
        info["status"] = _WARN
        info["detail"] = "no .venv — run 'make .venv'"
    return info


def _checks():
    """Run all checks. Returns a list of result dicts."""
    out = [_check_python_version()]
    out += _check_python_pkgs()

    # Core build tools
    out.append(_check_binary("cmake", ["cmake", "--version"]))
    out.append(_check_binary("ninja", ["ninja", "--version"], required=False))
    out.append(_check_binary("make", ["make", "--version"]))
    out.append(_check_binary("git", ["git", "--version"]))
    out.append(_check_binary("ccache", ["ccache", "--version"],
                             required=False))

    # Cross / native compilers
    out.append(_check_binary("gcc", ["gcc", "--version"], required=False))
    out.append(_check_binary("clang", ["clang", "--version"], required=False))
    out.append(_check_binary("arm-none-eabi-gcc",
                             ["arm-none-eabi-gcc", "--version"],
                             required=False))

    # Bindings toolchains
    out.append(_check_binary("cargo", ["cargo", "--version"], required=False))
    out.append(_check_binary("rustc", ["rustc", "--version"], required=False))
    out.append(_check_binary("zig", ["zig", "version"], required=False))

    # RTOS / emulation tools
    out.append(_check_binary("west", ["west", "--version"], required=False))
    out.append(_check_binary("kconfig-mconf",
                             ["kconfig-mconf", "--version"], required=False))
    out.append(_check_binary("qemu-system-arm",
                             ["qemu-system-arm", "--version"],
                             required=False))
    out.append(_check_binary("qemu-system-xtensa",
                             ["qemu-system-xtensa", "--version"],
                             required=False))

    # Workspace state
    out.append(_check_workspace())
    return out


def _format_text(results):
    lines = []
    name_w = max(len(r["name"]) for r in results)
    for r in results:
        marker = {"OK": "\u2713", "WARN": "!", "FAIL": "\u2717"}[r["status"]]
        ver = r.get("version", "") or r.get("detail", "")
        lines.append(f"  [{marker}] {r['name']:<{name_w}}  {ver}")
    return "\n".join(lines)


def cmd_doctor(args):
    """CLI entry point for 'ove doctor'."""
    results = _checks()
    failed = [r for r in results if r["status"] == _FAIL]
    warned = [r for r in results if r["status"] == _WARN]

    if getattr(args, "json", False):
        json.dump({"results": results,
                   "failed": [r["name"] for r in failed],
                   "warned": [r["name"] for r in warned]},
                  sys.stdout, indent=2)
        print()
    else:
        print("oveRTOS environment check")
        print("=" * 40)
        print(_format_text(results))
        print("=" * 40)
        if failed:
            print(f"FAIL: {len(failed)} required tool(s) missing — install "
                  "them before building.")
        elif warned:
            print(f"OK with {len(warned)} optional tool(s) missing — some "
                  "RTOS / language paths will be unavailable.")
        else:
            print("All checks passed.")

    sys.exit(1 if failed else 0)
