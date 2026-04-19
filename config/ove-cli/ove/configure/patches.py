# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""RTOS-tree patching.

Legacy entry point. ``build.py`` has its own ``_apply_patches`` that's
invoked on the actual download/build path. Kept here for callers that
still use the old API.
"""

import glob
import os
import subprocess
import sys

from ..workspace import get_bool


def apply_patches(patch_dir, target_dir, name):
    """Apply all numbered .patch files from patch_dir to target_dir."""
    if not os.path.isdir(patch_dir):
        return True

    patches = sorted(glob.glob(os.path.join(patch_dir, "*.patch")))
    if not patches:
        print(f"  {name}: no patches to apply")
        return True

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

    with open(sentinel, "w") as f:
        f.write("patched\n")
    return True


def apply_all_patches(ws):
    """Apply patches based on RTOS selection."""
    config = ws.config
    board_name = ws.board_name
    if not board_name:
        print("Error: CONFIG_OVE_BOARD_NAME not set in .config")
        sys.exit(1)

    board_base = os.path.join(ws.ove_dir, "boards", board_name)

    print("Applying patches...")
    ok = True

    if get_bool(config, "CONFIG_OVE_RTOS_FREERTOS"):
        stm32cube = os.path.join(ws.ws_dl_dir, "STM32CubeF7")
        if os.path.isdir(stm32cube):
            ok = apply_patches(
                os.path.join(board_base, "freertos", "patches"),
                stm32cube, "STM32CubeF7") and ok
        lvgl = os.path.join(ws.ws_dl_dir, "lvgl")
        if os.path.isdir(lvgl):
            ok = apply_patches(
                os.path.join(board_base, "freertos", "patches"),
                lvgl, "LVGL") and ok

    elif get_bool(config, "CONFIG_OVE_RTOS_ZEPHYR"):
        zephyr = os.path.join(ws.ws_dl_dir, "zephyr-workspace")
        if os.path.isdir(zephyr):
            ok = apply_patches(
                os.path.join(board_base, "zephyr", "patches"),
                zephyr, "Zephyr") and ok

    elif get_bool(config, "CONFIG_OVE_RTOS_NUTTX"):
        nuttx = os.path.join(ws.ws_dl_dir, "nuttx")
        if os.path.isdir(nuttx):
            ok = apply_patches(
                os.path.join(board_base, "nuttx", "patches"),
                nuttx, "NuttX") and ok

    elif get_bool(config, "CONFIG_OVE_RTOS_POSIX"):
        print("  POSIX: no patches needed")

    if not ok:
        print("\nSome patches failed to apply.")
        sys.exit(1)
    print("\nAll patches applied successfully.")
