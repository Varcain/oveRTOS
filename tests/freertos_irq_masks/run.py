# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe actual IRQ adapter bodies against initialized scalar register models."""
import argparse
import hashlib
import itertools
import json
import os
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.test import _run_owned_test_process

REJECTIONS = ("fast", "systick", "pendsv", "nmi", "hardfault", "fault", "supervisor",
              "debug", "reserved", "impossible", "group", "bits-low", "bits-high",
              "bits-mismatch", "threshold-zero", "threshold-wide", "threshold-unaligned",
              "eight-odd", "eight-group")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--provider", type=Path, default=ROOT / "backends/freertos/freertos_irq.c")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--one-profile", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    deadline = time.monotonic() + 300
    def run(command, stream, maximum):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("IRQ model gate exhausted its cumulative budget")
        return _run_owned_test_process(command, env=env, timeout=min(maximum, remaining), stdout=stream)
    explicit = env.get("OVE_FREERTOS_IRQ_MASKS_CANARY") == "1"
    if args.sanitize:
        env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
    rows = []
    profiles = [(mpu, allow, erratum, zero) for (mpu, allow), erratum, zero in itertools.product(((0, 0), (1, 0), (1, 1)), (0, 1), (0, 1))]
    if args.one_profile:
        profiles = profiles[:1]
    for mpu, allow, erratum, zero in profiles:
        label = f"mpu{mpu}-allow{allow}-erratum{erratum}-zero{zero}"
        folder = out / label
        folder.mkdir(parents=True, exist_ok=True)
        config = "#pragma once\n#define CONFIG_OVE_RTOS_FREERTOS 1\n#define CONFIG_OVE_SYNC 1\n#define CONFIG_OVE_ASYNC 1\n"
        if zero:
            config += "#define CONFIG_OVE_ZERO_HEAP 1\n"
        (folder / "ove_config.h").write_text(config)
        binary = folder / "model"
        command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-DOVE_MODEL_MPU=" + str(mpu), "-DOVE_MODEL_ALLOW=" + str(allow), "-DOVE_MODEL_ERRATUM=" + str(erratum),
                   "-DOVE_FREERTOS_IRQ_PORT_SUPPORTED=1", "-DOVE_IRQ_SOURCE=" + json.dumps(str(args.provider.resolve())),
                   "-I" + str(folder), "-I" + str(HERE / "sdk"), "-I" + str(ROOT / "include"),
                   "-I" + str(ROOT / "backends/common"), "-I" + str(ROOT / "backends/freertos/include"),
                   str(HERE / "model.c"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        log = folder / "build.log"
        with log.open("wb") as stream:
            status = run(command, stream, 90)
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        scenarios = ("normal",) + REJECTIONS + (() if mpu else ("ordinary-user",))
        for case in scenarios:
            for canary in (False, True):
                selected = canary or explicit
                selector = ("canary" if selected else "normal") if case == "normal" else ("reject-canary" if selected else "reject")
                command = [str(binary), selector] + ([] if case == "normal" else [case])
                log = folder / (case + ("-canary.log" if canary else ".log"))
                with log.open("wb") as stream:
                    status = run(command, stream, 10)
                data = log.read_bytes()
                count = (339 if mpu else 333) if case == "normal" else 1
                checks = ((3546 if mpu else 3462) if case == "normal" else 3) + int(canary)
                expected = (b"FAIL: early failure survives every later IRQ control\n" if canary else b"")
                expected += f"irq masks: cases={count} checks={checks} failures={int(canary)} rejected={int(case != 'normal')}\n".encode()
                valid = status == int(canary) and data == expected
                rows.append({"profile": label, "case": case, "canary": canary, "command": command, "status": status,
                             "valid": valid, "log": str(log), "log_sha256": hashlib.sha256(data).hexdigest(),
                             "elf_sha256_before_run": digest, "provider_sha256": hashlib.sha256(args.provider.read_bytes()).hexdigest()})
                (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
                print(label + "/" + case + ("/canary" if canary else ""), "PASS" if valid else "FAIL", flush=True)
                if not valid:
                    print(data.decode(errors="backslashreplace"), end="")
                if status not in (0, 1) or not re.search(rb"^irq masks: cases=[0-9]+ checks=[0-9]+ failures=[0-9]+ rejected=[01]\n\Z", data, re.MULTILINE):
                    return 1
    unsupported = {
        "missing-port": "#undef OVE_FREERTOS_IRQ_PORT_SUPPORTED\n",
        "other-port": "#undef OVE_FREERTOS_IRQ_PORT_SUPPORTED\n#define OVE_FREERTOS_IRQ_PORT_SUPPORTED 0\n",
        "other-architecture": "#undef OVE_IRQ_MASK_CORTEX_M\n",
        "multiple-cores": "#undef configNUMBER_OF_CORES\n#define configNUMBER_OF_CORES 2\n",
        "missing-cores": "#undef configNUMBER_OF_CORES\n",
        "mpu-v2": "#define portUSING_MPU_WRAPPERS 1\n#define configUSE_MPU_WRAPPERS_V1 0\n",
        "missing-priority-bits": "#undef configPRIO_BITS\n",
        "missing-threshold": "#undef configMAX_SYSCALL_INTERRUPT_PRIORITY\n",
    }
    for label, adjustment in unsupported.items():
        folder = out / label
        folder.mkdir(exist_ok=True)
        (folder / "ove_config.h").write_text("#pragma once\n#define CONFIG_OVE_ASYNC 1\n")
        wrapper = folder / "provider.c"
        wrapper.write_text('#include <ove_irq_mask_ops.h>\n' + adjustment +
                           '#include ' + json.dumps(str(args.provider.resolve())) + '\n')
        binary = folder / "model"
        command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                   "-DOVE_MODEL_MPU=0", "-DOVE_MODEL_ALLOW=0", "-DOVE_MODEL_ERRATUM=1",
                   "-DOVE_FREERTOS_IRQ_PORT_SUPPORTED=1", "-DOVE_IRQ_SOURCE=" + json.dumps(str(wrapper)),
                   "-I" + str(folder), "-I" + str(HERE / "sdk"), "-I" + str(ROOT / "include"),
                   "-I" + str(ROOT / "backends/common"), "-I" + str(ROOT / "backends/freertos/include"),
                   str(HERE / "model.c"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
        log = folder / "build.log"
        with log.open("wb") as stream:
            status = run(command, stream, 90)
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
        for canary in (False, True):
            command = [str(binary), "reject-canary" if canary or explicit else "reject", "unsupported"]
            log = folder / ("canary.log" if canary else "normal.log")
            with log.open("wb") as stream:
                status = run(command, stream, 10)
            data = log.read_bytes()
            expected = b"FAIL: early failure survives every later IRQ control\n" if canary else b""
            expected += f"irq masks: cases=1 checks={3+int(canary)} failures={int(canary)} rejected=1\n".encode()
            valid = status == int(canary) and data == expected
            rows.append({"profile": label, "case": "unsupported", "canary": canary, "status": status,
                         "valid": valid, "command": command, "log": str(log),
                         "elf_sha256_before_run": hashlib.sha256(binary.read_bytes()).hexdigest(),
                         "log_sha256": hashlib.sha256(data).hexdigest()})
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(label + ("/canary" if canary else ""), "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(data.decode(errors="backslashreplace"), end="")
            if status not in (0, 1) or not re.search(rb"^irq masks: cases=[0-9]+ checks=[0-9]+ failures=[0-9]+ rejected=[01]\n\Z", data, re.MULTILINE):
                return 1
    failed = sum(not row["valid"] for row in rows)
    print(f"freertos irq masks gate: observations={len(rows)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
