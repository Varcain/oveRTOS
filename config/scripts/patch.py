#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Apply numbered patches to extracted sources."""

import argparse
import os
import sys
import subprocess
import glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from genconfig import parse_dotconfig, get_config_bool, get_config_str


def apply_patches(patch_dir, target_dir, name):
    """Apply all numbered .patch files from patch_dir to target_dir."""
    if not os.path.isdir(patch_dir):
        return True

    patches = sorted(glob.glob(os.path.join(patch_dir, "*.patch")))
    if not patches:
        print(f"  {name}: no patches to apply")
        return True

    # Sentinel file to track already-applied patches
    sentinel = os.path.join(target_dir, ".ove_patched")
    if os.path.isfile(sentinel):
        print(f"  {name}: patches already applied")
        return True

    print(f"  {name}: applying {len(patches)} patch(es)...")
    for p in patches:
        pname = os.path.basename(p)
        ret = subprocess.run(
            ["patch", "-p1", "-d", target_dir, "-i", os.path.abspath(p)],
            capture_output=True, text=True)
        if ret.returncode != 0:
            print(f"  ERROR: patch {pname} failed:")
            print(f"  {ret.stderr}")
            return False
        print(f"    Applied: {pname}")

    # Write sentinel
    with open(sentinel, "w") as f:
        f.write("patched\n")

    return True


def main():
    parser = argparse.ArgumentParser(description="Apply patches to sources")
    parser.add_argument("--config", default=".config",
                        help="Path to .config file")
    parser.add_argument("--workspace-dir", default=None,
                        help="Workspace directory (patches use <workspace>/dl/ symlinks)")
    args = parser.parse_args()

    ove_dir = os.environ.get("OVE_DIR",
        os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))))

    config_path = os.path.join(ove_dir, args.config)
    if args.workspace_dir:
        dl_dir = os.path.join(args.workspace_dir, "dl")
    else:
        dl_dir = os.path.join(ove_dir, "dl")

    if not os.path.isfile(config_path):
        print("Error: .config not found.")
        sys.exit(1)

    config = parse_dotconfig(config_path)
    ok = True

    # Resolve board directory: boards/<board-name>/
    board_name = get_config_str(config, "CONFIG_OVE_BOARD_NAME")
    if not board_name:
        print("Error: CONFIG_OVE_BOARD_NAME not set in .config")
        sys.exit(1)
    board_base = os.path.join(ove_dir, "boards", board_name)

    print("Applying patches...")

    if get_config_bool(config, "CONFIG_OVE_RTOS_FREERTOS"):
        stm32cube = os.path.join(dl_dir, "STM32CubeF7")
        if os.path.isdir(stm32cube):
            ok = apply_patches(
                os.path.join(board_base, "freertos", "patches"),
                stm32cube, "STM32CubeF7") and ok

        lvgl = os.path.join(dl_dir, "lvgl")
        if os.path.isdir(lvgl):
            ok = apply_patches(
                os.path.join(board_base, "freertos", "patches"),
                lvgl, "LVGL") and ok

    elif get_config_bool(config, "CONFIG_OVE_RTOS_ZEPHYR"):
        zephyr = os.path.join(dl_dir, "zephyr-workspace")
        if os.path.isdir(zephyr):
            ok = apply_patches(
                os.path.join(board_base, "zephyr", "patches"),
                zephyr, "Zephyr") and ok

    elif get_config_bool(config, "CONFIG_OVE_RTOS_NUTTX"):
        nuttx = os.path.join(dl_dir, "nuttx")
        if os.path.isdir(nuttx):
            ok = apply_patches(
                os.path.join(board_base, "nuttx", "patches"),
                nuttx, "NuttX") and ok

    elif get_config_bool(config, "CONFIG_OVE_RTOS_POSIX"):
        print("  POSIX: no patches needed")

    if not ok:
        print("\nSome patches failed to apply.")
        sys.exit(1)

    print("\nAll patches applied successfully.")


if __name__ == "__main__":
    main()
