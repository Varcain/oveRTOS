# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Shared CLI utilities."""

import logging
import os
import subprocess
import sys

logger = logging.getLogger("ove")


def nproc() -> int:
    """Return number of CPUs for parallel operations."""
    try:
        return os.cpu_count() or 1
    except Exception:
        return 1


def run(cmd: list[str], *, env: dict | None = None, cwd: str | None = None,
        check: bool = True, capture: bool = False,
        log_file=None) -> subprocess.CompletedProcess:
    """Unified subprocess runner with consistent error handling.

    Args:
        cmd: Command and arguments
        env: Environment variables (defaults to os.environ)
        cwd: Working directory
        check: If True, exit on non-zero return code
        capture: If True, capture stdout/stderr
        log_file: If provided, stream output to terminal AND this file object
    """
    if log_file:
        log_file.write(f"$ {' '.join(cmd)}\n")
        log_file.flush()
        proc = subprocess.Popen(
            cmd, env=env, cwd=cwd,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for line in proc.stdout:
            text = line.decode("utf-8", errors="replace")
            sys.stdout.write(text)
            log_file.write(text)
        proc.wait()
        log_file.write(f"# exit {proc.returncode}\n\n")
        log_file.flush()
        result = subprocess.CompletedProcess(cmd, proc.returncode)
    elif capture:
        result = subprocess.run(cmd, env=env, cwd=cwd,
                                capture_output=True, text=True)
    else:
        result = subprocess.run(cmd, env=env, cwd=cwd)
    if check and result.returncode != 0:
        logger.error(f"command failed with exit code {result.returncode}")
        logger.error(f"  command: {' '.join(cmd)}")
        sys.exit(result.returncode)
    return result


def apply_defconfig_overlay(config_path: str, overlay_path: str) -> None:
    """Apply defconfig overlay by stripping existing keys and appending.

    Reads the overlay file, extracts CONFIG_* keys, removes matching lines
    from config_path, then appends the overlay content.
    """
    import re

    overlay_keys: set[str] = set()
    with open(overlay_path) as f:
        for line in f:
            m = re.match(r'^(CONFIG_\w+)', line)
            if m:
                overlay_keys.add(m.group(1))
            else:
                m = re.match(r'^# (CONFIG_\w+) is not set', line)
                if m:
                    overlay_keys.add(m.group(1))

    lines: list[str] = []
    with open(config_path) as f:
        for line in f:
            key = None
            m = re.match(r'^(CONFIG_\w+)', line)
            if m:
                key = m.group(1)
            else:
                m = re.match(r'^# (CONFIG_\w+) is not set', line)
                if m:
                    key = m.group(1)
            if key and key in overlay_keys:
                continue
            lines.append(line)

    with open(config_path, "w") as f:
        f.writelines(lines)
        with open(overlay_path) as ov:
            f.write(ov.read())

    # Validate no duplicate keys
    seen_keys = set()
    with open(config_path) as f:
        for line in f:
            m = re.match(r'^(CONFIG_\w+)', line)
            if m:
                k = m.group(1)
                if k in seen_keys:
                    logger.warning(f"Duplicate config key after overlay: {k}")
                seen_keys.add(k)
            else:
                m = re.match(r'^# (CONFIG_\w+) is not set', line)
                if m:
                    k = m.group(1)
                    if k in seen_keys:
                        logger.warning(f"Duplicate config key after overlay: {k}")
                    seen_keys.add(k)


def diff_configs(reference_path: str, current_path: str) -> str:
    """Compute the delta between two Kconfig .config files.

    Operates on raw text lines to preserve '# CONFIG_X is not set' syntax.
    Returns a defconfig/Kconfig fragment string with only the lines that
    differ between reference and current.
    """
    import re

    def _parse_config_lines(path):
        """Parse .config into {key: raw_line} dict."""
        entries = {}
        with open(path) as f:
            for line in f:
                line = line.rstrip("\n")
                m = re.match(r'^(CONFIG_\w+)=', line)
                if m:
                    entries[m.group(1)] = line
                    continue
                m = re.match(r'^# (CONFIG_\w+) is not set', line)
                if m:
                    entries[m.group(1)] = line
        return entries

    ref = _parse_config_lines(reference_path)
    cur = _parse_config_lines(current_path)

    delta_lines = []
    # Keys changed or added in current
    for key in sorted(set(cur.keys()) | set(ref.keys())):
        cur_line = cur.get(key)
        ref_line = ref.get(key)
        if cur_line != ref_line and cur_line is not None:
            delta_lines.append(cur_line)

    if not delta_lines:
        return ""

    header = (
        "# User RTOS customizations (from native menuconfig)\n"
        "# Regenerate with: make <rtos>-menuconfig\n"
    )
    return header + "\n".join(delta_lines) + "\n"
