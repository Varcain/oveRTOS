# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check native CV callers, foreign-stack retirement and original context."""
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
    for zero in (False, True):
        yield False, False, 0, zero, False
    for enforce, allow, zero in itertools.product((False, True), (0, 1), (False, True)):
        yield True, enforce, allow, zero, False
    for mpu, zero in itertools.product((False, True), repeat=2):
        yield mpu, mpu, 0, zero, True


def valid_output(data, mpu, enforce, allow, zero, trace, canary, all_priv):
    contexts = 1 + int(mpu and not trace and not all_priv)
    cases, workers = 8 * contexts + 3, 17 * contexts + 3
    checks = 91 * contexts + 21 + cases * int(trace) + int(canary) + int(mpu)
    prefix = "FAIL: setup: early failure survives later native controls\n" if canary else ""
    expression = (prefix + f"freertos cv native: mpu={int(mpu)} enforce={int(enforce)} "
                  f"allow={allow} trace={int(trace)} all_priv={int(all_priv)} heap={int(not zero)} "
                  f"cases={cases} workers={workers} headroom=([0-9]+) checks={checks} "
                  f"failures={int(canary)}\n")
    match = re.fullmatch(expression.encode(), data)
    return match is not None and 256 <= int(match[1]) <= 2048


def complete_footer(data, all_priv):
    pattern = (rb"^freertos cv native: mpu=[01] enforce=[01] allow=[01] trace=[01] "
               rb"all_priv=(?P<role>[01]) heap=[01] cases=[0-9]+ workers=[0-9]+ "
               rb"headroom=[0-9]+ checks=[0-9]+ failures=[0-9]+\n\Z")
    match = re.search(pattern, data, re.MULTILINE)
    return match is not None and int(match["role"]) == int(all_priv)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--freertos", required=True, type=Path)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--provider", type=Path)
    parser.add_argument("--backend-include", type=Path)
    parser.add_argument("--all-callers-privileged", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--verified-images", type=Path)
    args = parser.parse_args()
    alternate = (
        args.provider is not None and args.provider.resolve() != ROOT / "backends/freertos/freertos_sync.c"
    ) or (
        args.backend_include is not None and args.backend_include.resolve() != ROOT / "backends/freertos/include"
    )
    if alternate and not args.all_callers_privileged:
        parser.error("alternate native providers require --all-callers-privileged")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    metadata = out / "target-observations.json"
    env = dict(os.environ)
    verified = ({row["profile"]: row["elf_sha256"] for row in json.loads(args.verified_images.read_text())}
                if args.verified_images else None)
    explicit = env.get("OVE_FREERTOS_CV_CONTEXT_TARGET_CANARY") == "1"
    for mpu, enforce, allow, zero, trace in profiles():
        label = ("v1" if mpu else "ordinary") + f"-enforce{int(enforce)}-allow{allow}"
        label += "-" + ("zero" if zero else "heap") + ("-trace" if trace else "")
        folder = out / label
        folder.mkdir(parents=True, exist_ok=True)
        configure = ["cmake", "-S", str(HERE / "arm"), "-B", str(folder / "build"),
                     "-DOVE_TOOLCHAIN_DIR=" + str(args.toolchain.resolve()),
                     "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake"),
                     "-DFREERTOS_PATH=" + str(args.freertos.resolve()),
                     "-DOVE_MPU_V1=" + str(int(mpu)),
                     "-DOVE_ZERO_HEAP=" + str(int(zero)),
                     "-DOVE_ALLOW_UNPRIVILEGED_CRITICAL=" + str(allow),
                     "-DOVE_SYSCALL_ENFORCE=" + str(int(enforce)),
                     "-DOVE_NATIVE_TRACE=" + str(int(trace)),
                     "-DOVE_ALL_CALLERS_PRIVILEGED=" + str(int(args.all_callers_privileged)),
                     # The newer virtual CPU supports V1 workaround0. Ordinary
                     # CM7 always executes its stock erratum instruction sequence.
                     "-DOVE_ERRATUM=" + str(int(not mpu)),
                     "-DOVE_CV_CONTEXT_TARGET_CANARY=" + str(int(explicit))]
        if args.provider:
            configure.append("-DOVE_CV_PROVIDER=" + str(args.provider.resolve()))
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
        binary = folder / "build/cv_context_target"
        if args.build_only:
            continue
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        if verified is not None and verified.get(label) != digest:
            raise ValueError("native image differs from its reviewed artifact: " + label)
        command = [args.qemu, "-M", "mps2-an500", "-cpu", "cortex-m7", "-m", "16",
                   "-nographic", "-monitor", "none", "-semihosting-config", "enable=on,target=native",
                   "-kernel", str(binary)]
        log = folder / "run.log"
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=30, stdout=stream)
        data = log.read_bytes()
        expected = status == int(explicit) and valid_output(
            data, mpu, enforce, allow, zero, trace, explicit, args.all_callers_privileged
        )
        valid = not explicit and expected
        row = {"profile": label, "stage": "run", "command": command, "status": status,
               "valid": valid, "expected_output_and_status": expected,
               "all_callers_privileged": args.all_callers_privileged,
               "elf_sha256_before_run": digest, "log": str(log),
               "sha256": hashlib.sha256(data).hexdigest(),
               "qualifier": "real initialized native32 CV tasks;own stacks plus shared state only;IRQ1 marker only;stock V1 workaround0 runtime;trace/all-alternate callers privileged;no V2/native64/physical timing/whole-stack qualification"}
        rows.append(row)
        log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
        metadata.write_text(json.dumps(rows, indent=2) + "\n")
        print(label, "PASS" if valid else "FAIL", flush=True)
        if not valid:
            print(data.decode(errors="backslashreplace"), end="")
        if (
            status not in (0, 1) or not complete_footer(data, args.all_callers_privileged)
        ):
            return 1
    if args.build_only:
        if len(rows) != 28:
            raise ValueError("incomplete native compile-only profiles")
        print("freertos cv native build gate: profiles=14 failed=0", flush=True)
        return 0
    observations = [row for row in rows if row["stage"] == "run"]
    if len(rows) != 42 or len(observations) != 14:
        raise ValueError("incomplete native CV context observations")
    failed = sum(not row["valid"] for row in observations)
    print(f"freertos cv native gate: observations={len(observations)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
