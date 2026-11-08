# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check initialized shared Event callers, privilege and IRQ masks on Cortex-M7."""
import argparse
import hashlib
import itertools
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.test import _run_owned_test_process


def profiles():
    for mpu, abort, zero in itertools.product((False, True), repeat=3):
        for enforce in ((False, True) if mpu else (False,)):
            yield mpu, enforce, abort, zero, False
    for mpu, zero in itertools.product((False, True), repeat=2):
        yield mpu, mpu, False, zero, True


def valid_output(data, mpu, enforce, abort, zero, trace, canary, producer_priv_only=False):
    contexts = 1 + int(mpu and not trace)
    cases = (13 + 3 * int(abort)) * contexts + 5
    workers = (31 + 9 * int(abort)) * contexts + 5
    checks = ((159 + 45 * int(abort)) * contexts + 37 + int(mpu)
              + cases * int(trace) + int(canary))
    prefix = "FAIL: setup: early failure survives all later native controls\n" if canary else ""
    expression = (prefix + f"freertos event waiters native: mpu={int(mpu)} "
                  f"enforce={int(enforce)} erratum={int(not mpu)} abort={int(abort)} "
                  f"trace={int(trace)} producer_priv_only={int(producer_priv_only)} "
                  f"heap={int(not zero)} ticks=32 cases={cases} "
                  f"workers={workers} headroom=([0-9]+) checks={checks} failures={int(canary)}\n")
    match = re.fullmatch(expression.encode(), data)
    return match is not None and 256 <= int(match[1]) <= 2048


def complete_footer(data, producer_priv_only):
    pattern = (rb"^freertos event waiters native: mpu=[01] enforce=[01] erratum=[01] "
               rb"abort=[01] trace=[01] producer_priv_only=(?P<role>[01]) heap=[01] "
               rb"ticks=32 cases=[0-9]+ workers=[0-9]+ headroom=[0-9]+ "
               rb"checks=[0-9]+ failures=[0-9]+\n\Z")
    match = re.search(pattern, data, re.MULTILINE)
    return match is not None and int(match["role"]) == int(producer_priv_only)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--freertos", required=True, type=Path)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--provider", type=Path)
    parser.add_argument("--backend-include", type=Path)
    parser.add_argument("--privileged-producer-only", action="store_true")
    parser.add_argument("--stop-on-invalid-exit", action="store_true")
    args = parser.parse_args()
    alternate = (
        args.provider is not None and args.provider.resolve() != ROOT / "backends/freertos/freertos_sync.c"
    ) or (
        args.backend_include is not None and args.backend_include.resolve() != ROOT / "backends/freertos/include"
    )
    if alternate and not args.privileged_producer_only:
        parser.error("alternate native providers require --privileged-producer-only")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    metadata = out / "target-observations.json"
    env = dict(os.environ)
    explicit = env.get("OVE_FREERTOS_EVENT_WAITERS_TARGET_CANARY") == "1"
    for mpu, enforce, abort, zero, trace in profiles():
        label = ("v1" if mpu else "ordinary") + f"-enforce{int(enforce)}-abort{int(abort)}"
        label += "-" + ("zero" if zero else "heap") + ("-trace" if trace else "")
        folder = out / label
        folder.mkdir(parents=True, exist_ok=True)
        configure = ["cmake", "-S", str(HERE / "arm"), "-B", str(folder / "build"),
                     "-DOVE_TOOLCHAIN_DIR=" + str(args.toolchain.resolve()),
                     "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake"),
                     "-DFREERTOS_PATH=" + str(args.freertos.resolve()),
                     "-DOVE_MPU_V1=" + str(int(mpu)),
                     "-DOVE_ZERO_HEAP=" + str(int(zero)),
                     "-DOVE_NATIVE_ABORT=" + str(int(abort)),
                     "-DOVE_SYSCALL_ENFORCE=" + str(int(enforce)),
                     "-DOVE_NATIVE_TRACE=" + str(int(trace)),
                     "-DOVE_PRIVILEGED_PRODUCER_ONLY=" + str(int(args.privileged_producer_only)),
                     # The newer virtual CPU supports V1 workaround0. Ordinary
                     # CM7 always executes its stock erratum instruction sequence.
                     "-DOVE_ERRATUM=" + str(int(not mpu)),
                     "-DOVE_EVENT_WAITERS_TARGET_CANARY=" + str(int(explicit))]
        if args.provider:
            configure.append("-DOVE_EVENT_PROVIDER=" + str(args.provider.resolve()))
        if args.backend_include:
            configure.append("-DOVE_BACKEND_INCLUDE=" + str(args.backend_include.resolve()))
        for stage, command in (
            ("configure", configure),
            ("build", ["cmake", "--build", str(folder / "build"), "--parallel", "8"]),
        ):
            log = folder / (stage + ".log")
            with log.open("wb") as stream:
                status = _run_owned_test_process(command, env=env, timeout=240, stdout=stream)
            row = {"profile": label, "stage": stage, "command": command, "status": status,
                   "log": str(log), "sha256": hashlib.sha256(log.read_bytes()).hexdigest()}
            rows.append(row)
            metadata.write_text(json.dumps(rows, indent=2) + "\n")
            if status:
                print(log.read_text(errors="backslashreplace"), end="")
                return 1
        binary = folder / "build/event_waiters_target"
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        command = [args.qemu, "-M", "mps2-an500", "-cpu", "cortex-m7", "-m", "16",
                   "-nographic", "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                   "-kernel", str(binary)]
        log = folder / "run.log"
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=30, stdout=stream)
        data = log.read_bytes()
        expected = status == int(explicit) and valid_output(
            data, mpu, enforce, abort, zero, trace, explicit, args.privileged_producer_only
        )
        valid = not explicit and expected
        row = {"profile": label, "stage": "run", "command": command, "status": status,
               "valid": valid, "expected_output_and_status": expected,
               "privileged_producer_only": args.privileged_producer_only,
               "elf_sha256_before_run": digest, "log": str(log),
               "sha256": hashlib.sha256(data).hexdigest(),
               "qualifier": "real initialized native32 waiters;kernel-aware IRQ0;above-threshold IRQ1 observes only;V1 workaround0 runtime;trace privileged only;no native64/physical timing/fault recovery qualification"}
        rows.append(row)
        log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
        metadata.write_text(json.dumps(rows, indent=2) + "\n")
        print(label, "PASS" if valid else "FAIL", flush=True)
        if not valid:
            print(data.decode(errors="backslashreplace"), end="")
        if args.stop_on_invalid_exit and (
            status not in (0, 1) or not complete_footer(data, args.privileged_producer_only)
        ):
            return 1
    observations = [row for row in rows if row["stage"] == "run"]
    if len(rows) != 48 or len(observations) != 16:
        raise ValueError("incomplete native shared Event observations")
    failed = sum(not row["valid"] for row in observations)
    print(f"freertos event waiters native gate: observations={len(observations)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
