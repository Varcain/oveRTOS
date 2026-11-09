# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and qualify native IRQ callers before bounded positive execution."""
import argparse
import hashlib
import itertools
import json
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.test import _run_owned_test_process


def profiles():
    for zero in (0, 1):
        yield 0, 0, 0, zero
    for enforce, allow, zero in itertools.product((0, 1), repeat=3):
        yield 1, enforce, allow, zero


def elf_bytes(data, address, size):
    if data[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", data, 18)[0] != 40:
        raise ValueError("expected a little-endian ARM ELF32 image")
    offset = struct.unpack_from("<I", data, 28)[0]
    entry_size, count = struct.unpack_from("<HH", data, 42)
    for index in range(count):
        kind, start, virtual, _, length = struct.unpack_from("<5I", data, offset + index * entry_size)
        if kind == 1 and virtual <= address and address + size <= virtual + length:
            begin = start + address - virtual
            result = data[begin:begin + size]
            if len(result) == size:
                return result
    raise ValueError("requested artifact bytes are not file-backed")


def qualify(binary, folder, toolchain, profile, canary, alternate, all_privileged):
    mpu, enforce, allow, zero = profile
    nm = toolchain / "bin/arm-none-eabi-nm"
    objdump = toolchain / "bin/arm-none-eabi-objdump"
    symbols = subprocess.check_output([str(nm), "-S", "--defined-only", str(binary)], timeout=20).decode()
    assembly = subprocess.check_output([str(objdump), "-d", str(binary)], timeout=20).decode()
    (folder / "symbols.txt").write_text(symbols)
    (folder / "assembly.txt").write_text(assembly)
    table = {}
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) == 4:
            table[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
        elif len(parts) == 3:
            table[parts[2]] = (int(parts[0], 16), 0)
    wanted = ("caller_state", "pend_kernel") + (() if alternate else ("irq_task_enter", "irq_task_leave"))
    sites = []
    if mpu:
        low = table["__syscalls_flash_start__"][0]
        high = table["__syscalls_flash_end__"][0]
        for stem in wanted:
            found = [(name, value) for name, value in table.items() if name == stem or name.startswith(stem + ".")]
            if len(found) != 1:
                raise ValueError("expected one native syscall helper: " + stem)
            name, (address, size) = found[0]
            pcs = [int(match.group(1), 16) for match in re.finditer(r"^\s*([0-9a-f]+):.*\bsvc\s+102\b", assembly, re.MULTILINE)
                   if address <= int(match.group(1), 16) < address + size]
            if len(pcs) != 1 or not low <= pcs[0] + 2 <= high:
                raise ValueError("native SVC origin is not eligible: " + name)
            sites.append({"function": name, "instruction_pc": pcs[0], "return_pc": pcs[0] + 2,
                          "function_extent": [address, address + size], "range": [low, high]})
    data = binary.read_bytes()
    metadata_address, metadata_size = table["irq_profile"]
    metadata = struct.unpack("<17I", elf_bytes(data, metadata_address, metadata_size))
    expected = (0x49525131, mpu, enforce, allow, int(all_privileged), 1-zero, int(canary),
                int(not mpu), 16 if mpu else 0, 512, 2048, table["worker_tcb"][1],
                0x13070000 if mpu else 0, 2, 1, 128, 4)
    if metadata != expected:
        raise ValueError("compiled role, grant, atomic or configuration metadata differs")
    extents = []
    for name in ("shared", "worker_stack", "supervisor_stack", "worker_tcb", "supervisor_tcb"):
        address, size = table[name]
        if not size or not 0x20000000 <= address < address + size <= 0x20400000:
            raise ValueError("native static backing lies outside SRAM: " + name)
        if name == "shared" and (size != 512 or address % 512):
            raise ValueError("shared MPU grant does not reserve its aligned extent")
        if name == "worker_stack" and (size != 2048 or address % 2048):
            raise ValueError("worker stack does not reserve its aligned extent")
        if any(address < end and begin < address + size for _, begin, end in extents):
            raise ValueError("native static backing overlaps")
        extents.append((name, address, address + size))
    vectors = {}
    for exception, name in ((11, "SVC_Handler"), (14, "PendSV_Handler"), (15, "SysTick_Handler"),
                            (16, "UART0RX_IRQHandler"), (17, "UART0TX_IRQHandler")):
        value = struct.unpack("<I", elf_bytes(data, table["__isr_vector"][0] + 4 * exception, 4))[0]
        if value != table[name][0] | 1 or table[name][0] == table["Default_Handler"][0]:
            raise ValueError("native vector does not select its intended Thumb handler: " + name)
        vectors[name] = value
    if re.search(r"\b(?:__atomic_|__sync_)", symbols):
        raise ValueError("native word atomics unexpectedly require runtime helpers")
    inputs = [HERE / "arm/main.c", HERE / "arm/FreeRTOSConfig.h", HERE / "arm/ove_config.h",
              HERE / "arm/CMakeLists.txt", folder / "build/compile_commands.json"]
    result = {"elf_sha256": hashlib.sha256(data).hexdigest(), "svc_sites": sites,
              "compiled_metadata": metadata, "static_extents": extents, "vectors": vectors,
              "inputs": {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
              "symbols_sha256": hashlib.sha256(symbols.encode()).hexdigest(),
              "assembly_sha256": hashlib.sha256(assembly.encode()).hexdigest()}
    (folder / "eligibility.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--freertos", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--provider", type=Path)
    parser.add_argument("--all-callers-privileged", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--one-profile", action="store_true")
    parser.add_argument("--approved-images", type=Path,
                        help="externally inspected profile-to-ELF-hash map required before guests")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--budget", type=float, default=1800)
    args = parser.parse_args()
    if args.provider and args.provider.resolve() != ROOT / "backends/freertos/freertos_irq.c" and not args.all_callers_privileged:
        parser.error("alternate providers require --all-callers-privileged")
    if not args.build_only and not args.approved_images:
        parser.error("guest execution requires --approved-images from external artifact inspection")
    approved = json.loads(args.approved_images.read_text()) if args.approved_images else {}
    deadline = time.monotonic() + args.budget
    def run(command, stream, maximum):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("native IRQ gate exhausted its cumulative budget")
        return _run_owned_test_process(command, env=env, timeout=min(maximum, remaining), stdout=stream)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    explicit = env.get("OVE_FREERTOS_IRQ_TARGET_CANARY") == "1"
    selected = list(profiles())[:1] if args.one_profile else list(profiles())
    rows = []
    for mpu, enforce, allow, zero in selected:
        for canary in (False, True):
            label = f"mpu{mpu}-enforce{enforce}-allow{allow}-zero{zero}" + ("-canary" if canary else "")
            folder = out / label
            folder.mkdir(parents=True, exist_ok=True)
            command = ["cmake", "-S", str(HERE / "arm"), "-B", str(folder / "build"),
                       "-DOVE_TOOLCHAIN_DIR=" + str(args.toolchain.resolve()),
                       "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake"),
                       "-DFREERTOS_PATH=" + str(args.freertos.resolve()), "-DOVE_MPU_V1=" + str(mpu),
                       "-DOVE_ZERO_HEAP=" + str(zero), "-DOVE_SYSCALL_ENFORCE=" + str(enforce),
                       "-DOVE_ALLOW_UNPRIVILEGED_CRITICAL=" + str(allow), "-DOVE_ERRATUM=" + str(int(not mpu)),
                       "-DOVE_ALL_CALLERS_PRIVILEGED=" + str(int(args.all_callers_privileged)),
                       "-DOVE_IRQ_TARGET_CANARY=" + str(int(canary or explicit))]
            if args.provider:
                command.append("-DOVE_IRQ_PROVIDER=" + str(args.provider.resolve()))
            for stage, stage_command in (("configure", command), ("build", ["cmake", "--build", str(folder / "build"), "--parallel", "8"])):
                if args.skip_build:
                    continue
                log = folder / (stage + ".log")
                with log.open("wb") as stream:
                    status = run(stage_command, stream, 240)
                if status:
                    print(log.read_text(errors="backslashreplace"), end="")
                    return 1
            binary = folder / "build/irq_masks_target"
            alternate = args.provider is not None and args.provider.resolve() != ROOT / "backends/freertos/freertos_irq.c"
            eligibility = qualify(binary, folder, args.toolchain.resolve(), (mpu, enforce, allow, zero),
                                  canary or explicit, alternate, args.all_callers_privileged)
            if args.build_only:
                rows.append({"profile": label, "stage": "build-only", **eligibility})
                (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
                continue
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            if digest != eligibility["elf_sha256"] or approved.get(label) != digest:
                raise ValueError("native image lacks exact external artifact approval: " + label)
            command = [args.qemu, "-M", "mps2-an500", "-cpu", "cortex-m7", "-m", "16",
                       "-nographic", "-monitor", "none", "-semihosting-config", "enable=on,target=native", "-kernel", str(binary)]
            log = folder / "run.log"
            with log.open("wb") as stream:
                status = run(command, stream, 30)
            data = log.read_bytes()
            prefix = b"FAIL: early failure survives later native IRQ controls\n" if canary else b""
            workers = 2 if mpu and not args.all_callers_privileged else 1
            checks = 12 + workers * 9 + int(canary)
            footer = (f"freertos irq native: mpu={mpu} enforce={enforce} allow={allow} all_priv={int(args.all_callers_privileged)} "
                      f"heap={1-zero} cases={5+workers} workers={workers} headroom=([0-9]+) checks={checks} failures={int(canary)}\n").encode()
            match = re.fullmatch(re.escape(prefix) + footer, data)
            valid = status == int(canary) and match is not None and 512 <= int(match[1]) <= 2048
            rows.append({"profile": label, "stage": "run", "command": command, "status": status, "valid": valid,
                         "log": str(log), "log_sha256": hashlib.sha256(data).hexdigest(), "elf_sha256_before_run": digest, **eligibility})
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(label, "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(data.decode(errors="backslashreplace"), end="")
            if status not in (0, 1) or not re.search(rb"^freertos irq native: .* failures=[0-9]+\n\Z", data, re.MULTILINE):
                return 1
    failed = sum(not row.get("valid", True) for row in rows)
    print(f"freertos irq native gate: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
