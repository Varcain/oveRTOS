# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Dump STM32F746 FreeRTOS benchmark wrappers and check their direct callees."""

import argparse
import re
import subprocess
import sys
from fnmatch import fnmatchcase
from pathlib import Path

import yaml

TEXT_SYMBOL = re.compile(
    r"^([0-9a-f]+)[ \t]+(?:([0-9a-f]+)[ \t]+)?[Tt][ \t]+(.+)$", re.MULTILINE
)

# Thumb calls and branches, including conditional tails and compare-and-branch.
DIRECT_BRANCH = re.compile(
    r"\s*[0-9a-f]+:\s+"
    r"(?:b(?:lx?)?(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?"
    r"(?:\.[nw])?|cbn?z)\s+"
    r"(?:\w+,\s*)?([0-9a-f]+)(?:\s+<([^>]+)>)?\s*(?:@.*)?$"
)


def disassemble(objdump, elf, symbol):
    output = subprocess.check_output(
        [objdump, f"--disassemble={symbol}", "--no-show-raw-insn", elf],
        text=True,
    )
    for block in output.strip().split("\n\n"):
        header, _, body = block.partition("\n")
        if header.endswith(f" <{symbol}>:") and body:
            return block
    return ""


def callees(disasm, start, end):
    calls = []
    for line in disasm.splitlines():
        match = DIRECT_BRANCH.match(line)
        if match:
            address, name = match.groups()
            if not start <= int(address, 16) < end:
                calls.append(name or f"0x{address}")
    return calls


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--target", required=True, help="target key in the config")
    parser.add_argument("--config", default="tests/audit/hotpath_expected.yaml")
    parser.add_argument("--output-dir", default="output/audit/disasm")
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--objdump", default="arm-none-eabi-objdump")
    args = parser.parse_args()

    config = yaml.safe_load(Path(args.config).read_text()) or {}
    entries = config.get(args.target, {}).get("symbols")
    if not entries:
        print(f"[hotpath dump] FAIL: target {args.target} has no symbols in {args.config}",
              file=sys.stderr)
        return 2

    nm_output = subprocess.check_output(
        [args.nm, "--print-size", "--defined-only", args.elf], text=True,
    )
    symbols = TEXT_SYMBOL.findall(nm_output)
    output_dir = Path(args.output_dir) / args.target
    output_dir.mkdir(parents=True, exist_ok=True)

    failed = False
    for entry in entries:
        label = entry["name"]
        pattern = entry["pattern"]
        allowed = entry["allowed_calls"]
        matches = [
            (address, size, symbol) for address, size, symbol in symbols
            if fnmatchcase(symbol, pattern)
        ]
        if not matches:
            print(f"{label}: FAIL: NOT FOUND (pattern={pattern})")
            failed = True
            continue

        for address, size, symbol in matches:
            disasm = disassemble(args.objdump, args.elf, symbol)
            if not disasm or not size:
                print(f"{label}: FAIL: no sized disassembly for {symbol}")
                failed = True
                continue

            safe_name = re.sub(r"[^A-Za-z0-9._-]+", "_", symbol)[:120]
            (output_dir / f"{safe_name}.S").write_text(disasm + "\n")
            start = int(address, 16)
            calls = callees(disasm, start, start + int(size, 16))
            print(f"{label}: FOUND {symbol} -> {calls}")
            unexpected = [call for call in calls
                          if not any(fnmatchcase(call, glob) for glob in allowed)]
            if unexpected:
                print(f"  FAIL: unexpected callees: {unexpected} (allowed: {allowed})")
                failed = True

    status = "FAIL" if failed else "OK"
    print(f"[hotpath dump] target={args.target} status={status}")
    return int(failed)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"[hotpath dump] FAIL: {exc}", file=sys.stderr)
        sys.exit(2)
