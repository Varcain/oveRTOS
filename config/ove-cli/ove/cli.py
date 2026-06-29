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
        description=(
            "oveRTOS — write embedded RTOS applications in C++, Rust, "
            "Zig, or C; deploy on FreeRTOS, Zephyr, or Apache NuttX."
        ),
    )
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Enable debug logging")
    parser.add_argument("-q", "--quiet", action="store_true",
                        help="Suppress non-error output")
    sub = parser.add_subparsers(dest="command", help="Available commands")

    # ── defconfig ──────────────────────────────────────────────────────
    p = sub.add_parser("defconfig", help="Apply a defconfig and set up workspace")
    p.add_argument("name", help="Defconfig name (e.g. qemu_freertos_example_c)")

    # ── defconfig-fragments ──────────────────────────────────────────
    p = sub.add_parser("defconfig-fragments",
                       help="Configure from hierarchical fragments "
                            "(board.rtos.app)")
    p.add_argument("spec", help="board.rtos.app (e.g. qemu.freertos.example_c)")

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
    p = sub.add_parser("download",
                       help="Download RTOS sources and dependencies")
    p.add_argument("--dry-run", action="store_true",
                   help="List components that would be downloaded "
                        "and exit")

    # ── ensure-toolchain ───────────────────────────────────────────────
    p = sub.add_parser("ensure-toolchain",
                       help="Download a single toolchain / tool "
                            "(workspace-independent)")
    p.add_argument("name", choices=["zig", "zephyr-sdk", "renode"],
                   help="Toolchain/tool to ensure "
                        "(zig, zephyr-sdk, renode)")

    # ── configure ──────────────────────────────────────────────────────
    p = sub.add_parser("configure", help="Generate config files from .config")
    p.add_argument("--dry-run", action="store_true",
                   help="Print what would be generated, do not write files")

    # ── build ──────────────────────────────────────────────────────────
    p = sub.add_parser("build", help="Build firmware (auto-detects RTOS)")
    p.add_argument("--json", action="store_true",
                   help="Emit JSON build summary on stdout")
    p.add_argument("--dry-run", action="store_true",
                   help="Print the build target and exit without invoking "
                        "cmake/ninja")

    # ── allconfigs ─────────────────────────────────────────────────────
    from .allconfigs import add_subcommand as _add_allconfigs
    _add_allconfigs(sub)

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
                        "renode-stm32f746-freertos[-zeroheap], "
                        "qemu, renode, all)")
    p.add_argument("--json", action="store_true",
                   help="Emit JSON test summary on stdout")

    # ── benchmarks ─────────────────────────────────────────────────────
    p = sub.add_parser(
        "benchmarks",
        help="Build + run the benchmark suite across all bindings on a "
             "platform, then write a comparison report")
    p.add_argument("platform",
                   choices=["posix",
                            "stm32f746g-discovery",
                            "stm32f746g-discovery-nuttx",
                            "stm32f746g-discovery-zephyr"],
                   help="Target platform — 'posix' runs locally; "
                        "'stm32f746g-discovery' flashes the FreeRTOS "
                        "build, '*-nuttx' the NuttX build, '*-zephyr' "
                        "the Zephyr build.  All STM32 paths flash via "
                        "openocd and tail $OVE_SERIAL_LOG (default "
                        "/tmp/serial.log) for a picocom-recorded run.")
    p.add_argument("--skip-build", action="store_true",
                   help="Skip building (assume firmware already built)")
    p.add_argument("--binding", choices=["c", "cpp", "rust", "zig"],
                   action="append",
                   help="Restrict the run to one binding (repeatable).  "
                        "Other bindings' previously-captured logs are "
                        "left in place and the report is regenerated "
                        "from the union.  Useful for re-running a single "
                        "truncated binding without redoing the whole "
                        "suite.")
    p.add_argument("--zeroheap", action="store_true",
                   help="Build and run the zero-heap variants "
                        "(benchmark_zh / benchmark_cpp_zh / …) instead "
                        "of the default heap variants.  Logs land under "
                        "output/<board>/<rtos>/_benchmarks_zeroheap/ "
                        "and the report uses --page-mode zeroheap.")

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

    # ── doctor ────────────────────────────────────────────────────────
    p = sub.add_parser("doctor",
                       help="Check host environment (toolchains, deps, venv)")
    p.add_argument("--json", action="store_true",
                   help="Emit JSON instead of text")

    # ── completion ────────────────────────────────────────────────────
    p = sub.add_parser("completion",
                       help="Emit shell completion script (bash/zsh/fish)")
    p.add_argument("shell", choices=["bash", "zsh", "fish"])

    # ── lint / format ─────────────────────────────────────────────────
    p = sub.add_parser("lint",
                       help="Check formatting (clang-format / cargo fmt / "
                            "zig fmt / ruff)")
    p.add_argument("--only", default=None,
                   help="Run only the named check (e.g. clang-tidy-backends)")
    sub.add_parser("format",
                   help="Apply formatters in place (rewrites sources)")

    # ── ci ────────────────────────────────────────────────────────────
    p = sub.add_parser("ci",
                       help="Run pre-merge gates (doctor + lint + test all)")
    p.add_argument("--keep-going", action="store_true",
                   help="Run all stages even if earlier ones fail")

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

    # ── vscode ────────────────────────────────────────────────────────
    p = sub.add_parser("vscode",
                       help="Generate a VSCode project for the active "
                            "workspace and launch 'code'")
    p.add_argument("--no-open", action="store_true",
                   help="Generate .vscode/ files only, do not launch code")

    # ── app ────────────────────────────────────────────────────────────
    p = sub.add_parser("app", help="External-app scaffolding")
    app_sub = p.add_subparsers(dest="app_action")
    p_new = app_sub.add_parser(
        "new",
        help="Stamp a new external app from a template")
    p_new.add_argument("--lang", required=True,
                       choices=["c", "cpp", "rust", "zig"],
                       help="Application language")
    p_new.add_argument("--name", required=True,
                       help="App name (letters, digits, underscores, "
                            "hyphens; must start with a letter)")
    p_new.add_argument("--template", default="hello",
                       choices=["hello", "lvgl", "net", "audio"],
                       help="Template to stamp (default: hello)")
    p_new.add_argument("--dir", default=None,
                       help="Destination directory (default: ./<name>)")
    p_new.add_argument("--ove-dir", default=None,
                       help="Path to the oveRTOS checkout "
                            "(default: $OVE_DIR or the running checkout)")
    p_new.add_argument("--no-git", action="store_true",
                       help="Skip 'git init' inside the new app")
    p_new.add_argument("--force", action="store_true",
                       help="Overwrite a non-empty destination directory")

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

    elif args.command == "defconfig-fragments":
        from .kconfig import cmd_defconfig_fragments
        cmd_defconfig_fragments(args)

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

    elif args.command == "ensure-toolchain":
        from .download import cmd_ensure_toolchain
        cmd_ensure_toolchain(args)

    elif args.command == "configure":
        from .configure import cmd_configure
        cmd_configure(args)

    elif args.command == "build":
        from .build import cmd_build
        cmd_build(args)

    elif args.command == "allconfigs":
        from .allconfigs import cmd_allconfigs
        cmd_allconfigs(args)

    elif args.command == "run":
        from .run import cmd_run
        cmd_run(args)

    elif args.command == "flash":
        from .run import cmd_flash
        cmd_flash(args)

    elif args.command == "test":
        from .test import cmd_test
        cmd_test(args)

    elif args.command == "benchmarks":
        from .benchmarks import cmd_benchmarks
        cmd_benchmarks(args)

    elif args.command == "clean":
        _cmd_clean(args)

    elif args.command == "manifest":
        from .manifest import cmd_manifest
        cmd_manifest(args)

    elif args.command == "doctor":
        from .doctor import cmd_doctor
        cmd_doctor(args)

    elif args.command == "completion":
        from .completion import cmd_completion
        cmd_completion(args)

    elif args.command == "lint":
        from .lint import cmd_lint
        cmd_lint(args)

    elif args.command == "format":
        from .lint import cmd_format
        cmd_format(args)

    elif args.command == "ci":
        from .ci import cmd_ci
        cmd_ci(args)

    elif args.command == "board":
        from .board import cmd_board
        cmd_board(args)

    elif args.command == "vscode":
        from .vscode import cmd_vscode
        cmd_vscode(args)

    elif args.command == "app":
        if args.app_action == "new":
            from .app_new import cmd_app_new
            cmd_app_new(args)
        else:
            parser.print_help()
            sys.exit(1)

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
