# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""oveRTOS CLI — entry point with argparse subcommands."""

import argparse
import logging
import os
import shutil
import sys


def main():
    parser = argparse.ArgumentParser(
        prog="ove",
        description="oveRTOS RTOS Abstraction Framework — Build CLI",
    )
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Enable debug logging")
    parser.add_argument("-q", "--quiet", action="store_true",
                        help="Suppress non-error output")
    sub = parser.add_subparsers(dest="command", help="Available commands")

    # ── defconfig ──────────────────────────────────────────────────────
    p = sub.add_parser("defconfig", help="Apply a defconfig and set up workspace")
    p.add_argument("name", help="Defconfig name (e.g. qemu_freertos_example_c)")

    # ── menuconfig ─────────────────────────────────────────────────────
    sub.add_parser("menuconfig", help="Interactive configuration (TUI)")

    # ── rtos-menuconfig ────────────────────────────────────────────────
    p = sub.add_parser("rtos-menuconfig",
                       help="Launch native RTOS kernel menuconfig")
    p.add_argument("rtos", nargs="?",
                   help="RTOS to configure (nuttx, zephyr). "
                        "Auto-detected from .config if omitted.")

    # ── savedefconfig ──────────────────────────────────────────────────
    sub.add_parser("savedefconfig", help="Save current config as minimal defconfig")

    # ── download ───────────────────────────────────────────────────────
    sub.add_parser("download", help="Download RTOS sources and dependencies")

    # ── configure ──────────────────────────────────────────────────────
    sub.add_parser("configure", help="Generate config files from .config")

    # ── build ──────────────────────────────────────────────────────────
    sub.add_parser("build", help="Build firmware (auto-detects RTOS)")

    # ── run ────────────────────────────────────────────────────────────
    p = sub.add_parser("run", help="Run firmware (QEMU or native)")
    p.add_argument("--headless", action="store_true",
                   help="No display viewer (QEMU)")
    p.add_argument("extra", nargs="*", help="Extra arguments")

    # ── flash ──────────────────────────────────────────────────────────
    sub.add_parser("flash", help="Flash firmware to hardware")

    # ── test ───────────────────────────────────────────────────────────
    p = sub.add_parser("test", help="Run tests")
    p.add_argument("names", nargs="*",
                   help="Test names (stub, cpp, rust, freertos, nuttx, "
                        "zephyr, qemu-freertos, qemu-nuttx, qemu-zephyr, "
                        "qemu, all)")

    # ── clean ──────────────────────────────────────────────────────────
    p = sub.add_parser("clean", help="Clean build artifacts")
    p.add_argument("--all", action="store_true",
                   help="Clean all workspaces (entire output/)")
    p.add_argument("--dist", action="store_true",
                   help="Full reset (output/, dl/, .venv, .config)")

    # ── manifest ──────────────────────────────────────────────────────
    p = sub.add_parser("manifest",
                       help="Show manifest versions and integrity status")
    p.add_argument("--check", action="store_true",
                   help="Exit non-zero if manifest has uncommitted changes")

    # ── board ──────────────────────────────────────────────────────────
    p = sub.add_parser("board", help="Board import/sync tools")
    board_sub = p.add_subparsers(dest="board_action")
    board_sub.add_parser("list", help="List all boards")
    p_import = board_sub.add_parser("import", help="Import board from RTOS")
    p_import.add_argument("--rtos", required=True,
                          help="RTOS to import from (zephyr, nuttx)")
    p_import.add_argument("--name", required=True, help="Board name in RTOS")
    p_import.add_argument("--ove-name", help="oveRTOS board name")
    p_register = board_sub.add_parser("register",
                                      help="Re-run post-import registration")
    p_register.add_argument("--name", required=True, help="Board name")
    p_sync = board_sub.add_parser("sync", help="Sync board.yaml to RTOS configs")
    p_sync.add_argument("--name", help="Board name (omit for all)")
    p_sync.add_argument("--rtos", help="RTOS to sync (omit for all)")

    # ── Parse and dispatch ─────────────────────────────────────────────
    args = parser.parse_args()

    if args.verbose:
        level = logging.DEBUG
    elif args.quiet:
        level = logging.WARNING
    else:
        level = logging.INFO
    logging.basicConfig(
        level=level,
        format="[ove] %(levelname)s: %(message)s",
    )

    if not args.command:
        parser.print_help()
        sys.exit(0)

    if args.command == "defconfig":
        from .kconfig import cmd_defconfig
        cmd_defconfig(args)

    elif args.command == "menuconfig":
        from .kconfig import cmd_menuconfig
        cmd_menuconfig(args)

    elif args.command == "rtos-menuconfig":
        from .rtos_menuconfig import cmd_rtos_menuconfig
        cmd_rtos_menuconfig(args)

    elif args.command == "savedefconfig":
        from .kconfig import cmd_savedefconfig
        cmd_savedefconfig(args)

    elif args.command == "download":
        from .download import cmd_download
        cmd_download(args)

    elif args.command == "configure":
        from .configure import cmd_configure
        cmd_configure(args)

    elif args.command == "build":
        from .build import cmd_build
        cmd_build(args)

    elif args.command == "run":
        from .run import cmd_run
        cmd_run(args)

    elif args.command == "flash":
        from .run import cmd_flash
        cmd_flash(args)

    elif args.command == "test":
        from .test import cmd_test
        cmd_test(args)

    elif args.command == "clean":
        _cmd_clean(args)

    elif args.command == "manifest":
        from .manifest import cmd_manifest
        cmd_manifest(args)

    elif args.command == "board":
        from .board import cmd_board
        cmd_board(args)

    else:
        parser.print_help()
        sys.exit(1)


def _cmd_clean(args):
    """Clean build artifacts."""
    from .workspace import Workspace
    logger = logging.getLogger("ove")
    ws = Workspace()

    if args.dist:
        logger.info("Cleaning everything (output, downloads, venv, config)...")
        for d in (ws.output_dir, ws.dl_dir, ws.venv_dir):
            if os.path.isdir(d):
                shutil.rmtree(d)
        for f in (ws.config_path, ws.config_path + ".old"):
            if os.path.exists(f):
                os.unlink(f)

    elif args.all:
        logger.info("Cleaning all workspaces and toolchains...")
        if os.path.isdir(ws.output_dir):
            shutil.rmtree(ws.output_dir)
        if os.path.islink(ws.config_path) or os.path.isfile(ws.config_path):
            os.unlink(ws.config_path)

    else:
        logger.info("Cleaning active workspace build artifacts...")
        for d in (ws.build_dir, ws.gen_dir, ws.images_dir):
            if os.path.isdir(d):
                shutil.rmtree(d)
        tc_link = os.path.join(ws.workspace_dir, "toolchain")
        if os.path.islink(tc_link):
            os.unlink(tc_link)


if __name__ == "__main__":
    main()
