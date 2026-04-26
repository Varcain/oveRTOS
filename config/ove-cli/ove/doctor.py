# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove doctor` — environment health check.

Verifies that everything an oveRTOS user needs is present and reachable
on the host — PATH binaries, downloaded toolchains under
`output/toolchains/` + `output/tools/`, and source tarballs under `dl/`
that the active workspace's .config points to.  Exit code: 0 on
green/yellow, 1 on red. JSON output via `--json` for CI consumption.
"""

import glob
import json
import logging
import os
import shutil
import subprocess
import sys

from .manifest import get_component, load_manifest
from .workspace import Workspace, find_ove_dir

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


def _check_downloaded_binary(display_name, path, version_cmd=None,
                             hint=None, required=False):
    """Binary that lives at a fixed path (inside output/toolchains/ or
    output/tools/), not necessarily in PATH.  `path` may be a glob."""
    matches = sorted(glob.glob(path))
    real = next((p for p in matches if os.path.isfile(p)), None)
    if not real:
        return {
            "name": display_name,
            "status": _WARN if not required else _FAIL,
            "detail": hint or f"not found at {path}",
        }
    version = ""
    if version_cmd:
        cmd = [real] + list(version_cmd)
        rc, out = _run(cmd)
        if rc == 0 and out:
            version = out.splitlines()[0]
    return {"name": display_name, "status": _OK, "path": real, "version": version}


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
                          ("jsonschema", False),
                          # pyserial — only needed by the manual HW test
                          # runner.  Surface a warning so devs know it's
                          # missing without failing the doctor check on
                          # machines that don't intend to do HIL work.
                          ("serial", False)]:
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


def _check_manifest_component(ove_dir, display, path_glob, required=False):
    """Check that a manifest-sourced source tree is present under dl/.
    `path_glob` is evaluated relative to `ove_dir/dl`. """
    dl_dir = os.path.join(ove_dir, "dl")
    full = os.path.join(dl_dir, path_glob)
    matches = glob.glob(full)
    if matches:
        return {"name": f"dl:{display}", "status": _OK,
                "path": matches[0]}
    return {"name": f"dl:{display}",
            "status": _FAIL if required else _WARN,
            "detail": f"not found under dl/ — run 'ove download'"}


def _checks():
    """Run all checks. Returns a list of result dicts."""
    out = [_check_python_version()]
    out += _check_python_pkgs()

    # ── Core build tools (required) ──────────────────────────────────
    out.append(_check_binary("cmake", ["cmake", "--version"]))
    out.append(_check_binary("ninja", ["ninja", "--version"], required=False))
    out.append(_check_binary("make", ["make", "--version"]))
    out.append(_check_binary("git", ["git", "--version"]))
    out.append(_check_binary("ccache", ["ccache", "--version"],
                             required=False))

    # ── Cross / native compilers ─────────────────────────────────────
    out.append(_check_binary("gcc", ["gcc", "--version"], required=False))
    out.append(_check_binary("clang", ["clang", "--version"], required=False))
    out.append(_check_binary("arm-none-eabi-gcc",
                             ["arm-none-eabi-gcc", "--version"],
                             required=False))

    # ── Bindings toolchains ──────────────────────────────────────────
    out.append(_check_binary("cargo", ["cargo", "--version"], required=False))
    out.append(_check_binary("rustc", ["rustc", "--version"], required=False))
    out.append(_check_binary("zig", ["zig", "version"], required=False))

    # ── RTOS / emulation (PATH binaries) ─────────────────────────────
    out.append(_check_binary("west", ["west", "--version"], required=False))
    out.append(_check_binary("kconfig-mconf",
                             ["kconfig-mconf", "--version"], required=False))
    out.append(_check_binary("qemu-system-arm",
                             ["qemu-system-arm", "--version"],
                             required=False))
    out.append(_check_binary("qemu-system-xtensa",
                             ["qemu-system-xtensa", "--version"],
                             required=False))
    # OpenOCD — only the manual HW test runner needs it (and the
    # `make flash` board script).  Optional on machines that don't
    # touch hardware.
    out.append(_check_binary("openocd", ["openocd", "--version"],
                             required=False))

    # ── Lint + format tools (optional; see `make lint`) ──────────────
    out.append(_check_binary("clang-format",
                             ["clang-format", "--version"], required=False))
    out.append(_check_binary("clang-tidy",
                             ["clang-tidy", "--version"], required=False))
    out.append(_check_binary("ruff",
                             ["ruff", "--version"], required=False))
    out.append(_check_binary("lcov",
                             ["lcov", "--version"], required=False))

    # ── Downloaded tools (output/toolchains, output/tools) ───────────
    ove_dir = find_ove_dir()
    out.append(_check_downloaded_binary(
        "arm-gnu (downloaded)",
        os.path.join(ove_dir, "output", "toolchains",
                     "arm-gnu-toolchain-*", "bin", "arm-none-eabi-gcc"),
        version_cmd=["--version"],
        hint="run 'ove download' to fetch the pinned toolchain"))
    out.append(_check_downloaded_binary(
        "zig (downloaded)",
        os.path.join(ove_dir, "output", "toolchains", "zig-*", "zig"),
        version_cmd=["version"],
        hint="run 'ove ensure-toolchain zig' to fetch"))
    out.append(_check_downloaded_binary(
        "renode (downloaded)",
        os.path.join(ove_dir, "output", "tools", "renode",
                     "renode_*_portable", "renode"),
        version_cmd=["--version"],
        hint="run 'ove ensure-toolchain renode' to fetch"))

    # ── Manifest components (source tarballs / clones under dl/) ─────
    manifest = load_manifest(ove_dir)
    if manifest:
        if get_component(manifest, "rtos", "freertos", "kernel-qemu"):
            out.append(_check_manifest_component(
                ove_dir, "FreeRTOS-Kernel", "FreeRTOS-Kernel-*"))
        if get_component(manifest, "rtos", "freertos", "stm32cubef7"):
            out.append(_check_manifest_component(
                ove_dir, "STM32CubeF7", "STM32CubeF7-*"))
        if get_component(manifest, "libraries", "lvgl"):
            out.append(_check_manifest_component(
                ove_dir, "lvgl", "lvgl-*"))
        if get_component(manifest, "libraries", "cmsis-dsp"):
            out.append(_check_manifest_component(
                ove_dir, "CMSIS-DSP", "CMSIS-DSP-*"))
        if get_component(manifest, "libraries", "mbedtls"):
            out.append(_check_manifest_component(
                ove_dir, "mbedtls", "mbedtls-*"))
        if get_component(manifest, "libraries", "lwip"):
            out.append(_check_manifest_component(
                ove_dir, "lwip", "lwip-*"))

    # ── Workspace state ──────────────────────────────────────────────
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
