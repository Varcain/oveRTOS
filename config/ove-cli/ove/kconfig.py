# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Kconfig operations: menuconfig, defconfig, savedefconfig."""

import os
import sys

from .appgen import generate_app_kconfig
from .workspace import Workspace, find_ove_dir


def _find_external_app_for_defconfig(defconfig_path):
    """If defconfig_path lives under an external app, return that app dir."""
    ext_apps_env = os.environ.get("OVE_EXTERNAL_APPS", "")
    if not ext_apps_env:
        return None
    defconfig_abs = os.path.abspath(defconfig_path)
    for d in ext_apps_env.split(":"):
        d = d.strip()
        if not d:
            continue
        app_abs = os.path.abspath(d)
        if defconfig_abs.startswith(app_abs + os.sep):
            return app_abs
    return None


def cmd_menuconfig(args):
    """Run Kconfig menuconfig TUI."""
    ove_dir = find_ove_dir()
    os.chdir(ove_dir)

    config_path = os.path.join(ove_dir, ".config")
    os.environ.setdefault("KCONFIG_CONFIG", config_path)
    os.environ["srctree"] = ove_dir

    if os.path.isfile(config_path) and not os.path.islink(config_path):
        print("WARNING: .config is a regular file, not a workspace symlink.")
        print("  Run 'ove defconfig <name>' to use workspace separation.")
        print()

    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib not installed.")
        print("Install with: pip install kconfiglib")
        sys.exit(1)

    try:
        from menuconfig import menuconfig
    except ImportError:
        print("Error: menuconfig not installed.")
        print("Install with: pip install kconfiglib")
        sys.exit(1)

    generate_app_kconfig(ove_dir)
    kconf = kconfiglib.Kconfig("Config.in")
    menuconfig(kconf)


def cmd_defconfig(args):
    """Load a defconfig and set up workspace."""
    ove_dir = find_ove_dir()
    name = args.name

    # Ensure it ends with _defconfig
    if not name.endswith("_defconfig"):
        name = name + "_defconfig"

    # Find the defconfig file — search in-tree first, then external apps
    search_dirs = [os.path.join(ove_dir, "defconfigs")]
    ext_apps_env = os.environ.get("OVE_EXTERNAL_APPS", "")
    if ext_apps_env:
        for d in ext_apps_env.split(":"):
            d = d.strip()
            if d:
                ext_defconfigs = os.path.join(os.path.abspath(d), "defconfigs")
                if os.path.isdir(ext_defconfigs):
                    search_dirs.append(ext_defconfigs)

    defconfig_path = None
    for defconfig_dir in search_dirs:
        for root, dirs, files in os.walk(defconfig_dir):
            if name in files:
                defconfig_path = os.path.join(root, name)
                break
        if defconfig_path:
            break

    if not defconfig_path:
        print(f"Error: defconfig '{name}' not found in defconfigs/")
        sys.exit(1)

    # Parse workspace location from defconfig name.
    # Filename format: <board>_<rtos>_<app>[_zeroheap]_defconfig
    # Extract board by finding the first known RTOS name in the stem.
    stem = name.replace("_defconfig", "")
    valid_rtos = ("freertos", "nuttx", "zephyr", "posix")
    ws_board = None
    ws_rtos = None
    for rtos in valid_rtos:
        idx = stem.find(f"_{rtos}_")
        if idx >= 0:
            ws_board = stem[:idx]
            ws_rtos = rtos
            break
    if not ws_board or not ws_rtos:
        print(f"Error: cannot parse board/RTOS from defconfig '{name}'")
        sys.exit(1)

    ws_app = stem[len(ws_board) + 1 + len(ws_rtos) + 1:]
    if not ws_app:
        print(f"Error: could not parse app name from defconfig '{name}'")
        sys.exit(1)

    output_dir = os.path.join(ove_dir, "output")

    # Determine if this defconfig comes from an external app.
    # If so, place the workspace under the external app's output/ dir.
    ext_app_dir = _find_external_app_for_defconfig(defconfig_path)
    if ext_app_dir:
        ws_output = os.path.join(ext_app_dir, "output")
        ws_dir = os.path.join(ws_output, ws_board, ws_rtos, ws_app)
        print(f"Loading defconfig: {defconfig_path}")
        print(f"  Workspace: {ws_dir}/")
    else:
        ws_dir = os.path.join(output_dir, ws_board, ws_rtos, ws_app)
        rel_defconfig = os.path.relpath(defconfig_path, ove_dir)
        print(f"Loading defconfig: {rel_defconfig}")
        print(f"  Workspace: output/{ws_board}/{ws_rtos}/{ws_app}/")

    os.makedirs(ws_dir, exist_ok=True)

    # Use kconfiglib to load and expand the defconfig
    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib not installed.")
        sys.exit(1)

    os.environ["srctree"] = ove_dir
    generate_app_kconfig(ove_dir)
    kconf = kconfiglib.Kconfig(os.path.join(ove_dir, "Config.in"))
    kconf.load_config(defconfig_path)
    ws_config = os.path.join(ws_dir, ".config")
    kconf.write_config(ws_config)
    print(f"Configuration written to {ws_config}")

    # Symlink root .config -> workspace .config
    config_link = os.path.join(ove_dir, ".config")
    if os.path.islink(config_link) or os.path.exists(config_link):
        os.unlink(config_link)
    os.symlink(ws_config, config_link)

    # Symlink output/current -> workspace
    os.makedirs(output_dir, exist_ok=True)
    current_link = os.path.join(output_dir, "current")
    if ext_app_dir:
        # External app: use absolute path since workspace is outside output/
        target = ws_dir
    else:
        target = os.path.join(ws_board, ws_rtos, ws_app)
    if os.path.islink(current_link):
        os.unlink(current_link)
    os.symlink(target, current_link)

    # Link toolchain if available
    tc_sentinel = os.path.join(output_dir, "toolchains", "path.txt")
    if os.path.isfile(tc_sentinel):
        with open(tc_sentinel) as f:
            tc_path = f.read().strip()
        tc_name = os.path.basename(tc_path)
        tc_link = os.path.join(ws_dir, "toolchain")
        rel = os.path.relpath(
            os.path.join(output_dir, "toolchains", tc_name), ws_dir)
        if os.path.islink(tc_link):
            os.unlink(tc_link)
        if os.path.isdir(os.path.join(output_dir, "toolchains", tc_name)):
            os.symlink(rel, tc_link)

    print(f"Active workspace: {ws_dir}/")


def cmd_savedefconfig(args):
    """Save current config as minimal defconfig."""
    ove_dir = find_ove_dir()
    config_path = os.path.join(ove_dir, ".config")

    if not os.path.isfile(config_path):
        print("Error: .config not found. Run menuconfig or load a "
              "defconfig first.")
        sys.exit(1)

    try:
        import kconfiglib
    except ImportError:
        print("Error: kconfiglib not installed.")
        sys.exit(1)

    os.environ["srctree"] = ove_dir
    generate_app_kconfig(ove_dir)
    kconf = kconfiglib.Kconfig(os.path.join(ove_dir, "Config.in"))
    kconf.load_config(config_path)
    kconf.write_min_config(os.path.join(ove_dir, "defconfig"))
    print("Minimal config saved to defconfig")
