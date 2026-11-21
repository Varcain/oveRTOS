#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Board Import Tool for oveRTOS.

Subcommands:
    import zephyr  --board <name>  [--zephyr-base <path>] [--ove-name <name>]
    import nuttx   --board <name>  [--nuttx-base <path>]  [--config <variant>] [--ove-name <name>]
    list

Examples:
    board_tool.py import zephyr --board stm32f746g_disco
    board_tool.py import nuttx  --board stm32f746g-disco --config nsh
    board_tool.py list
"""

import argparse
import contextlib
import copy
import datetime
import json
import os
import pickle
import re
import shutil
import signal
import subprocess
import sys
import textwrap
from collections import deque
from pathlib import Path

from ove.download import find_zephyr_sdk, zephyr_sdk_env
from ove.manifest import get_component, load_manifest
from ove.utils import hashed_dir
from ove.workspace import parse_dotconfig

try:
    import yaml
except ImportError:
    print("Error: pyyaml not installed. Install with: pip install pyyaml")
    sys.exit(1)

try:
    import jsonschema
except ImportError:
    print("Error: jsonschema not installed. Install with: pip install jsonschema")
    sys.exit(1)

try:
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    print("Error: jinja2 not installed. Install with: pip install jinja2")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------

def ove_dir():
    """Return OVE_DIR from env or derive from script location."""
    return os.environ.get(
        "OVE_DIR",
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    )


def boards_dir():
    return os.path.join(ove_dir(), "boards")


def dl_dir():
    return os.path.join(ove_dir(), "dl")


# ---------------------------------------------------------------------------
# BoardYaml helper
# ---------------------------------------------------------------------------

class BoardYaml:
    """Load, validate, save, and merge board.yaml files."""

    def __init__(self, data=None, path=None):
        self.data = data or {}
        self.path = path

    # -- I/O ----------------------------------------------------------------

    @classmethod
    def load(cls, path):
        with open(path, "r") as f:
            data = yaml.safe_load(f) or {}
        return cls(data=data, path=path)

    def save(self, path=None):
        path = path or self.path
        dirname = os.path.dirname(path)
        if dirname:
            os.makedirs(dirname, exist_ok=True)
        # Strip _meta before validation, but keep it in the saved file
        data_to_save = self.data
        with open(path, "w") as f:
            yaml.dump(data_to_save, f, default_flow_style=False, sort_keys=False)
        self.path = path

    # -- Validation ---------------------------------------------------------

    def validate(self):
        schema = _load_schema()
        # Strip _meta for validation since it's not in the schema
        clean = {k: v for k, v in self.data.items() if k != "_meta"}
        try:
            jsonschema.validate(instance=clean, schema=schema)
        except jsonschema.ValidationError as e:
            print(f"Board YAML validation error: {e.message}")
            sys.exit(1)

    # -- Merge --------------------------------------------------------------

    def merge_from(self, other):
        """Section-level merge: keep existing sections, fill missing from *other*."""
        for key, val in other.data.items():
            if key == "_meta":
                continue
            if key not in self.data:
                self.data[key] = copy.deepcopy(val)


def _load_schema():
    schema_path = os.path.join(ove_dir(), "config", "schemas", "board_schema.json")
    with open(schema_path, "r") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# Devicetree to board.yaml mapping (Zephyr import)
# ---------------------------------------------------------------------------

# Map compatible prefixes to oveRTOS MCU families.
_MCU_FAMILY_MAP = {
    "arm,mps2": "arm",
    "st,stm32f7": "stm32f7",
    "st,stm32f4": "stm32f4",
    "st,stm32f3": "stm32f3",
    "st,stm32f1": "stm32f1",
    "st,stm32f0": "stm32f0",
    "st,stm32h7": "stm32h7",
    "st,stm32l4": "stm32l4",
    "st,stm32l0": "stm32l0",
    "st,stm32g0": "stm32g0",
    "st,stm32g4": "stm32g4",
    "st,stm32u5": "stm32u5",
    "st,stm32wb": "stm32wb",
    "st,stm32wl": "stm32wl",
    "nordic,nrf52": "nrf52",
    "nordic,nrf53": "nrf53",
    "nordic,nrf91": "nrf91",
    "espressif,esp32": "esp32",
    "nxp,imxrt": "imxrt",
    "nxp,lpc": "lpc",
    "atmel,sam": "sam",
    "raspberrypi,rp2040": "rp2040",
}


def _mcu_family_from_compatible(compat):
    for prefix, family in _MCU_FAMILY_MAP.items():
        if compat.startswith(prefix):
            return family
    # Fallback: strip vendor prefix
    if "," in compat:
        return compat.split(",", 1)[1].rstrip("0123456789").rstrip("-_")
    return compat


def _refine_nuttx_mcu_family(chip_dir, config):
    """Refine MCU family from NuttX defconfig beyond the chip directory name.

    The chip directory (e.g., boards/arm/stm32/<board>) is often too generic.
    This scans CONFIG_ARCH_CHIP_* boolean keys to extract the exact family.

    Examples:
        CONFIG_ARCH_CHIP_STM32F429Z=y → stm32f4
        CONFIG_ARCH_CHIP_NRF52840=y   → nrf52
    """
    # Collect candidates, prefer longer (more specific) suffixes
    candidates = []
    for key, val in config.items():
        if not key.startswith("CONFIG_ARCH_CHIP_") or val is not True:
            continue
        suffix = key[len("CONFIG_ARCH_CHIP_"):]
        if not suffix or len(suffix) <= 5:
            continue  # skip generic short names like "STM32"
        candidates.append(suffix)
    # Sort longest first so the most specific match wins
    candidates.sort(key=len, reverse=True)

    for suffix in candidates:
        # STM32 family: STM32F429Z → stm32f4, STM32H743XI → stm32h7
        m = re.match(r"STM32([A-Z])(\d)", suffix)
        if m:
            return f"stm32{m.group(1).lower()}{m.group(2)}"
        # Nordic: NRF52840 → nrf52, NRF5340 → nrf53
        m = re.match(r"NRF(\d{2})", suffix)
        if m:
            return f"nrf{m.group(1)}"
        # ESP: ESP32S3 → esp32
        if suffix.startswith("ESP32"):
            return "esp32"

    # Fall back to chip directory regex
    m = re.match(r"([a-zA-Z]+\d+[a-zA-Z]?\d*)", chip_dir)
    if m:
        return m.group(1).lower()
    return chip_dir


def _derive_nuttx_mcu(config, chip_dir):
    """Derive MCU identifier (e.g. STM32F429xx) from NuttX defconfig.

    Examples:
        CONFIG_ARCH_CHIP_STM32F429Z=y → STM32F429xx
        CONFIG_ARCH_CHIP_STM32F746ZG=y → STM32F746xx
        CONFIG_ARCH_CHIP_NRF52840=y   → NRF52840
    """
    # Collect candidates, prefer longest (most specific) suffix
    candidates = []
    for key, val in config.items():
        if not key.startswith("CONFIG_ARCH_CHIP_") or val is not True:
            continue
        suffix = key[len("CONFIG_ARCH_CHIP_"):]
        if not suffix or len(suffix) <= 5:
            continue  # skip generic short names like "STM32"
        candidates.append(suffix)
    candidates.sort(key=len, reverse=True)

    for suffix in candidates:
        # STM32: strip pin-count suffix letter(s), append xx
        m = re.match(r"(STM32[A-Z]\d{3,4})[A-Z]{1,2}$", suffix)
        if m:
            return m.group(1) + "xx"
        # Non-STM32 with enough specificity: return as-is
        return suffix

    # Fall back to CONFIG_ARCH_CHIP string value
    arch_chip = config.get("CONFIG_ARCH_CHIP", chip_dir)
    if isinstance(arch_chip, bool):
        return chip_dir
    return str(arch_chip)


def _edt_prop(node, name, default=None):
    prop = node.props.get(name) if node is not None else None
    return prop.val if prop is not None else default


def _edt_gpio_devices(edt, enabled, alias, compatible):
    nodes = [node for node in enabled if alias in node.aliases]
    for group in edt.compat2okay.get(compatible, []):
        nodes.extend(child for child in group.children.values()
                     if child.status == "okay" and child not in nodes)
    return nodes


def _extract_board_yaml_from_edt(edt, board_name):
    warnings = []
    soc = next((node for node in edt.nodes if node.path == "/soc"), None)
    compat = next((c for c in soc.compats if "," in c), "") if soc else ""
    root_compat = next(iter(edt.get_node("/").compats), "") if not compat else ""
    mcu = compat.split(",", 1)[-1] or "unknown"
    if re.fullmatch(r"stm32[a-z]\d{3,4}", mcu):
        mcu = mcu.upper() + "xx"
    if not compat:
        warnings.append("board.mcu needs manual entry (no SoC-specific /soc compatible)")
    data = {"board": {
        "name": re.sub(r"[^A-Za-z0-9-]", "-", board_name),
        "mcu_family": _mcu_family_from_compatible(compat or root_compat) or "unknown",
        "mcu": mcu,
    }}

    enabled = [node for node in edt.nodes if node.status == "okay"]
    chosen = edt.chosen_nodes
    sram = chosen.get("zephyr,sram")
    dtcm = chosen.get("zephyr,dtcm")
    sdram = [node for node in enabled if node.regs and (
        node.name.startswith("sdram@")
        or _edt_prop(node, "zephyr,memory-region", "").upper().startswith("SDRAM")
    )]
    for kind, nodes in (
        ("ram", [sram] if sram is dtcm else [sram, dtcm]),
        ("flash", [chosen.get("zephyr,flash")]),
        ("sdram", sdram[:1]),
    ):
        regs = [reg for node in nodes if node is not None and node.status == "okay"
                for reg in node.regs if reg.addr is not None and reg.size]
        if regs:
            data.setdefault("memory", {}).update({
                f"{kind}_kb": sum(reg.size for reg in regs) // 1024,
                f"{kind}_start": f"0x{min(reg.addr for reg in regs):08X}",
            })

    baud = _edt_prop(chosen.get("zephyr,console"), "current-speed")
    if baud:
        data["serial"] = {"console": {"baud": baud}}

    for key, labels in (("sysclk_mhz", ("sysclk", "rcc")), ("hse_freq_mhz", ("clk_hse",))):
        for label in labels:
            clock = edt.label2node.get(label)
            hz = _edt_prop(clock, "clock-frequency", 0)
            if hz >= 1000000 and clock.status == "okay":
                data.setdefault("clock", {})[key] = hz // 1000000
                break

    leds = _edt_gpio_devices(edt, enabled, "led0", "gpio-leds")
    if leds:
        data["leds"] = []
    for node in leds:
        for gpio in _edt_prop(node, "gpios", []):
            if gpio is None:
                continue
            port = next((label[4:] for label in gpio.controller.labels
                         if re.fullmatch(r"gpio(?:[a-z]|[0-9]+)", label)), "")
            if not port or gpio.controller.status != "okay" or "pin" not in gpio.data:
                warnings.append(f"leds: cannot map {node.path} to an oveRTOS GPIO")
                continue
            data["leds"].append({
                "port": int(port) if port.isdigit() else ord(port) - ord("a"),
                "pin": gpio.data["pin"],
                "active_low": bool(gpio.data.get("flags", 0) & 1),
            })
    buttons = _edt_gpio_devices(edt, enabled, "sw0", "gpio-keys")
    if buttons:
        warnings.append("GPIO buttons have no board.yaml field: "
                        + ", ".join(node.path for node in buttons))

    display = chosen.get("zephyr,display")
    if display is not None and display.status == "okay":
        width, height = (_edt_prop(display, key) for key in ("width", "height"))
        if width and height:
            data["display"] = {"width": width, "height": height}
    if any({"zephyr,sdmmc-disk", "st,stm32-sdmmc"}.intersection(node.compats)
           for node in enabled):
        data["sdcard"] = {}

    for node in enabled:
        if (node.compats and "i2c" in node.on_buses
                and "/bindings/audio/" in (node.binding_path or "")):
            codec = {"type": node.compats[0].split(",", 1)[-1].upper().replace("-", "")}
            if node.regs and node.regs[0].addr is not None:
                codec["i2c_addr"] = f"0x{node.regs[0].addr:02X}"
            data["audio"] = {"codec": codec}
            warnings.append("audio.i2s settings need manual entry")
            break

    return data, warnings


# ---------------------------------------------------------------------------
# NuttX board discovery & parsing
# ---------------------------------------------------------------------------

def _find_nuttx_board_dir(nuttx_base, board_name):
    """Locate a NuttX board directory: boards/<arch>/<chip>/<board>/"""
    boards_root = os.path.join(nuttx_base, "boards")
    if not os.path.isdir(boards_root):
        return None
    for arch in os.listdir(boards_root):
        arch_dir = os.path.join(boards_root, arch)
        if not os.path.isdir(arch_dir):
            continue
        for chip in os.listdir(arch_dir):
            candidate = os.path.join(arch_dir, chip, board_name)
            if os.path.isdir(candidate):
                return candidate
    return None


def _extract_board_yaml_from_nuttx(nuttx_board_dir, config_variant, board_name):
    """Build board.yaml dict from NuttX board directory.

    Returns (board_yaml_dict, warnings_list).
    """
    warnings = []
    data = {}

    # Derive mcu_family from path: boards/<arch>/<chip>/<board>
    parts = nuttx_board_dir.replace("\\", "/").split("/")
    chip = ""
    # Find the "boards" segment and extract chip
    for i, p in enumerate(parts):
        if p == "boards" and i + 2 < len(parts):
            chip = parts[i + 2]
            break

    # Parse defconfig
    defconfig_path = os.path.join(
        nuttx_board_dir, "configs", config_variant, "defconfig"
    )
    config = {}
    if os.path.isfile(defconfig_path):
        config = parse_dotconfig(defconfig_path)
    else:
        warnings.append(f"defconfig not found: {defconfig_path}")

    # Board identity — use refined helpers that scan CONFIG_ARCH_CHIP_* keys
    mcu_family = _refine_nuttx_mcu_family(chip, config)
    mcu = _derive_nuttx_mcu(config, chip)

    ove_name = board_name.replace("_", "-")
    data["board"] = {
        "name": ove_name,
        "mcu_family": mcu_family,
        "mcu": mcu,
    }

    # Memory
    memory = {}
    ram_size = config.get("CONFIG_RAM_SIZE")
    if isinstance(ram_size, int) and ram_size > 0:
        # Heuristic: values > 1024 are likely bytes, smaller values are likely KB
        memory["ram_kb"] = ram_size // 1024 if ram_size > 1024 else ram_size
        warnings.append(f"memory.ram_kb={memory['ram_kb']} — verify unit (CONFIG_RAM_SIZE={ram_size}, assumed {'bytes' if ram_size > 1024 else 'KB'})")
    flash_size = config.get("CONFIG_FLASH_SIZE")
    if isinstance(flash_size, int) and flash_size > 0:
        memory["flash_kb"] = flash_size // 1024 if flash_size > 1024 else flash_size
        warnings.append(f"memory.flash_kb={memory['flash_kb']} — verify unit (CONFIG_FLASH_SIZE={flash_size}, assumed {'bytes' if flash_size > 1024 else 'KB'})")
    if memory:
        data["memory"] = memory

    # Serial console
    for n in range(1, 9):
        key = f"CONFIG_USART{n}_SERIAL_CONSOLE"
        if config.get(key) is True:
            baud_key = f"CONFIG_USART{n}_BAUD"
            baud = config.get(baud_key)
            if isinstance(baud, int):
                data["serial"] = {"console": {"baud": baud}}
            break
    # Also check UART (not USART)
    if "serial" not in data:
        for n in range(0, 9):
            key = f"CONFIG_UART{n}_SERIAL_CONSOLE"
            if config.get(key) is True:
                baud_key = f"CONFIG_UART{n}_BAUD"
                baud = config.get(baud_key)
                if isinstance(baud, int):
                    data["serial"] = {"console": {"baud": baud}}
                break

    # Parse board.h for additional info
    board_h_path = os.path.join(nuttx_board_dir, "include", "board.h")
    if os.path.isfile(board_h_path):
        defines = _parse_board_h(board_h_path)

        # Clock
        clock = {}
        for key in ["BOARD_XTAL_FREQUENCY", "STM32_HSE_FREQUENCY"]:
            val = defines.get(key)
            if val and val > 100000:
                clock["hse_freq_mhz"] = val // 1000000
                break
        sysclk = defines.get("STM32_SYSCLK_FREQUENCY")
        if sysclk and sysclk > 100000:
            clock["sysclk_mhz"] = sysclk // 1000000
        if clock:
            data["clock"] = clock

        # Display
        w = defines.get("STM32_LTDC_WIDTH") or defines.get("BOARD_LCD_WIDTH")
        h = defines.get("STM32_LTDC_HEIGHT") or defines.get("BOARD_LCD_HEIGHT")
        if w and h:
            data["display"] = {"width": w, "height": h}

        # LED count
        nleds = defines.get("BOARD_NLEDS")
        if nleds and nleds > 0:
            data["leds"] = [{"port": 0, "pin": i, "active_low": False} for i in range(nleds)]
            warnings.append(f"leds: {nleds} LEDs detected from BOARD_NLEDS but port/pin need manual assignment")

    return data, warnings


def _parse_board_h(path):
    """Extract #define values from a board.h file. Returns dict of name→int."""
    defines = {}
    with open(path, "r") as f:
        for line in f:
            m = re.match(r"\s*#\s*define\s+(\w+)\s+(\S+)", line)
            if m:
                name = m.group(1)
                val_str = m.group(2)
                val = _try_parse_int(val_str)
                if val is not None:
                    defines[name] = val
    return defines


def _try_parse_int(s):
    """Try to parse a string as an integer (decimal or hex)."""
    s = s.strip().rstrip("UuLl")
    if not s:
        return None
    try:
        if s.startswith("0x") or s.startswith("0X"):
            return int(s, 16)
        return int(s)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Scaffold generation
# ---------------------------------------------------------------------------

def _render_scaffold(rtos, board_name, mcu_family, mcu, output_dir):
    """Render scaffold templates for a given RTOS into output_dir."""
    templates_dir = os.path.join(
        ove_dir(), "config", "templates", "board_scaffold", rtos
    )
    if not os.path.isdir(templates_dir):
        print(f"  Warning: no scaffold templates for {rtos}")
        return

    env = Environment(
        loader=FileSystemLoader(templates_dir),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )

    ctx = {
        "board_name": board_name,
        "mcu_family": mcu_family,
        "mcu": mcu,
    }

    os.makedirs(output_dir, exist_ok=True)
    for template_name in env.loader.list_templates():
        if not template_name.endswith(".j2"):
            continue
        output_name = template_name[:-3]  # strip .j2
        output_path = os.path.join(output_dir, output_name)
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        template = env.get_template(template_name)
        content = template.render(**ctx)
        with open(output_path, "w") as f:
            f.write(content)
        print(f"  Created: {os.path.relpath(output_path, ove_dir())}")


# ---------------------------------------------------------------------------
# Starter RTOS config for an imported board
# ---------------------------------------------------------------------------

STARTER_NOTE = "Starter written by `ove board import`; maintained by hand from here on."


def _write_starter(path, lines):
    """Create *path* from *lines* unless it already exists.

    A starter is a one-time bootstrap: once written, the file belongs to the
    board's maintainer, so an existing file is never touched.
    """
    rel = os.path.relpath(path, ove_dir())
    if os.path.exists(path):
        print(f"  Kept: {rel} (already exists)")
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  Created: {rel}")


def _zephyr_starter_conf(data):
    """Enable the Zephyr subsystems for the peripherals board.yaml lists."""
    lines = [f"# {STARTER_NOTE}"]
    audio = data.get("audio", {})
    if "i2s" in audio:
        lines += ["CONFIG_I2S=y", "CONFIG_I2C=y", "CONFIG_DMA=y", "CONFIG_AUDIO=y"]
    if "codec" in audio:
        lines.append("CONFIG_AUDIO_CODEC=y")
    if "sdcard" in data:
        lines += ["CONFIG_DISK_ACCESS=y", "CONFIG_DISK_DRIVERS=y",
                  "CONFIG_DISK_DRIVER_SDMMC=y"]
    if "display" in data:
        lines.append("CONFIG_DISPLAY=y")
    return lines


def _zephyr_starter_overlay(data):
    """Point at what the upstream devicetree may still lack.

    board.yaml was read from the upstream devicetree, so its LEDs, console
    and codec are already defined there; repeating them here would only
    duplicate nodes.
    """
    lines = [
        "/*",
        f" * {STARTER_NOTE}",
        " *",
        " * board.yaml was read from the upstream devicetree: its LEDs, console",
        " * and codec are already defined there. Add only what upstream lacks.",
        " */",
    ]
    display = data.get("display", {})
    if display.get("width") and display.get("height"):
        size = f"{display['width']}x{display['height']}"
        lines += ["", f"/* TODO: check that upstream describes the {size} display panel. */"]
    if data.get("audio", {}).get("i2s"):
        lines += ["", "/* TODO: check that the SAI/I2S controller and its DMA channels are on. */"]
    return lines


def _nuttx_starter_defconfig(data):
    """Explain the board overlay layer NuttX applies over oveRTOS's defconfig.

    board.yaml was read from the upstream NuttX config, which already sets
    the console, LEDs and clocks.
    """
    lines = [
        f"# {STARTER_NOTE}",
        "# Applied on top of oveRTOS's generated NuttX defconfig. A sibling",
        "# ove_board_defconfig.<feature> is applied only when",
        "# CONFIG_OVE_<FEATURE> is set (e.g. ove_board_defconfig.fs for OVE_FS).",
    ]
    if "sdcard" in data:
        lines += ["", "# TODO: enable the SD card controller and CONFIG_MMCSD in ove_board_defconfig.fs."]
    return lines


# ---------------------------------------------------------------------------
# Conflict resolution
# ---------------------------------------------------------------------------

def _resolve_conflict(existing_yaml_path, new_data, args):
    """Handle conflict when board.yaml already exists.

    Returns the BoardYaml to save (or None to skip).
    """
    existing = BoardYaml.load(existing_yaml_path)
    new_board = BoardYaml(data=new_data)
    # Overwriting replaces the hardware description; the engines imported
    # so far stay supported.
    if "rtos" in existing.data:
        new_board.data["rtos"] = copy.deepcopy(existing.data["rtos"])

    non_interactive = getattr(args, "non_interactive", False)
    overwrite = getattr(args, "overwrite", False)

    if non_interactive:
        if overwrite:
            print("  Overwriting existing board.yaml (--overwrite)")
            return new_board
        else:
            print("  Keeping existing board.yaml (non-interactive, use --overwrite to replace)")
            existing.merge_from(new_board)
            return existing

    # Interactive prompt
    print(f"\n  board.yaml already exists at: {existing_yaml_path}")
    print("  Options:")
    print("    [k] Keep existing (default)")
    print("    [o] Overwrite with new import")
    print("    [m] Merge (keep existing sections, fill missing)")
    print("    [d] Show diff")

    while True:
        try:
            choice = input("  Choice [k/o/m/d]: ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            choice = "k"

        if choice in ("", "k"):
            print("  Keeping existing board.yaml")
            return existing
        elif choice == "o":
            print("  Overwriting with new import")
            return new_board
        elif choice == "m":
            print("  Merging: keeping existing sections, filling missing")
            existing.merge_from(new_board)
            return existing
        elif choice == "d":
            _show_diff(existing.data, new_data)
        else:
            print("  Invalid choice. Use k, o, m, or d.")


def _show_diff(old_data, new_data):
    """Print a simple section-level diff."""
    all_keys = sorted(set(list(old_data.keys()) + list(new_data.keys())))
    for key in all_keys:
        if key == "_meta":
            continue
        if key in old_data and key not in new_data:
            print(f"    - {key}: (only in existing)")
        elif key not in old_data and key in new_data:
            print(f"    + {key}: (only in new)")
        elif old_data[key] != new_data[key]:
            print(f"    ~ {key}: differs")
        else:
            print(f"      {key}: same")


# ---------------------------------------------------------------------------
# Flash scripts
# ---------------------------------------------------------------------------

def _generate_flash_scripts(board_dir, rtos, ove_name, board):
    """Auto-generate flash.sh, debug.sh, and gdb.sh for the board."""
    rtos_dir = os.path.join(board_dir, rtos)
    if not os.path.isdir(rtos_dir):
        return

    # Discover OpenOCD board config
    openocd_scripts = "/usr/share/openocd/scripts/board"
    openocd_cfg = None

    # Derive chip prefix for precise matching: STM32F429xx → stm32f429
    chip_prefix = (board.get("mcu") or "").lower().rstrip("x")
    mcu_family = board.get("mcu_family", "unknown")

    if os.path.isdir(openocd_scripts):
        board_files = sorted(os.listdir(openocd_scripts))

        # Strategy 1: match by chip prefix (most specific)
        if chip_prefix:
            for f in board_files:
                if chip_prefix in f and f.endswith(".cfg"):
                    openocd_cfg = f"board/{f}"
                    break

        # Strategy 2: match by mcu_family + "discovery"
        if not openocd_cfg:
            candidate = f"{mcu_family}discovery.cfg"
            if os.path.isfile(os.path.join(openocd_scripts, candidate)):
                openocd_cfg = f"board/{candidate}"

    # Strategy 3: fallback
    if not openocd_cfg:
        openocd_cfg = f"board/{mcu_family}discovery.cfg"

    pretty_name = ove_name.upper()

    scripts = {
        "flash.sh": (
            f"#!/bin/bash\n"
            f"# Flash script for {pretty_name} ({rtos.capitalize()})\n"
            f"# Usage: flash.sh <firmware.elf>\n"
            f"\n"
            f"set -e\n"
            f"\n"
            f'FIRMWARE="${{1:?Usage: $0 <firmware.elf>}}"\n'
            f"\n"
            f'if [ ! -f "$FIRMWARE" ]; then\n'
            f'    echo "Error: firmware file not found: $FIRMWARE"\n'
            f"    exit 1\n"
            f"fi\n"
            f"\n"
            f"openocd \\\n"
            f"    -f {openocd_cfg} \\\n"
            f'    -c "program $FIRMWARE verify reset exit"\n'
        ),
        "debug.sh": (
            f"#!/bin/bash\n"
            f"# Start OpenOCD debug server for {pretty_name} ({rtos.capitalize()})\n"
            f"# Usage: debug.sh\n"
            f"\n"
            f"set -e\n"
            f"\n"
            f"openocd -f {openocd_cfg}\n"
        ),
        "gdb.sh": (
            f"#!/bin/bash\n"
            f"# Connect GDB to OpenOCD for {pretty_name} ({rtos.capitalize()})\n"
            f"# Usage: gdb.sh <firmware.elf>\n"
            f"\n"
            f"set -e\n"
            f"\n"
            f'FIRMWARE="${{1:?Usage: $0 <firmware.elf>}}"\n'
            f"\n"
            f'if [ ! -f "$FIRMWARE" ]; then\n'
            f'    echo "Error: firmware file not found: $FIRMWARE"\n'
            f"    exit 1\n"
            f"fi\n"
            f"\n"
            f'arm-none-eabi-gdb "$FIRMWARE" \\\n'
            f'    -ex "target extended-remote :3333" \\\n'
            f'    -ex "monitor reset halt" \\\n'
            f'    -ex "load"\n'
        ),
    }

    for name, content in scripts.items():
        script_path = os.path.join(rtos_dir, name)
        if os.path.isfile(script_path):
            print(f"  Scripts: {name} already exists, skipping")
            continue
        with open(script_path, "w") as f:
            f.write(content)
        os.chmod(script_path, 0o755)
        print(f"  Scripts: created {os.path.relpath(script_path, ove_dir())}")


# ---------------------------------------------------------------------------
# Subcommand: import
# ---------------------------------------------------------------------------

def _set_board_identity(board_yaml, ove_name):
    """Give an imported board its Kconfig symbol and menu title, unless set."""
    board = board_yaml.data["board"]
    board.setdefault("symbol", "OVE_BOARD_" + ove_name.upper().replace("-", "_"))
    board.setdefault("title", ove_name.upper())


def cmd_import(args):
    rtos = args.import_rtos

    if rtos == "zephyr":
        _import_zephyr(args)
    elif rtos == "nuttx":
        _import_nuttx(args)
    else:
        print(f"Error: unsupported RTOS '{rtos}' for import")
        sys.exit(1)


def _configure_zephyr(zephyr_base, board_name):
    base = Path(zephyr_base).resolve()
    sample = base / "samples/hello_world"
    edtlib_src = base / "scripts/dts/python-devicetree/src"
    if not (sample / "CMakeLists.txt").is_file() or not (edtlib_src / "devicetree/edtlib.py").is_file():
        sys.exit(f"Error: Zephyr workspace not found at {base}. "
                 "Run 'ove download' from a Zephyr configuration or pass --zephyr-base "
                 "when running board_tool.py directly.")

    output = Path(ove_dir()).resolve() / "output"
    sdk_dir = find_zephyr_sdk(str(output / "toolchains"), load_manifest(ove_dir()))
    if not sdk_dir:
        sys.exit("Error: Zephyr SDK not found. Run 'ove ensure-toolchain zephyr-sdk'.")

    env = zephyr_sdk_env(os.environ, sdk_dir)
    env["ZEPHYR_BASE"] = str(base)
    env["PATH"] = os.path.dirname(sys.executable) + os.pathsep + env["PATH"]
    slug = re.sub(r"[^A-Za-z0-9_-]", "-", board_name)
    build_dir = output / "board-import" / slug
    build_dir.mkdir(parents=True, exist_ok=True)
    log_path = build_dir / "configure.log"
    edt_path = build_dir / "zephyr/edt.pickle"
    cmd = ["cmake", "-S", str(sample), "-B", str(build_dir), "-G", "Ninja",
           f"-DBOARD={board_name}", f"-DPython3_EXECUTABLE={sys.executable}",
           f"-DUSER_CACHE_DIR={output / 'board-import/.cache'}"]

    print(f"  Configuring Zephyr devicetree (log: {log_path})", flush=True)
    try:
        # CMake otherwise keeps the first source path and caches Zephyr's board.
        (build_dir / "CMakeCache.txt").unlink(missing_ok=True)
        shutil.rmtree(build_dir / "CMakeFiles", ignore_errors=True)
        edt_path.unlink(missing_ok=True)
        with log_path.open("w") as log, subprocess.Popen(
            cmd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True
        ) as proc:
            try:
                status = proc.wait(timeout=120)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(proc.pid, signal.SIGKILL)
                raise
        if status:
            raise RuntimeError(f"CMake exited with status {status}")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        if log_path.is_file():
            with log_path.open(errors="replace") as log:
                sys.stderr.writelines(deque(log, maxlen=20))
        sys.exit(f"Error: Zephyr configure failed for {board_name}: {exc} (see {log_path})")

    sys.path.insert(0, str(edtlib_src))
    try:
        with edt_path.open("rb") as pickle_file:
            return pickle.load(pickle_file)
    except (OSError, ImportError, pickle.UnpicklingError, EOFError) as exc:
        sys.exit(f"Error: cannot load Zephyr devicetree at {edt_path}: {exc}")
    finally:
        sys.path.pop(0)


def _import_zephyr(args):
    zephyr_base = args.zephyr_base
    if not zephyr_base:
        version = get_component(load_manifest(ove_dir()), "rtos", "zephyr", "version")
        if not version:
            sys.exit("Error: Zephyr version missing from manifest.yaml")
        workspace, _, _ = hashed_dir(dl_dir(), "zephyr-workspace", version)
        zephyr_base = os.path.join(workspace, "zephyr")
    board_name = args.board

    print(f"Importing Zephyr board: {board_name}")
    edt = _configure_zephyr(zephyr_base, board_name)
    data, warnings = _extract_board_yaml_from_edt(edt, board_name)

    # oveRTOS board name
    ove_name = args.ove_name or data["board"]["name"]
    data["board"]["name"] = ove_name

    # Add provenance
    data["_meta"] = {
        "imported_from": "zephyr",
        "import_date": datetime.date.today().isoformat(),
        "zephyr_board": board_name,
    }

    # Target directory
    target_dir = os.path.join(boards_dir(), ove_name)

    # Check for existing board.yaml
    yaml_path = os.path.join(target_dir, "board.yaml")
    if os.path.isfile(yaml_path):
        board_yaml = _resolve_conflict(yaml_path, data, args)
        if board_yaml is None:
            return
    else:
        board_yaml = BoardYaml(data=data)
    _set_board_identity(board_yaml, ove_name)
    board_yaml.data.setdefault("rtos", {})["zephyr"] = {"board": board_name}

    # Validate (strip _meta)
    board_yaml.validate()

    # Save board.yaml
    os.makedirs(target_dir, exist_ok=True)
    board_yaml.save(yaml_path)
    print(f"  Saved: {os.path.relpath(yaml_path, ove_dir())}")

    # Create Zephyr scaffold
    zephyr_board_name = board_name  # keep original Zephyr name for .conf/.overlay
    zephyr_scaffold_dir = os.path.join(target_dir, "zephyr")
    _render_scaffold(
        "zephyr",
        ove_name,
        data["board"]["mcu_family"],
        data["board"]["mcu"],
        zephyr_scaffold_dir,
    )

    # Starter boards/<zephyr board>.conf and .overlay, which Zephyr picks up
    # by name. A qualified name (mps2/an521/cpu0) maps to underscores.
    zephyr_boards_dir = os.path.join(zephyr_scaffold_dir, "boards")
    starter = os.path.join(zephyr_boards_dir, zephyr_board_name.replace("/", "_"))
    _write_starter(starter + ".conf", _zephyr_starter_conf(board_yaml.data))
    _write_starter(starter + ".overlay", _zephyr_starter_overlay(board_yaml.data))

    _generate_flash_scripts(target_dir, "zephyr", ove_name, board_yaml.data["board"])

    # Print warnings
    if warnings:
        print("\n  Fields that need manual review:")
        for w in warnings:
            print(f"    - {w}")

    print(f"\nImport complete: boards/{ove_name}/")


def _import_nuttx(args):
    nuttx_base = args.nuttx_base or os.path.join(dl_dir(), "nuttx")
    board_name = args.board
    config_variant = args.config or "nsh"

    if not os.path.isdir(nuttx_base):
        print(f"Error: NuttX base not found: {nuttx_base}")
        print("  Download NuttX first or specify --nuttx-base")
        sys.exit(1)

    board_dir = _find_nuttx_board_dir(nuttx_base, board_name)
    if not board_dir:
        print(f"Error: NuttX board '{board_name}' not found in {nuttx_base}/boards/")
        sys.exit(1)

    print(f"Importing NuttX board: {board_name} (config: {config_variant})")
    print(f"  Source: {board_dir}")

    data, warnings = _extract_board_yaml_from_nuttx(board_dir, config_variant, board_name)

    # oveRTOS board name
    ove_name = args.ove_name or data["board"]["name"]
    data["board"]["name"] = ove_name

    # Add provenance
    data["_meta"] = {
        "imported_from": "nuttx",
        "import_date": datetime.date.today().isoformat(),
        "nuttx_board": board_name,
        "nuttx_config": config_variant,
    }

    # Target directory
    target_dir = os.path.join(boards_dir(), ove_name)

    # Check for existing board.yaml
    yaml_path = os.path.join(target_dir, "board.yaml")
    if os.path.isfile(yaml_path):
        board_yaml = _resolve_conflict(yaml_path, data, args)
        if board_yaml is None:
            return
    else:
        board_yaml = BoardYaml(data=data)
    _set_board_identity(board_yaml, ove_name)
    board_yaml.data.setdefault("rtos", {})["nuttx"] = {
        "board": board_name, "config": config_variant}

    # Validate
    board_yaml.validate()

    # Save board.yaml
    os.makedirs(target_dir, exist_ok=True)
    board_yaml.save(yaml_path)
    print(f"  Saved: {os.path.relpath(yaml_path, ove_dir())}")

    # Create NuttX scaffold
    nuttx_scaffold_dir = os.path.join(target_dir, "nuttx")
    _render_scaffold(
        "nuttx",
        ove_name,
        data["board"]["mcu_family"],
        data["board"]["mcu"],
        nuttx_scaffold_dir,
    )
    _write_starter(os.path.join(nuttx_scaffold_dir, "ove_board_defconfig"),
                   _nuttx_starter_defconfig(board_yaml.data))

    _generate_flash_scripts(target_dir, "nuttx", ove_name, board_yaml.data["board"])

    # Print warnings
    if warnings:
        print("\n  Fields that need manual review:")
        for w in warnings:
            print(f"    - {w}")

    print(f"\nImport complete: boards/{ove_name}/")


# ---------------------------------------------------------------------------
# Subcommand: list
# ---------------------------------------------------------------------------

def cmd_list(args):
    bdir = boards_dir()
    if not os.path.isdir(bdir):
        print("No boards directory found.")
        return

    rows = []
    for entry in sorted(os.listdir(bdir)):
        board_path = os.path.join(bdir, entry)
        if not os.path.isdir(board_path):
            continue
        yaml_path = os.path.join(board_path, "board.yaml")
        if not os.path.isfile(yaml_path):
            continue
        board_yaml = BoardYaml.load(yaml_path)
        bd = board_yaml.data.get("board", {})
        name = bd.get("name", entry)
        mcu = bd.get("mcu", "")
        family = bd.get("mcu_family", "")

        # Detect RTOS subdirectories
        rtos_list = []
        for rtos in ["freertos", "zephyr", "nuttx"]:
            if os.path.isdir(os.path.join(board_path, rtos)):
                rtos_list.append(rtos)

        rows.append((name, family, mcu, ", ".join(rtos_list) or "-"))

    if not rows:
        print("No boards found.")
        return

    # Print table
    headers = ("Board", "MCU Family", "MCU", "RTOS Support")
    widths = [max(len(h), max(len(r[i]) for r in rows)) for i, h in enumerate(headers)]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)

    print(fmt.format(*headers))
    print(fmt.format(*("-" * w for w in widths)))
    for row in rows:
        print(fmt.format(*row))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="oveRTOS Board Import Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            examples:
              %(prog)s import zephyr --board stm32f746g_disco
              %(prog)s import nuttx  --board stm32f746g-disco --config nsh
              %(prog)s list
        """),
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # --- import ---
    import_parser = subparsers.add_parser("import", help="Import board from RTOS source tree")
    import_sub = import_parser.add_subparsers(dest="import_rtos", required=True)

    # import zephyr
    zephyr_parser = import_sub.add_parser("zephyr", help="Import from Zephyr board definition")
    zephyr_parser.add_argument("--board", required=True, help="Zephyr board name (e.g. stm32f746g_disco)")
    zephyr_parser.add_argument("--zephyr-base", help="Path to Zephyr source tree")
    zephyr_parser.add_argument("--ove-name", help="Override oveRTOS board name")
    zephyr_parser.add_argument("--non-interactive", action="store_true", help="Non-interactive mode")
    zephyr_parser.add_argument("--overwrite", action="store_true", help="Overwrite existing board.yaml")
    zephyr_parser.set_defaults(func=cmd_import)

    # import nuttx
    nuttx_parser = import_sub.add_parser("nuttx", help="Import from NuttX board definition")
    nuttx_parser.add_argument("--board", required=True, help="NuttX board name (e.g. stm32f746g-disco)")
    nuttx_parser.add_argument("--nuttx-base", help="Path to NuttX source tree")
    nuttx_parser.add_argument("--config", help="NuttX config variant (default: nsh)")
    nuttx_parser.add_argument("--ove-name", help="Override oveRTOS board name")
    nuttx_parser.add_argument("--non-interactive", action="store_true", help="Non-interactive mode")
    nuttx_parser.add_argument("--overwrite", action="store_true", help="Overwrite existing board.yaml")
    nuttx_parser.set_defaults(func=cmd_import)

    # --- list ---
    list_parser = subparsers.add_parser("list", help="List all oveRTOS boards")
    list_parser.set_defaults(func=cmd_list)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
