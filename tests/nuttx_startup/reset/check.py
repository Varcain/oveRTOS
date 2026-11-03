# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Cross-compile actual native reset source; inspect linked images only."""

import argparse
import hashlib
import json
import os
import re
import shlex
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.nuttx_startup import require_arm_startup


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    # The CLI/proof caller owns this group, including every compiler child.
    # A second detached tool group could outlive cancellation of this script.
    owner_group = os.getpgrp()
    if owner_group != os.getpid():
        raise RuntimeError("Run native reset checks in a dedicated caller-owned process group")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    build = args.native_build.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    database = build / "compile_commands.json"
    entries = [row for row in json.loads(database.read_text()) if
               row["file"].endswith("/arch/arm/src/armv7-m/arm_vectors.c")]
    if len(entries) != 1:
        raise RuntimeError("Expected one actual native vector compile command")
    entry = entries[0]
    source = args.source.resolve() if args.source else Path(entry["file"])
    if not source.is_file():
        raise RuntimeError("Missing actual native vector source")
    command = entry.get("arguments") or shlex.split(entry["command"])
    index = next(i for i, word in enumerate(command) if word.endswith("arm-none-eabi-gcc"))
    compiler = command[index]
    flags = []
    cursor = index + 1
    while cursor < len(command):
        word = command[cursor]
        if word in ("-o", "-MF", "-MT", "-MQ"):
            cursor += 2
            continue
        if word not in ("-c", "-MD", "-MMD", "-MP", entry["file"]):
            flags.append(word)
        cursor += 1
    target = [word for word in flags if word == "-mthumb" or
              word.startswith(("-mcpu=", "-march=", "-mfpu=", "-mfloat-abi="))]
    config = build / ".config"
    script = build / "flash.ld.tmp"
    configuration = config.read_text()
    if 'CONFIG_ARCH_BOARD="mps2-an500"' in configuration:
        board = "mps2-an500"
    elif 'CONFIG_ARCH_BOARD="stm32f746g-disco"' in configuration:
        board = "stm32f746g-disco"
    else:
        raise RuntimeError("Unsupported native reset fixture board")
    identities = {"source": digest(source), "config": digest(config),
                  "script": digest(script), "database": digest(database)}
    wrapper = output / "entry.o"
    commands = []

    def run(label, command):
        with (output / (label + ".log")).open("w") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
                                       cwd=entry["directory"])
            try:
                group = os.getpgid(process.pid)
                if group != owner_group:
                    raise RuntimeError("Compiler escaped the native reset owner group")
                code = process.wait(timeout=30)
            except BaseException:
                process.kill()
                process.wait(timeout=5)
                raise
        if code != 0:
            raise RuntimeError("Native reset fixture failed: " + label)
        commands.append({"label": label, "command": command, "rc": code,
                         "tool_pid": process.pid, "tool_pgid": group, "owner_pgid": owner_group})

    run("entry", [compiler, *target, "-I" + str(build / "include"), "-c",
                  str(Path(__file__).with_name("entry.S")), "-o", str(wrapper)])
    facts = output / "branch-facts.o"
    run("branch-facts", [compiler, *target, "-c",
                        str(Path(__file__).with_name("branch-facts.S")), "-o", str(facts)])
    run("branch-facts-disassembly", [compiler[:-3] + "objdump", "-d", str(facts)])
    expected_encodings = {
        "narrow_forward": ["e000"], "narrow_backward": ["e7f8"],
        "wide_forward": ["f000", "b800"], "wide_backward": ["f7ff", "bff8"],
        "wide_plus4m": ["f000", "b000"], "wide_plus8m": ["f000", "9800"],
        "wide_minus4m": ["f400", "b800"], "wide_minus8m": ["f400", "b000"],
    }
    observed_encodings = {}
    current = None
    for line in (output / "branch-facts-disassembly.log").read_text().splitlines():
        symbol = re.fullmatch(r"[0-9a-f]+ <([a-z0-9_]+)>:", line)
        if symbol:
            current = symbol[1]
            continue
        instruction = re.fullmatch(r"\s*[0-9a-f]+:\s+((?:[0-9a-f]{4}\s+){1,2})b(?:\.[nw])?\s+.*", line)
        if current and instruction:
            if current in observed_encodings:
                raise RuntimeError("Duplicate assembler branch control")
            observed_encodings[current] = instruction[1].split()
    if observed_encodings != expected_encodings:
        raise RuntimeError("Assembler branch encodings disagree with the literal controls")
    results = []
    cases = [(optimization, frame, reverse, False)
             for optimization in ("-O0", "-Os")
             for frame in ("-fno-omit-frame-pointer", "-fomit-frame-pointer")
             for reverse in (False, True)]
    cases.append(("-O0", "-fno-omit-frame-pointer", True, True))
    for optimization, frame, reverse, instrumented in cases:
        label = optimization[1:] + ("-keep" if frame.startswith("-fno") else "-omit")
        label += "-backward" if reverse else "-forward"
        label += "-instrumented" if instrumented else ""
        obj = output / (label + ".o")
        elf = output / (label + ".elf")
        run(label + "-compile", [compiler, *flags, optimization, frame,
            *(["-finstrument-functions"] if instrumented else []),
            "-MD", "-MF", str(output / (label + ".d")), "-c", str(source), "-o", str(obj)])
        objects = [str(wrapper), str(obj)] if reverse else [str(obj), str(wrapper)]
        run(label + "-link", [compiler, *target, "-nostdlib", *objects,
            "-Wl,-T," + str(script), "-Wl,--gc-sections,--build-id=none",
            "-Wl,-u,g_idle_topstack,-u,fixture_live,-Map," + str(output / (label + ".map")),
            "-o", str(elf)])
        evidence = require_arm_startup(elf, config, script, board)
        transfer = evidence["reset_transfer"]
        if (transfer["branch_target"] < transfer["start"]) != reverse:
            raise RuntimeError("Reset fixture did not exercise the intended branch direction")
        results.append({"label": label, "object_sha256": digest(obj),
                        "elf_sha256": digest(elf), "admission": evidence})
    if identities != {"source": digest(source), "config": digest(config),
                       "script": digest(script), "database": digest(database)}:
        raise RuntimeError("Native reset fixture inputs changed during compilation")
    result = {"native_build": str(build), "source": str(source), "identities": identities,
              "branch_facts_sha256": digest(facts),
              "branch_encodings": observed_encodings,
              "commands": commands, "cases": results,
              "scope": "Actual native vector TU and real config/flags; link-only C-start/handler definitions. No generated ARM image is executed."}
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print("NATIVE_RESET_LINK_ONLY_PASS", board, len(results))


if __name__ == "__main__":
    def stop(signum, frame):
        raise KeyboardInterrupt("Native reset fixture interrupted")

    signal.signal(signal.SIGTERM, stop)
    main()
