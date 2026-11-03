# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Check startup geometry and the known reset transfer of flat ARM images.

The selected native reset, idle-task and heap providers establish the meaning
of the symbols. Admission also recognizes their known stack-preserving reset
transfer; this does not decode arbitrary startup programs or qualify firmware.
"""

import hashlib
import re
import struct
from pathlib import Path


class _Image:
    def __init__(self, path):
        self.path = Path(path)
        self.data = Path(path).read_bytes()
        if self.data[:7] != b"\x7fELF\x01\x01\x01":
            raise ValueError("expected little-endian ELF32")
        header = self.unpack("<HHIIIIIHHHHHH", 16)
        if header[0:3] != (2, 40, 1) or header[7] != 52:
            raise ValueError("expected ARM executable")
        if header[8] != 32 or header[10] != 40 or not header[11]:
            raise ValueError("unsupported ELF table layout")
        self.programs = [self.unpack("<IIIIIIII", header[4] + i * 32)
                         for i in range(header[9])]
        self.sections = [self.unpack("<IIIIIIIIII", header[5] + i * 40)
                         for i in range(header[11])]
        if header[6] & 0xff000000 != 0x05000000:
            raise ValueError("expected ARM EABI5")
        for kind, offset, virtual, physical, filesz, memsz, flags, align in self.programs:
            if kind == 1 and (filesz > memsz or offset > len(self.data) or
                              filesz > len(self.data) - offset or virtual + memsz > 2**32):
                raise ValueError("invalid load span")
        for section in self.sections:
            if section[2] & 2 and section[3] + section[5] > 2**32:
                raise ValueError("unrepresentable allocated section")
            if section[1] != 8:
                self.section_bytes(section)
        if header[12] >= len(self.sections):
            raise ValueError("invalid section string table")
        if self.sections[header[12]][1] != 3:
            raise ValueError("invalid section-name table type")
        names = self.section_bytes(self.sections[header[12]])
        self.section_names = [self.string(names, row[0]) for row in self.sections]
        self.symbols = {}
        self.functions = {}
        for section in self.sections:
            if section[1] != 2:
                continue
            if section[9] != 16 or section[5] % 16 or section[6] >= len(self.sections):
                raise ValueError("invalid symbol table")
            if self.sections[section[6]][1] != 3:
                raise ValueError("invalid symbol string table type")
            strings = self.section_bytes(self.sections[section[6]])
            table = self.section_bytes(section)
            for offset in range(0, len(table), 16):
                name, value, size, info, other, index = struct.unpack_from("<IIIBBH", table, offset)
                name = self.string(strings, name)
                if name in ("start", "__start"):
                    self.functions.setdefault(name, []).append((value, size, info, index))
                if name in ("_vectors", "g_idle_topstack", "_ebss", "_sbss"):
                    if name in self.symbols:
                        raise ValueError("ambiguous boundary symbol " + name)
                    self.symbols[name] = (value, size, info, index)
        if set(self.symbols) != {"_vectors", "g_idle_topstack", "_ebss", "_sbss"}:
            raise ValueError("missing native boundary symbols")

    def unpack(self, pattern, offset):
        size = struct.calcsize(pattern)
        if offset < 0 or offset > len(self.data) - size:
            raise ValueError("ELF table exceeds file")
        return struct.unpack_from(pattern, self.data, offset)

    @staticmethod
    def string(data, offset):
        if offset >= len(data):
            raise ValueError("ELF string offset exceeds table")
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError("unterminated ELF string")
        return data[offset:end].decode("utf-8")

    def section_bytes(self, section):
        if section[1] == 8:
            raise ValueError("expected file-backed section")
        offset, size = section[4:6]
        if offset > len(self.data) or size > len(self.data) - offset:
            raise ValueError("section exceeds file")
        return self.data[offset:offset + size]

    def word(self, address):
        matches = []
        for kind, offset, virtual, physical, filesz, memsz, flags, align in self.programs:
            if kind == 1 and virtual <= address and address - virtual <= filesz - 4:
                matches.append(self.unpack("<I", offset + address - virtual)[0])
        if len(matches) != 1:
            raise ValueError("word is not uniquely file-backed")
        return matches[0]

    def object_span(self, name, minimum_size):
        address, size, info, index = self.symbols[name]
        if not 0 < index < len(self.sections) or info != 17 or size < minimum_size:
            raise ValueError("invalid native object " + name)
        section = self.sections[index]
        if (section[1] == 8 or not section[2] & 2 or
                not section[3] <= address <= section[3] + section[5] or
                size > section[3] + section[5] - address):
            raise ValueError("native object exceeds allocated section " + name)
        loads = [row for row in self.programs if row[0] == 1 and
                 row[2] <= address and address - row[2] + size <= row[4]]
        if len(loads) != 1:
            raise ValueError("native object is not uniquely file-backed " + name)
        return address

    def function_bytes(self, name, expected_info):
        symbols = self.functions.get(name, [])
        if len(symbols) != 1:
            raise ValueError("missing or ambiguous native function " + name)
        value, size, info, index = symbols[0]
        if (not value & 1 or info != expected_info or size < 2 or
                not 0 < index < len(self.sections)):
            raise ValueError("invalid native Thumb function " + name)
        address = value & ~1
        section = self.sections[index]
        if (section[1] != 1 or section[2] & 6 != 6 or
                not section[3] <= address <= section[3] + section[5] or
                size > section[3] + section[5] - address):
            raise ValueError("native function exceeds executable section " + name)
        loads = [row for row in self.programs if row[0] == 1 and row[6] & 1 and
                 row[2] <= address and address - row[2] + size <= row[4]]
        if len(loads) != 1:
            raise ValueError("native function is not uniquely executable " + name)
        load = loads[0]
        offset = load[1] + address - load[2]
        if offset != section[4] + address - section[3]:
            raise ValueError("inconsistent native function file backing " + name)
        return address, self.data[offset:offset + size]


def _config_value(config, name):
    matches = re.findall(r"^" + re.escape(name) + r"=(\S+)$", config, re.M)
    if len(matches) != 1:
        raise ValueError("missing configuration " + name)
    return int(matches[0], 0)


def _primary_ram(script):
    matches = re.findall(r"\bsram1\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*"
                      r"(0x[0-9a-fA-F]+|[0-9]+)\s*,\s*LENGTH\s*=\s*"
                      r"(0x[0-9a-fA-F]+|[0-9]+)([KkMm]?)\b", script)
    if len(matches) != 1:
        raise ValueError("missing primary SRAM definition")
    origin, amount, unit = matches[0]
    base = int(origin, 0)
    size = int(amount, 0) * {"": 1, "k": 1024, "m": 1048576}[unit.lower()]
    if not 0 <= base < 2**32 or not 0 < size <= 2**32 - base:
        raise ValueError("unrepresentable primary SRAM span")
    return base, size


def _inspect_geometry(image, config, script, board):
    configuration = Path(config).read_text()
    enabled = set(re.findall(r"^(CONFIG_[A-Z0-9_]+)=y$", configuration, re.M))
    if ("CONFIG_BUILD_FLAT" not in enabled or
            enabled & {"CONFIG_ARCH_SIM", "CONFIG_BUILD_PROTECTED",
                       "CONFIG_BUILD_KERNEL", "CONFIG_BUILD_PIC", "CONFIG_SMP"}):
        raise ValueError("only selected flat native images are covered")
    boards = {
        "mps2-an500": ("CONFIG_ARCH_BOARD_MPS2_AN500",
                       {(0x20000000, 4 * 1024**2), (0x60000000, 16 * 1024**2)}),
        "stm32f746g-disco": ("CONFIG_ARCH_BOARD_STM32F746G_DISCO",
                             {(0x20010000, 240 * 1024)}),
    }
    if board not in boards:
        raise ValueError("unsupported native startup board " + board)
    board_option, regions = boards[board]
    identities = re.findall(r'^CONFIG_ARCH_BOARD="([^"]+)"$', configuration, re.M)
    board_options = {entry[0] for entry in boards.values()}
    if identities != [board] or enabled & board_options != {board_option}:
        raise ValueError("native startup board configuration does not match")
    idle = _config_value(configuration, "CONFIG_IDLETHREAD_STACKSIZE")
    region, length = _primary_ram(Path(script).read_text())
    if (region, length) not in regions:
        raise ValueError("unsupported native primary SRAM geometry")
    config_region = _config_value(configuration, "CONFIG_RAM_START")
    config_length = _config_value(configuration, "CONFIG_RAM_SIZE")
    if config_region != region or not 0 < config_length <= length:
        raise ValueError("configured RAM aperture is outside native primary SRAM")
    boundary = image.symbols["_ebss"][0]
    bss_start = image.symbols["_sbss"][0]
    for name in ("_sbss", "_ebss"):
        value, size, info, index = image.symbols[name]
        if info != 16 or size or index != 0xfff1:
            raise ValueError("invalid absolute native BSS symbol " + name)
    bss = [row for name, row in zip(image.section_names, image.sections) if name == ".bss"]
    if (len(bss) != 1 or bss[0][1:3] != (8, 3) or
            bss[0][3] != bss_start or bss[0][3] + bss[0][5] != boundary):
        raise ValueError("native BSS symbols do not delimit the allocated BSS")
    vector = image.object_span("_vectors", 8)
    if vector % 4:
        raise ValueError("unaligned native vector object")
    idle_object, object_size, _, _ = image.symbols["g_idle_topstack"]
    if object_size != 4:
        raise ValueError("unexpected native stack constant size")
    image.object_span("g_idle_topstack", 4)
    if idle_object % 4:
        raise ValueError("unaligned native stack constant object")
    sp, reset = image.word(vector), image.word(vector + 4)
    top = image.word(idle_object)
    live = []
    for name, section in zip(image.section_names, image.sections):
        address, size = section[3], section[5]
        if section[2] & 3 != 3 or not size:
            continue
        if address + size > 2**32:
            raise ValueError("unrepresentable writable section")
        if address < region + length and region < address + size:
            live.append({"name": name, "address": address, "size": size})
    writable_loads = [row for row in image.programs if row[0] == 1 and row[6] & 2 and row[5]
                      and row[2] < region + length and region < row[2] + row[5]]
    checks = {
        "positive_idle_size": idle > 0,
        "idle_size_aligned8": idle % 8 == 0,
        "bss_boundary_in_ram": region <= bss_start <= boundary <= region + length,
        "bss_end_aligned8": boundary % 8 == 0,
        "idle_interval_fits_ram": idle > 0 and region <= boundary <= region + length
                                 and idle <= region + length - boundary,
        "vector_payload_agree": sp == top == boundary + idle,
        "entry_sp_aligned8": sp % 8 == 0,
        "all_primary_writable_before_idle": all(region <= row["address"] and
                                                row["address"] + row["size"] <= boundary
                                                for row in live),
        "all_primary_writable_loads_before_idle": all(
            region <= row[2] and row[2] + row[5] <= boundary for row in writable_loads),
        "thumb_reset_in_executable_load": bool(reset & 1) and any(
            row[0] == 1 and row[6] & 1 and row[2] <= reset - 1 < row[2] + row[4]
            for row in image.programs),
        "stackcheck_reserve": board != "stm32f746g-disco" or
                              "CONFIG_ARMV7M_STACKCHECK" not in enabled or idle > 64,
    }
    result = {"elf": str(image.path), "sha256": hashlib.sha256(image.data).hexdigest(),
              "idle_size": idle, "primary_ram_base": region, "primary_ram_size": length,
              "configured_ram_base": config_region, "configured_ram_size": config_length,
              "bss_start": bss_start, "bss_end": boundary, "vector_sp": sp,
              "idle_constant_payload": top, "reset_vector": reset,
              "raw_heap_start": top, "raw_heap_size": region + length - top,
              "primary_writable_sections": live, "checks": checks,
              "failed_checks": [name for name, passed in checks.items() if not passed]}
    return result


def inspect_arm_startup(elf, config, script, board):
    """Inspect geometry only, including nonexecuted assembly linker fixtures."""
    return _inspect_geometry(_Image(elf), config, script, board)


def _reset_transfer(image):
    """Recognize only the native clear-LR and unconditional Thumb branch stub."""
    start, code = image.function_bytes("start", 2)
    c_start, _ = image.function_bytes("__start", 18)
    vector = image.symbols["_vectors"][0]
    result = {"start": start, "c_start": c_start, "branch_target": None,
              "branch_bytes": None, "known_transfer": False}
    if image.word(vector + 4) != start + 1 or code[:4] != b"\x4f\xf0\x00\x0e":
        return result
    if len(code) < 6:
        raise ValueError("truncated native reset transfer")
    first = struct.unpack_from("<H", code, 4)[0]
    if first & 0xf800 == 0xe000:
        displacement = (first & 0x7ff) << 1
        if displacement & (1 << 11):
            displacement -= 1 << 12
        width = 2
    elif first & 0xf800 == 0xf000:
        if len(code) < 8:
            raise ValueError("truncated native wide reset branch")
        second = struct.unpack_from("<H", code, 6)[0]
        if second & 0xd000 != 0x9000:
            return result
        sign = (first >> 10) & 1
        i1 = 1 ^ (((second >> 13) & 1) ^ sign)
        i2 = 1 ^ (((second >> 11) & 1) ^ sign)
        displacement = ((sign << 24) | (i1 << 23) | (i2 << 22) |
                        ((first & 0x3ff) << 12) | ((second & 0x7ff) << 1))
        if sign:
            displacement -= 1 << 25
        width = 4
    else:
        return result
    destination = start + 8 + displacement
    result.update(branch_target=destination, branch_bytes=width,
                  known_transfer=destination == c_start)
    return result


def require_arm_startup(elf, config, script, board):
    """Require geometry and the native stack-preserving reset transfer."""
    try:
        image = _Image(elf)
        result = _inspect_geometry(image, config, script, board)
        result["reset_transfer"] = _reset_transfer(image)
        known = result["reset_transfer"]["known_transfer"]
        result["checks"]["known_reset_transfer"] = known
        if not known:
            result["failed_checks"].append("known_reset_transfer")
    except (OSError, ValueError, struct.error, UnicodeError) as error:
        raise RuntimeError("Cannot admit native ARM startup: " + str(error)) from error
    if result["failed_checks"]:
        raise RuntimeError("Native ARM startup boundary failed: " +
                           ", ".join(result["failed_checks"]))
    return result
