# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""QEMU launch, display viewer, and browser bundle server."""

import os
import subprocess
import sys

from .workspace import Workspace, get_bool


def _get_config_bool(ws, key):
    return ws.config.get(key) is True


def _run_web(ws, args):
    """Serve the browser bundle (CONFIG_OVE_SIM_WEB) the build step wrote."""
    run_script = os.path.join(ws.workspace_dir, "serve", "run.sh")
    if not os.path.isfile(run_script):
        print("Error: serve/run.sh not found. Run 'ove build' first.")
        sys.exit(1)

    extra = args.extra if hasattr(args, "extra") else []
    os.execv("/bin/bash", ["bash", run_script] + extra)


def cmd_run(args):
    """CLI entry point for 'ove run'."""
    ws = Workspace()
    ws.require_config()

    rtos = ws.rtos
    if not rtos:
        print("Error: no RTOS selected in .config")
        sys.exit(1)

    # Browser bundle: serve it
    if _get_config_bool(ws, "CONFIG_OVE_SIM_WEB"):
        _run_web(ws, args)
        return

    firmware = os.path.join(ws.images_dir, "firmware.elf")

    # QEMU or hardware
    qemu_script = os.path.join(ws.board_dir, "qemu-run.sh")
    if os.path.isfile(qemu_script):
        if not os.path.isfile(firmware):
            print("Error: firmware.elf not found. Run 'ove build' first.")
            sys.exit(1)
        cmd = [qemu_script, firmware]
        headless = (hasattr(args, "headless") and args.headless) or \
            not get_bool(ws.config, "CONFIG_OVE_SIM_DASHBOARD")
        if headless:
            cmd.append("--headless")
        if hasattr(args, "extra"):
            cmd.extend(args.extra)
        os.execv(qemu_script, cmd)
    else:
        print(f"Error: no run method for board '{ws.board_name}' "
              f"with RTOS '{rtos}'")
        print("Use 'ove flash' for hardware targets.")
        sys.exit(1)


def _check_image_identity(ws, force):
    """Refuse to flash an image that is not the one this workspace builds.

    .config is a symlink into output/<board>/<rtos>/<app>/, so a defconfig or
    gate build can silently repoint the whole workspace; the float ABIs live
    inside .config and do not show up in any path at all. Flashing is the
    irreversible step, so verify identity here rather than trusting the layout.
    """
    from . import image_id

    reasons = image_id.mismatches(ws)
    if not reasons:
        ident = image_id.load(ws.images_dir)
        print(f"=== Image: {image_id.describe(ident)} ===")
        return

    print("Error: the built image does not match this workspace:",
          file=sys.stderr)
    for reason in reasons:
        print(f"  - {reason}", file=sys.stderr)
    if not force:
        print("\nRun 'ove build' to rebuild, or 'ove flash --force' to flash "
              "it anyway.", file=sys.stderr)
        sys.exit(1)
    print("\nflashing anyway (--force)", file=sys.stderr)


def cmd_flash(args):
    """CLI entry point for 'ove flash'."""
    ws = Workspace()
    ws.require_config()

    rtos = ws.rtos
    firmware = os.path.join(ws.images_dir, "firmware.elf")

    # Before either flash path: os.execv below replaces this process, so there
    # is no post-flash hook to check from.
    _check_image_identity(ws, getattr(args, "force", False))

    if rtos == "zephyr":
        west = os.path.join(ws.venv_dir, "bin", "west")
        env = ws.toolchain_env()
        zephyr_ws = os.path.join(ws.ws_dl_dir, "zephyr-workspace", "zephyr")
        if os.path.isdir(zephyr_ws):
            env["ZEPHYR_BASE"] = zephyr_ws
        fw_build = os.path.join(ws.build_dir, "firmware")
        print("=== Flashing Zephyr firmware ===")
        subprocess.run(
            [west, "flash", "-d", fw_build, "--runner", "openocd"],
            env=env)
        return

    # FreeRTOS or NuttX: use board flash.sh
    flash_sh = os.path.join(ws.board_dir, rtos, "flash.sh")
    if os.path.isfile(flash_sh):
        print(f"=== Flashing {rtos} firmware ===")
        os.execv(flash_sh, [flash_sh, firmware])
    else:
        print(f"Error: flash.sh not found for {rtos}")
        sys.exit(1)
