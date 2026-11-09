# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile native bindings with real headers in no_std, alloc and std modes."""
import argparse
import hashlib
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    for key in tuple(env):
        if key in ("DOCS_RS", "CARGO_BUILD_TARGET", "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS") or key.startswith("BINDGEN_EXTRA_CLANG_ARGS"):
            env.pop(key)
    # Empty encoded flags override target-specific/config-file cfg injections.
    env["CARGO_ENCODED_RUSTFLAGS"] = ""
    env.update(OVE_DIR=str(ROOT), RUST_IS_NATIVE="1", CARGO_TARGET_DIR=str(out / "cargo"),
               LVGL_INCLUDE_PATH=str(ROOT / "tests/backends/stub/lvgl"),
               LVGL_PARENT_PATH=str(ROOT / "tests/backends/stub"))
    deadline = time.monotonic() + 600

    def run(command, log, maximum):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("native core binding gate exhausted its cumulative budget")
        with log.open("wb") as stream:
            return _run_owned_test_process(command, env=env, timeout=min(maximum, remaining), stdout=stream)

    version = out / "rustc-version.log"
    if run(["rustc", "-vV"], version, 15):
        print(version.read_text(errors="backslashreplace"), end="")
        return 1
    hosts = re.findall(r"^host: (\S+)$", version.read_text(), re.MULTILINE)
    if len(hosts) != 1:
        raise ValueError("rustc did not identify one native host target")
    host = hosts[0]
    source = ROOT / "bindings/rust/ove/build.rs"
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    (out / "build-source.rs").write_bytes(source.read_bytes())
    failures = int(env.get("OVE_RUST_NATIVE_CORE_CANARY") == "1")
    if failures:
        print("FAIL: early failure survives every native binding profile", flush=True)
    rows = []
    for zero in (0, 1):
        for feature in ("no_std", "alloc", "std"):
            label = ("zero" if zero else "heap") + "-" + feature
            folder = out / label
            folder.mkdir(exist_ok=True)
            config = "#pragma once\n#define CONFIG_OVE_RTOS_POSIX 1\n"
            if zero:
                config += "#define CONFIG_OVE_ZERO_HEAP 1\n"
            (folder / "ove_config.h").write_text(config)
            storage = folder / "storage"
            storage.mkdir(exist_ok=True)
            for stage, command in (("configure", ["cmake", "-S", str(ROOT / "tests/rust/stub_cmake"), "-B", str(storage),
                                                   "-DOVE_STUB_CONFIG_DIR=" + str(folder)]),
                                   ("build", ["cmake", "--build", str(storage), "--target", "storage_sizes", "--parallel", "8"])):
                log = storage / (stage + ".log")
                if run(command, log, 60):
                    print(log.read_text(errors="backslashreplace"), end="")
                    return 1
            env.update(OVE_GEN_DIR=str(folder), OVE_STORAGE_SIZES=str(storage / "ove_storage_sizes.env"),
                       CARGO_TARGET_DIR=str(folder / "cargo"))
            command = ["cargo", "check", "--locked", "--lib", "--no-default-features",
                       "--target", host, "--message-format=json",
                       "--manifest-path", str(ROOT / "bindings/rust/ove/Cargo.toml")]
            if feature != "no_std":
                command += ["--features", feature]
            log = folder / "cargo.log"
            status = run(command, log, 120)
            data = log.read_bytes()
            records = []
            for line in data.decode(errors="backslashreplace").splitlines():
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError:
                    continue
            package = "path+" + (ROOT / "bindings/rust/ove").as_uri() + "#"
            generated = [Path(row["out_dir"]) / "ove_bindings.rs" for row in records
                         if row.get("reason") == "build-script-executed" and row.get("package_id", "").startswith(package)]
            if len(generated) != 1 or not generated[0].is_relative_to(folder / "cargo") or not generated[0].is_file():
                print("missing exact generated native binding record", flush=True)
                return 1
            binding = generated[0].read_bytes()
            (folder / "bindings.rs").write_bytes(binding)
            code = re.sub(r'"(?:[^"\\]|\\.)*"|//[^\n]*|/\*.*?\*/', "", binding.decode(), flags=re.DOTALL)
            core = "::std::" not in code and "std::" not in code
            stable = hashlib.sha256(source.read_bytes()).hexdigest() == before
            valid = status == 0 and core and stable
            rows.append({"profile": label, "status": status, "valid": valid, "command": command,
                         "log": str(log), "log_sha256": hashlib.sha256(data).hexdigest(),
                         "build_source_sha256": before, "source_unchanged": stable, "host_target": host,
                         "generated_origin": str(generated[0]), "bindings_sha256": hashlib.sha256(binding).hexdigest(),
                         "core_namespace": core, "compiled": status == 0})
            (out / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
            print(label, "PASS" if valid else "FAIL", flush=True)
            if not valid:
                failures += 1
                # A complete compiler rejection is distinct from setup, signal
                # or timeout failure. Preserve the full diagnostic in its log.
                messages = [row.get("message", {}) for row in records if row.get("reason") == "compiler-message"]
                unresolved_std = any(message.get("code", {}).get("code") == "E0433" and
                                     any(text in message.get("message", "") for text in ("could not find `std`", "cannot find `std`")) for message in messages
                                     if isinstance(message.get("code"), dict))
                semantic = stable and ((status == 101 and unresolved_std) or (status == 0 and not core))
                if not semantic:
                    print(data.decode(errors="backslashreplace"), end="")
                    return 1
    print(f"native core bindgen gate: observations={len(rows)} failed={failures}", flush=True)
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
