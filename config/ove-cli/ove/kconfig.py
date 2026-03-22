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

    # Find the defconfig file
    defconfig_dir = os.path.join(ove_dir, "defconfigs")
    defconfig_path = None
    for root, dirs, files in os.walk(defconfig_dir):
        if name in files:
            defconfig_path = os.path.join(root, name)
            break

    if not defconfig_path:
        print(f"Error: defconfig '{name}' not found in defconfigs/")
        sys.exit(1)

    # Parse workspace location from defconfig name
    ws_board = os.path.basename(os.path.dirname(defconfig_path))
    stem = name.replace(f"{ws_board}_", "", 1).replace("_defconfig", "")

    # Extract RTOS from stem
    ws_rtos = stem.split("_")[0]
    valid_rtos = ("freertos", "nuttx", "zephyr", "posix")
    if ws_rtos not in valid_rtos:
        print(f"Error: unknown RTOS '{ws_rtos}' in defconfig name")
        sys.exit(1)

    ws_app = stem[len(ws_rtos) + 1:]
    if not ws_app:
        print(f"Error: could not parse app name from defconfig '{name}'")
        sys.exit(1)

    output_dir = os.path.join(ove_dir, "output")
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
    current_link = os.path.join(output_dir, "current")
    rel_ws = os.path.join(ws_board, ws_rtos, ws_app)
    if os.path.islink(current_link):
        os.unlink(current_link)
    os.symlink(rel_ws, current_link)

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

    print(f"Active workspace: output/{ws_board}/{ws_rtos}/{ws_app}/")


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
