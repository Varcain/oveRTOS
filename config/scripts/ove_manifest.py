# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Build manifest for the sim dashboard.

What a firmware build has, so the dashboard can draw the board and lay
itself out once instead of discovering panels as their first frames
arrive: board hardware from boards/<board>/board.yaml, enabled features
from the workspace .config, and the build ID.

The dashboard fetches it as manifest.json. The local bridge
(ove-dashboard-bridge.py) serves it live; the browser bundle (ove build,
web variant) writes it next to index.html. Every field is optional for the
consumer, which falls back to a generic board.
"""

import os
import re

# Kconfig symbol -> manifest feature flag.
FEATURES = {
    "display":  "CONFIG_OVE_LVGL",
    "audio":    "CONFIG_OVE_AUDIO",
    "trace":    "CONFIG_OVE_TRACE_STREAM",
    "profiler": "CONFIG_OVE_PROFILER",
    "net":      "CONFIG_OVE_NET",
    "fs":       "CONFIG_OVE_FS",
    "gpio":     "CONFIG_OVE_GPIO",
    "i2c":      "CONFIG_OVE_I2C",
    "spi":      "CONFIG_OVE_SPI",
    "infer":    "CONFIG_OVE_INFER",
}

# board.yaml sections passed through to the dashboard's board drawing.
BOARD_SECTIONS = ("clock", "memory", "display", "audio", "serial", "leds",
                  "buttons", "gpio", "sdcard", "ethernet")


def _is_workspace_config(path):
    """True if *path* is an oveRTOS workspace .config.

    Vendored RTOS trees keep their own .config inside the build directory
    (NuttX writes <ws>/build/nuttx-cmake/.config), so the presence of a
    .config alone does not identify the workspace."""
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("CONFIG_OVE_BOARD_NAME="):
                    return True
    except OSError:
        pass
    return False


def find_workspace(start_dir):
    """Walk up from *start_dir* (e.g. the CMake build dir) to the
    workspace directory holding .config; None if there is none."""
    d = os.path.abspath(start_dir)
    for _ in range(4):
        if _is_workspace_config(os.path.join(d, ".config")):
            return d
        d = os.path.dirname(d)
    return None


def read_kconfig(path):
    cfg = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line.startswith("CONFIG_") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                if v.startswith('"') and v.endswith('"'):
                    v = v[1:-1]
                cfg[k] = v
    except OSError:
        pass
    return cfg


def build(workspace_dir, ove_dir, debugger, console, web=False, log=print):
    """Return the manifest dict for the build in *workspace_dir*.

    *debugger*: a GDB is attached (run control works). *console*: typed
    input reaches the firmware's console UART. *web*: the firmware runs in
    the page (browser bundle) rather than under the local bridge.
    """
    m = {"version": 1, "web": bool(web), "features": {}, "board": {}}
    cfg = read_kconfig(os.path.join(workspace_dir, ".config")) if workspace_dir else {}

    m["app"] = cfg.get("CONFIG_OVE_APP_NAME")
    m["lang"] = cfg.get("CONFIG_OVE_APP_LANG")
    for rtos in ("FREERTOS", "ZEPHYR", "NUTTX"):
        if cfg.get("CONFIG_OVE_RTOS_" + rtos) == "y":
            m["rtos"] = rtos.lower()
    m["zero_heap"] = cfg.get("CONFIG_OVE_ZERO_HEAP") == "y"
    for feat, sym in FEATURES.items():
        m["features"][feat] = cfg.get(sym) == "y"
    m["features"]["debugger"] = bool(debugger)
    m["features"]["console"] = bool(console)
    if cfg.get("CONFIG_OVE_PROFILER_HZ"):
        m["profiler_hz"] = int(cfg["CONFIG_OVE_PROFILER_HZ"])

    if workspace_dir:
        bid = os.path.join(workspace_dir, "generated", "ove_build_id.h")
        try:
            with open(bid) as f:
                mt = re.search(r'#define OVE_BUILD_ID "([^"]*)"', f.read())
                if mt:
                    m["build_id"] = mt.group(1)
        except OSError:
            pass

    board_name = cfg.get("CONFIG_OVE_BOARD_NAME")
    if board_name:
        m["board"]["name"] = board_name
        yml = os.path.join(ove_dir, "boards", board_name, "board.yaml")
        try:
            import yaml  # in the ove venv both callers run from
            with open(yml) as f:
                desc = yaml.safe_load(f) or {}
            b = desc.get("board", {}) or {}
            m["board"].update({
                "title": b.get("title"),
                "mcu": b.get("mcu"),
                "mcu_family": b.get("mcu_family"),
            })
            for key in BOARD_SECTIONS:
                if key in desc:
                    m["board"][key] = desc[key]
        except Exception as e:  # no yaml module or file: generic board
            log(f"[manifest] board.yaml not loaded: {e}")
    return m
