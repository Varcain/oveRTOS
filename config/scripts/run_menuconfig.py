#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Wrapper for kconfiglib menuconfig TUI."""

import os
import sys


def main():
    ove_dir = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    os.chdir(ove_dir)

    config_path = os.path.join(ove_dir, ".config")
    os.environ.setdefault("KCONFIG_CONFIG", config_path)
    os.environ["srctree"] = ove_dir

    if os.path.isfile(config_path) and not os.path.islink(config_path):
        print("WARNING: .config is a regular file, not a workspace symlink.")
        print("  Run 'make <name>_defconfig' to use workspace separation.")
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

    kconf = kconfiglib.Kconfig("Config.in")
    menuconfig(kconf)


if __name__ == "__main__":
    main()
