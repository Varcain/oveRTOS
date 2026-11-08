# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile ordinary ownership, feature and unsafe-boundary callers."""
import hashlib
import json
import subprocess
from pathlib import Path

CASES = {
    "owner_traits": "pub fn traits<T:Send+Sync+core::fmt::Debug>(){} pub fn control(){traits::<ove::EventGroup>();}",
    "storage_traits": "static S:ove::EventGroupStorage=ove::EventGroupStorage::new(); pub fn traits<T:Sync+Default>(){} pub fn control(){traits::<ove::EventGroupStorage>();let _=&S;}",
    "local_storage": "pub fn control()->ove::Result<ove::EventGroup>{let s=ove::EventGroupStorage::new();ove::EventGroup::create(&s)}",
    "relative_time": "pub fn control(eg:&ove::EventGroup){let _=eg.wait_bits_for(1,ove::WaitFlags::NONE,core::time::Duration::ZERO);}",
    "mask_query": "pub fn control()->u32{ove::EventGroup::valid_mask()}",
    "heap_new": "pub fn control(){let _=ove::EventGroup::new();}",
    "raw_unsafe": "pub fn control(p:*mut ove::ffi::ove_eventgroup_storage_t){let _=ove::EventGroup::from_static(p);}",
    "deadline": "pub fn control(eg:&ove::EventGroup){let _=eg.wait_bits_until(1,ove::WaitFlags::NONE,DEADLINE);}",
    "async_consumer": "pub fn control(eg:ove::EventGroup){let _=ove::AsyncEventGroup::new(eg);}",
}


def verify(library, output, env, *, zero=False, asynchronous=False):
    library, output = Path(library), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, source in CASES.items():
        if name == "async_consumer" and not asynchronous:
            continue
        source = source.replace("DEADLINE", "ove::time::Instant::FOREVER" if asynchronous else "loop {}")
        path = output / (name + ".rs")
        path.write_text(source)
        command = ["rustc", "--edition=2024", "--crate-type=lib", "--emit=metadata",
                   "--cap-lints=allow", "--error-format=json", "--crate-name", name,
                   "--extern", "ove=" + str(library), "-L", "dependency=" + str(library.parent if library.parent.name == "deps" else library.parent / "deps"),
                   "-L", "dependency=" + str(Path(env["CARGO_TARGET_DIR"]) / "debug/deps"),
                   str(path), "-o", str(output / (name + ".rmeta"))]
        result = subprocess.run(command, env=env, capture_output=True, timeout=60, check=False)
        log = output / (name + ".log")
        log.write_bytes(result.stdout + result.stderr)
        errors = [json.loads(line) for line in result.stderr.splitlines() if line.startswith(b"{")]
        errors = [row for row in errors if row.get("level") == "error"]
        code = ("E0597" if name == "local_storage" else
                "E0133" if name == "raw_unsafe" and zero else
                "E0599" if name == "raw_unsafe" or (name == "heap_new" and zero)
                or (name == "deadline" and not asynchronous) else None)
        valid = result.returncode == int(code is not None)
        if code:
            valid &= any((row.get("code") or {}).get("code") == code
                         and any(s.get("file_name") == str(path) for s in row.get("spans", []))
                         for row in errors)
        rows.append({"name": name, "command": command, "status": result.returncode,
                     "valid": valid, "expected_code": code, "log": str(log),
                     "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest(),
                     "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                     "library": str(library), "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest()})
        print(name, "PASS" if valid else "FAIL", flush=True)
        if not valid:
            print(log.read_text(errors="backslashreplace"), end="")
    (output / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
    return all(row["valid"] for row in rows)
