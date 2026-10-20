# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Locate and inspect the repository's Python virtual environment.

The venv is created by the top-level Makefile at <ove_dir>/.venv.  Keep
the path here so the CLI modules do not each hard-code it.
"""

import os
import subprocess

VENV_DIRNAME = ".venv"

# Zephyr's CMake build refuses older interpreters (zephyr/cmake/modules/
# python.cmake).  The pinned Zephyr 4.4 requires 3.12.
ZEPHYR_MIN_PYTHON = (3, 12)


def path(ove_dir):
    """Absolute path of the venv directory."""
    return os.path.join(ove_dir, VENV_DIRNAME)


def bin_path(ove_dir, name=None):
    """Path of the venv's bin directory, or a program inside it."""
    bin_dir = os.path.join(path(ove_dir), "bin")
    return os.path.join(bin_dir, name) if name else bin_dir


def python_path(ove_dir):
    """Path of the venv's Python interpreter."""
    return bin_path(ove_dir, "python")


def python_version(executable):
    """Return (major, minor, patch) for *executable*, or None."""
    if not executable or not os.path.isfile(executable):
        return None
    try:
        ret = subprocess.run(
            [executable, "-c",
             "import sys; print('%d %d %d' % sys.version_info[:3])"],
            capture_output=True, text=True, timeout=8)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if ret.returncode != 0:
        return None
    try:
        return tuple(int(part) for part in ret.stdout.split())
    except ValueError:
        return None


def format_version(version):
    return ".".join(str(part) for part in version)


# What to run when the venv is missing or too old.  `make setup` picks an
# interpreter >= ZEPHYR_MIN_PYTHON and rebuilds a venv made by another
# Python version.
SETUP_HINT = ("run 'make setup' (or 'make OVE_PYTHON=/path/to/python3.12 "
              "setup' to choose the interpreter)")


def zephyr_error(ove_dir):
    """Return a fix-it message if the venv cannot build Zephyr, else None."""
    exe = python_path(ove_dir)
    version = python_version(exe)
    if version is None:
        return f"{exe} not found — {SETUP_HINT}"
    if version < ZEPHYR_MIN_PYTHON:
        need = format_version(ZEPHYR_MIN_PYTHON)
        return (f"{path(ove_dir)} uses Python {format_version(version)}; "
                f"Zephyr needs >= {need}. To recreate it, {SETUP_HINT}")
    return None
