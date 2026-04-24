# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""`ove vscode` — generate a VSCode project scoped to the active workspace.

Writes .vscode/{settings,tasks,launch,extensions}.json pointing at the
currently-active board+RTOS+app (resolved from .config), then launches
`code` on the repo root. Regenerates on every run; clobbers user edits
after printing a warning.
"""

import json
import logging
import os
import shutil
import subprocess
import sys

from .workspace import Workspace, get_bool


# OpenOCD board.cfg lookup. Extend here when new hardware targets land;
# keeps launch.json generation out of board.yaml parsing.
_OPENOCD_BOARD_CFG = {
    "stm32f746g-discovery": "board/stm32f7discovery.cfg",
}


def cmd_vscode(args):
    """CLI entry point for 'ove vscode'."""
    logger = logging.getLogger("ove")
    ws = Workspace()
    try:
        ws.require_config()
    except FileNotFoundError:
        logger.error(
            ".config not present. Run 'make <board>.<rtos>.<app>' first "
            "(e.g. make qemu.freertos.example_c).")
        sys.exit(1)

    vscode_dir = os.path.join(ws.ove_dir, ".vscode")
    os.makedirs(vscode_dir, exist_ok=True)

    elf = _firmware_elf(ws)
    posix = _posix_bin(ws)
    if ws.rtos != "posix" and not _is_wasm_board(ws) \
            and not os.path.isfile(elf):
        logger.warning(
            f"{elf} not present — debug launches will fail until you run "
            f"'make build'. Generating configs anyway.")
    elif ws.rtos == "posix" and not os.path.isfile(posix):
        logger.warning(
            f"{posix} not present — POSIX debug/run launches will fail "
            f"until 'make build'. Generating configs anyway.")

    overwritten = []
    _write_json(os.path.join(vscode_dir, "settings.json"),
                _build_settings_json(ws), overwritten)
    _write_json(os.path.join(vscode_dir, "tasks.json"),
                _build_tasks_json(ws), overwritten)
    _write_json(os.path.join(vscode_dir, "launch.json"),
                _build_launch_json(ws), overwritten)
    _write_json(os.path.join(vscode_dir, "extensions.json"),
                _build_extensions_json(ws), overwritten)

    # External app → emit a multi-root workspace file pulling in both
    # the oveRTOS tree and the external app dir. The folder-level
    # .vscode/*.json written above still provides tasks/launch/settings,
    # which VSCode merges from the first folder in multi-root mode.
    workspace_file = None
    if _is_external_app(ws):
        workspace_file = os.path.join(vscode_dir, "ove.code-workspace")
        _write_json(workspace_file, _build_code_workspace(ws), overwritten)

    if overwritten:
        logger.warning(
            "Overwrote existing .vscode files: " + ", ".join(overwritten) +
            " — any hand edits are lost.")

    opened = False
    target = workspace_file if workspace_file else ws.ove_dir
    if not getattr(args, "no_open", False):
        code = _find_code_cli()
        if not code:
            _install_hint_and_exit()
        # WSL-installed VSCode prompts "[y/N]" on stdin unless this env
        # var is set. Without it, a non-TTY stdin silently answers N and
        # the launcher exits before opening a window.
        env = dict(os.environ, DONT_PROMPT_WSL_INSTALL="1")
        try:
            subprocess.Popen([code, target],
                             stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL,
                             start_new_session=True,
                             env=env)
            opened = True
        except OSError as e:
            logger.error(f"Failed to launch 'code': {e}")
            sys.exit(1)

    _print_summary(ws, overwritten, opened, target, workspace_file)


def _write_json(path, data, overwritten):
    if os.path.exists(path):
        overwritten.append(os.path.basename(path))
    with open(path, "w") as f:
        json.dump(data, f, indent=2, sort_keys=False)
        f.write("\n")


def _find_code_cli():
    return shutil.which("code")


def _install_hint_and_exit():
    msg = """
ERROR: 'code' command not found in PATH.

Install VSCode and make sure the 'code' CLI shim is on your PATH:

  Ubuntu/Debian (apt):   sudo apt install code
                         (after adding Microsoft's apt repo; see
                          https://code.visualstudio.com/docs/setup/linux)
  Ubuntu/Debian (.deb):  https://code.visualstudio.com/Download
  Fedora / RHEL:         sudo dnf install code       (after adding repo)
  Arch:                  sudo pacman -S code         (open-source build)
                         or install 'visual-studio-code-bin' from AUR
  Snap:                  sudo snap install code --classic
  macOS (Homebrew):      brew install --cask visual-studio-code

Once installed, open VSCode and run the command:
  'Shell Command: Install "code" command in PATH'
from the Command Palette (Ctrl+Shift+P).
"""
    print(msg, file=sys.stderr)
    sys.exit(1)


def _resolve_gdb(ws):
    tc = ws.toolchain_dir
    if tc:
        cand = os.path.join(tc, "bin", "arm-none-eabi-gdb")
        if os.path.isfile(cand):
            return cand
    on_path = shutil.which("arm-none-eabi-gdb")
    return on_path or "arm-none-eabi-gdb"


def _firmware_elf(ws):
    return os.path.join(ws.images_dir, "firmware.elf")


def _posix_bin(ws):
    return os.path.join(ws.images_dir, "ove_posix")


def _is_qemu_board(ws):
    return bool(ws.board_dir) and os.path.isfile(
        os.path.join(ws.board_dir, "qemu-run.sh"))


def _is_wasm_board(ws):
    return get_bool(ws.config, "CONFIG_OVE_BOARD_WASM")


def _is_hardware_board(ws):
    rtos = ws.rtos
    if not rtos or not ws.board_dir:
        return False
    return os.path.isfile(os.path.join(ws.board_dir, rtos, "flash.sh"))


def _is_external_app(ws):
    """True when the active app lives outside the oveRTOS tree."""
    d = ws.app_dir
    if not d or not os.path.isdir(d):
        return False
    ove = os.path.realpath(ws.ove_dir)
    app = os.path.realpath(d)
    return not (app == ove or app.startswith(ove + os.sep))


def _build_code_workspace(ws):
    """Multi-root .code-workspace listing both the oveRTOS repo and the app.

    Settings and extensions are embedded at workspace level so they apply
    to every folder — otherwise clangd launched on a file in the external
    app folder would not see --compile-commands-dir and fail to resolve
    `#include "ove/..."` into the oveRTOS tree.
    """
    return {
        "folders": [
            {"path": ws.ove_dir, "name": "oveRTOS"},
            {"path": ws.app_dir, "name": f"app: {ws.app_name}"},
        ],
        "settings": _build_settings_json(ws),
        "extensions": _build_extensions_json(ws),
    }


def _openocd_cfg_for(ws):
    return _OPENOCD_BOARD_CFG.get(ws.board_name, "board/stm32f7discovery.cfg")


def _build_settings_json(ws):
    # Absolute path so clangd finds the same compile DB regardless of
    # which folder in the workspace it was launched in (matters for
    # external-app multi-root workspaces).
    cc_dir = ws.output_dir
    settings = {
        "clangd.arguments": [
            f"--compile-commands-dir={cc_dir}",
            "--background-index",
            "--header-insertion=never",
            "--completion-style=detailed",
            "--pch-storage=memory",
        ],
        "C_Cpp.intelliSenseEngine": "disabled",
        "files.associations": {
            "*.h": "c",
            "*.inl": "c",
            "Kconfig": "kconfig",
            "Config.in": "kconfig",
            "*.defconfig": "properties",
            "*.ld": "linkerscript",
        },
        "files.exclude": {
            "**/__pycache__": True,
            "**/.cache": True,
            "output/*/build": True,
            "dl": True,
            ".venv": True,
        },
        "search.exclude": {
            "output": True,
            "dl": True,
            ".venv": True,
            "docs-site/site": True,
            "**/target": True,
        },
        "editor.formatOnSave": False,
        "[c]":    {"editor.defaultFormatter":
                   "llvm-vs-code-extensions.vscode-clangd"},
        "[cpp]":  {"editor.defaultFormatter":
                   "llvm-vs-code-extensions.vscode-clangd"},
        "[python]": {"editor.defaultFormatter": "ms-python.python"},
        "ove.activeBoard": ws.board_name,
        "ove.activeRtos":  ws.rtos,
        "ove.activeApp":   ws.app_name,
        "ove.activeAppLang": ws.app_lang,
    }
    if ws.app_lang == "rust":
        settings["rust-analyzer.linkedProjects"] = [
            "bindings/rust/ove/Cargo.toml",
        ]
        settings["rust-analyzer.check.allTargets"] = False
    return settings


def _build_extensions_json(ws):
    recommendations = [
        "llvm-vs-code-extensions.vscode-clangd",
        "ms-python.python",
        "ms-vscode.makefile-tools",
        "twxs.cmake",
    ]
    if _is_wasm_board(ws):
        pass
    elif ws.rtos == "posix":
        recommendations.append("vadimcn.vscode-lldb")
    else:
        recommendations.append("marus25.cortex-debug")

    if ws.app_lang == "rust":
        recommendations.append("rust-lang.rust-analyzer")
    elif ws.app_lang == "zig":
        recommendations.append("ziglang.vscode-zig")

    return {"recommendations": recommendations}


def _build_tasks_json(ws):
    def make(label, args, group=None, problem="$gcc"):
        t = {
            "label": label,
            "type": "shell",
            "command": "make",
            "args": args,
            "options": {"cwd": "${workspaceFolder}"},
            "problemMatcher": problem,
            "presentation": {
                "reveal": "always",
                "panel": "shared",
                "clear": False,
            },
        }
        if group:
            t["group"] = group
        return t

    tasks = [
        make("ove: build", ["build"],
             group={"kind": "build", "isDefault": True}),
        make("ove: clean", ["clean"], problem=[]),
        make("ove: test", ["test"],
             group={"kind": "test", "isDefault": True}),
        make("ove: run", ["run"], problem=[]),
        make("ove: run (headless)", ["run", "HEADLESS=1"], problem=[]),
        make("ove: flash", ["flash"], problem=[]),
        make("ove: menuconfig", ["menuconfig"], problem=[]),
        make("ove: docs", ["docs"], problem=[]),
        make("ove: docs-serve", ["docs-serve"], problem=[]),
        {
            "label": "ove: open API docs",
            "type": "shell",
            "command": (
                "sh -c 'f=docs-site/site/index.html; "
                "[ -f \"$f\" ] || make docs; "
                "(xdg-open \"$f\" 2>/dev/null || open \"$f\" 2>/dev/null || "
                "echo \"Open docs-site/site/index.html in your browser.\")'"
            ),
            "options": {"cwd": "${workspaceFolder}"},
            "problemMatcher": [],
            "presentation": {"reveal": "silent", "panel": "shared"},
        },
    ]

    if _is_hardware_board(ws):
        tasks.append({
            "label": "ove: openocd (stm32)",
            "type": "shell",
            "command": os.path.join(ws.board_dir, ws.rtos, "debug.sh"),
            "isBackground": True,
            "problemMatcher": {
                "pattern": {"regexp": "^.*$", "file": 1, "location": 2,
                            "message": 3},
                "background": {
                    "activeOnStart": True,
                    "beginsPattern": ".*Info : Listening on port 3333.*",
                    "endsPattern":   ".*Info : Listening on port 3333.*",
                },
            },
            "presentation": {"reveal": "silent", "panel": "dedicated"},
        })

    return {"version": "2.0.0", "tasks": tasks}


def _build_launch_json(ws):
    elf = _firmware_elf(ws)
    posix = _posix_bin(ws)
    gdb = _resolve_gdb(ws)
    configurations = []

    if _is_wasm_board(ws):
        pass

    elif ws.rtos == "posix":
        configurations.append({
            "name": "Debug POSIX (CodeLLDB)",
            "type": "lldb",
            "request": "launch",
            "program": posix,
            "args": [],
            "cwd": "${workspaceFolder}",
            "env": {"OVE_DIR": ws.ove_dir},
            "preLaunchTask": "ove: build",
        })
        configurations.append({
            "name": "Debug POSIX (gdb)",
            "type": "cppdbg",
            "request": "launch",
            "program": posix,
            "args": [],
            "cwd": "${workspaceFolder}",
            "environment": [{"name": "OVE_DIR", "value": ws.ove_dir}],
            "MIMode": "gdb",
            "preLaunchTask": "ove: build",
        })

    elif _is_qemu_board(ws):
        configurations.append({
            "name": "Debug QEMU (cortex-debug, attach)",
            "cwd": "${workspaceFolder}",
            "executable": elf,
            "request": "attach",
            "type": "cortex-debug",
            "servertype": "external",
            "gdbTarget": "localhost:1234",
            "gdbPath": gdb,
            "runToEntryPoint": "main",
            "showDevDebugOutput": "none",
        })
        configurations.append({
            "name": "Debug QEMU (cortex-debug, launch via make run)",
            "cwd": "${workspaceFolder}",
            "executable": elf,
            "request": "attach",
            "type": "cortex-debug",
            "servertype": "external",
            "gdbTarget": "localhost:1234",
            "gdbPath": gdb,
            "preLaunchTask": "ove: run (headless)",
            "runToEntryPoint": "main",
            "showDevDebugOutput": "none",
        })

    elif _is_hardware_board(ws):
        configurations.append({
            "name": f"Debug {ws.board_name} (cortex-debug / openocd)",
            "cwd": "${workspaceFolder}",
            "executable": elf,
            "request": "launch",
            "type": "cortex-debug",
            "servertype": "openocd",
            "configFiles": [_openocd_cfg_for(ws)],
            "gdbPath": gdb,
            "runToEntryPoint": "main",
            "showDevDebugOutput": "none",
            "preLaunchTask": "ove: build",
        })
        configurations.append({
            "name": f"Attach {ws.board_name} (cortex-debug / openocd)",
            "cwd": "${workspaceFolder}",
            "executable": elf,
            "request": "attach",
            "type": "cortex-debug",
            "servertype": "openocd",
            "configFiles": [_openocd_cfg_for(ws)],
            "gdbPath": gdb,
            "showDevDebugOutput": "none",
        })

    return {"version": "0.2.0", "configurations": configurations}


def _print_summary(ws, overwritten, opened, target, workspace_file):
    print()
    print("Generated VSCode project:")
    print(f"  board: {ws.board_name}")
    print(f"  rtos:  {ws.rtos}")
    print(f"  app:   {ws.app_name} ({ws.app_lang})")
    print(f"  dir:   {os.path.join(ws.ove_dir, '.vscode')}")
    if workspace_file:
        print(f"  mode:  multi-root (external app)")
        print(f"  app dir: {ws.app_dir}")
        print(f"  workspace: {workspace_file}")
    else:
        print(f"  mode:  single-folder")
    if overwritten:
        print(f"  note:  overwrote {len(overwritten)} existing file(s)")
    if opened:
        print(f"  launched: code {target}")
    else:
        print(f"  (--no-open used; run 'code {target}' to open)")
