# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check exact finite conversion and inert FreeRTOS consumer arguments."""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.manifest import get_component, load_manifest
from ove.test import _run_owned_test_process
from ove.utils import hashed_dir

RATES = (3, 100, 1000, 1024, 3000, 20000, 1000000000, 1000000001,
         3000000000, 18446744073709551615)


def prepare_lwip(out, env, cache):
    manifest = load_manifest(str(ROOT))
    revision = get_component(manifest, "libraries", "lwip", "version")
    url = get_component(manifest, "libraries", "lwip", "url")
    if not revision or not url:
        raise ValueError("The manifest must specify the lwIP source and revision")
    source = Path(hashed_dir(str(cache), "lwip", revision, None)[0])
    headers = [source / "src/include/lwip" / name for name in ("dns.h", "sockets.h")]
    cached = all(path.is_file() for path in headers)
    command, status = None, 0
    log = out / "lwip.log"
    if cached:
        log.write_text("Using cached manifest lwIP headers\n")
    else:
        cache.mkdir(parents=True, exist_ok=True)
        command = ["git", "clone", "--depth", "1", "--branch", revision, url, str(source)]
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=120, stdout=stream)
    valid = status == 0 and all(path.is_file() for path in headers)
    row = {"command": command, "status": status, "valid": valid, "cached": cached,
           "source": str(source), "revision": revision, "log": str(log),
           "sha256": hashlib.sha256(log.read_bytes()).hexdigest()}
    (out / "lwip.json").write_text(json.dumps(row, indent=2) + "\n")
    if not valid:
        print(log.read_text(errors="backslashreplace"), end="")
        print("Pinned lwIP headers are unavailable", flush=True)
        return None
    return source


def expected_output(name, canary):
    fields = name.split("_")
    bits, rate = fields[1:3]
    if fields[0] == "arith":
        count = 89
        prefix = f"freertos timeout arithmetic: bits={bits} rate={rate} cases=30"
    else:
        count = (525 if fields[3] == "heap" else 521) + (4 if rate == "3000" else 0)
        prefix = f"freertos timeout native observers: bits={bits} rate={rate} heap={int(fields[3] == 'heap')}"
    return (("FAIL: early failure survives later controls\n" if canary else "") +
            f"{prefix} checks={count + int(canary)} failures={int(canary)}\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source-cache", type=Path, default=ROOT / "dl",
                        help="Cache directory for manifest lwIP sources")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if args.sanitize:
        env.update(ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                   UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", LSAN_OPTIONS="exitcode=23")
    lwip = prepare_lwip(out, env, args.source_cache.resolve())
    if lwip is None:
        return 1
    rows, commands, diagnostics = [], [], []
    explicit = env.get("OVE_FREERTOS_TIMEOUTS_CANARY") == "1"
    for name, command in (
        ("configure", ["cmake", "-S", str(HERE), "-B", str(out / "build"),
                       "-DOVE_EXTENDED_RATES=ON", "-DOVE_NATIVE_MODELS=ON",
                       "-DOVE_LWIP_DIR=" + str(lwip),
                       "-DOVE_SANITIZE=" + ("ON" if args.sanitize else "OFF")]),
        ("build", ["cmake", "--build", str(out / "build"), "--parallel", "8"]),
    ):
        log = out / (name + ".log")
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=240, stdout=stream)
        commands.append({"command": command, "status": status, "log": str(log),
                         "sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        (out / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if status:
            print(log.read_text(errors="backslashreplace"), end="")
            return 1
    for bits in (16, 32, 64):
        command = ["cmake", "--build", str(out / "build"), "--target", f"reject_zero_{bits}"]
        log = out / f"reject-zero-{bits}.log"
        with log.open("wb") as stream:
            status = _run_owned_test_process(command, env=env, timeout=30, stdout=stream)
        data = log.read_bytes()
        valid = status in (1, 2) and b"FreeRTOS tick rate must be positive" in data
        row = {"command": command, "status": status, "valid": valid, "log": str(log),
               "sha256": hashlib.sha256(data).hexdigest(),
               "qualifier": "explicit invalid-zero compiler diagnostic; no execution"}
        diagnostics.append(row)
        (out / "diagnostics.json").write_text(json.dumps(diagnostics, indent=2) + "\n")
        print(f"zero-{bits}", "PASS" if valid else "FAIL", flush=True)
        if not valid:
            print(data.decode(errors="backslashreplace"), end="")
    names = [f"arith_{bits}_{rate}" for bits in (16, 32, 64) for rate in RATES]
    names += [f"native_{bits}_{rate}_{mode}" for bits in (16, 32, 64)
              for rate in (1000, 3000, 20000) for mode in ("heap", "zero")]
    for name in names:
        binary = out / "build" / name
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        for canary in (False, True):
            active = canary or explicit
            command = [str(binary)] + (["canary"] if active else [])
            log = out / (name + ("-canary" if canary else "-normal") + ".log")
            with log.open("wb") as stream:
                status = _run_owned_test_process(command, env=env, timeout=15, stdout=stream)
            data = log.read_bytes()
            valid = not explicit and status == int(active) and data == expected_output(name, active)
            row = {"command": command, "status": status, "valid": valid,
                   "elf_sha256": digest, "log": str(log),
                   "sha256": hashlib.sha256(data).hexdigest(),
                   "qualifier": "explicit tick/rate models; inert initialized entries, actual DNS body/lwIP headers; Event accounting remains separate"}
            rows.append(row)
            log.with_suffix(".status.json").write_text(json.dumps(row, indent=2) + "\n")
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(name, "canary" if canary else "normal", "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(data.decode(errors="backslashreplace"), end="")
    if len(rows) != 96 or len(diagnostics) != 3:
        raise ValueError("incomplete FreeRTOS timeout observations")
    failed = sum(not row["valid"] for row in rows + diagnostics)
    print(f"freertos timeouts: observations={len(rows)} diagnostics={len(diagnostics)} failed={failed}", flush=True)
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
