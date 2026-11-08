# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile actual Queue lifetime, payload and storage controls without execution."""
import hashlib
import json
import subprocess
from pathlib import Path

CASES = {
    "shortened_view": """use ove::Queue;
pub fn insert<'a>(q: &Queue<&'static u32, 1>, value: &'a u32) {
    let view: &Queue<&'a u32, 1> = q;
    let _ = view.try_send(&value);
}
""",
    "owner_traits": """use core::{cell::Cell, marker::PhantomData};
use ove::Queue;
#[derive(Clone, Copy)] pub struct SendNotSync(PhantomData<Cell<()>>);
pub fn traits<T: Send + Sync>() {}
pub fn send<T: Send>() {}
pub fn control() { send::<SendNotSync>(); traits::<Queue<SendNotSync, 1>>(); }
""",
    "payload_not_sync": """use core::{cell::Cell, marker::PhantomData};
#[derive(Clone, Copy)] pub struct SendNotSync(PhantomData<Cell<()>>);
pub fn sync<T: Sync>() {}
pub fn control() { sync::<SendNotSync>(); }
""",
    "owner_not_send": """pub fn send<T: Send>() {}
pub fn control() { send::<ove::Queue<*mut u8, 1>>(); }
""",
    "owner_not_sync": """pub fn sync<T: Sync>() {}
pub fn control() { sync::<ove::Queue<*mut u8, 1>>(); }
""",
    "shared_storage": """static STORAGE: ove::QueueStorage<*mut u8, 1> = ove::QueueStorage::new();
pub fn sync<T: Sync>() {}
pub fn control() { sync::<ove::QueueStorage<*mut u8, 1>>(); let _ = &STORAGE; }
pub fn local() -> ove::Result<()> {
    let mut value = 1u8;
    let q = ove::Queue::create(&STORAGE)?;
    q.try_send(&(&mut value as *mut u8))?;
    let _ = q.try_recv()?;
    Ok(())
}
""",
    "local_storage": """pub fn control() -> ove::Result<ove::Queue<u32, 1>> {
    let storage = ove::QueueStorage::new();
    ove::Queue::create(&storage)
}
""",
    "borrowed_heap": """pub fn control<'a>(value: &'a u32) -> ove::Result<()> {
    let q = ove::Queue::<&'a u32, 1>::new()?;
    q.try_send(&value)?;
    let _ = q.try_recv()?;
    Ok(())
}
""",
    "borrowed_escape": """pub fn control<'a>(value: &'a u32) -> ove::Queue<&'static u32, 1> {
    let q = ove::Queue::<&'a u32, 1>::new().unwrap();
    let _ = q.try_send(&value);
    q
}
""",
    "async_traits": """use core::{cell::Cell, marker::PhantomData};
#[derive(Clone, Copy)] pub struct SendNotSync(PhantomData<Cell<()>>);
pub fn traits<T: Send + Sync>() {}
pub fn control() { traits::<ove::AsyncQueue<SendNotSync, 1>>(); }
""",
    "async_not_send": """pub fn send<T: Send>() {}
pub fn control() { send::<ove::AsyncQueue<*mut u8, 1>>(); }
""",
    "async_not_sync": """pub fn sync<T: Sync>() {}
pub fn control() { sync::<ove::AsyncQueue<*mut u8, 1>>(); }
""",
    "relative_time": """use ove::Queue;
pub fn control(q: &Queue<u32, 1>) {
    let _ = q.try_send_for(&1, core::time::Duration::ZERO);
    let _ = q.try_recv_for(core::time::Duration::ZERO);
}
""",
}


def verify(library, output, env, *, zero=False, asynchronous=False, parent=False):
    """Check compiler statuses and intended diagnostics against the actual library."""
    library, output = Path(library), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for name, source in CASES.items():
        if zero and name in ("borrowed_heap", "borrowed_escape"):
            continue
        if name.startswith("async_") and not asynchronous:
            continue
        path = output / (name + ".rs")
        path.write_text(source)
        command = ["rustc", "--edition=2024", "--crate-type=lib", "--emit=metadata",
                   "--cap-lints=allow", "--error-format=json", "--crate-name", name,
                   "--extern", "ove=" + str(library), "-L", "dependency=" + str(library.parent if library.parent.name == "deps"
                                            else library.parent / "deps"),
                   str(path), "-o", str(output / (name + ".rmeta"))]
        # An explicit target keeps host procedural macros in a separate tree,
        # even when that target is the host architecture.
        profile_dir = library.parent.parent if library.parent.name == "deps" else library.parent
        host_deps = Path(env["CARGO_TARGET_DIR"]) / profile_dir.name / "deps"
        command += ["-L", "dependency=" + str(host_deps)]
        p = subprocess.run(command, env=env, capture_output=True,
                           timeout=60, check=False)
        (output / (name + ".stdout")).write_bytes(p.stdout)
        (output / (name + ".stderr")).write_bytes(p.stderr)
        diagnostics = [json.loads(line) for line in p.stderr.splitlines() if line.startswith(b"{")]
        errors = [row for row in diagnostics if row.get("level") == "error"]
        reject = (name in ("payload_not_sync", "owner_not_send", "owner_not_sync",
                           "local_storage", "borrowed_escape", "async_not_send", "async_not_sync")
                  or (name == "shortened_view" and not parent))
        valid = p.returncode == int(reject)
        if reject:
            if name in ("shortened_view", "borrowed_escape"):
                valid = valid and any("lifetime" in row["message"]
                                      or (row.get("code") or {}).get("code") == "E0521"
                                      for row in errors)
                valid = valid and any(span.get("file_name") == str(path)
                                      for row in errors for span in row.get("spans", []))
                if name == "shortened_view":
                    valid = valid and any(span.get("is_primary") and span.get("line_start") == 3
                                          for row in errors for span in row.get("spans", []))
            else:
                code = "E0597" if name == "local_storage" else "E0277"
                codes = [(row.get("code") or {}).get("code") for row in errors]
                valid = valid and code in codes and all(x in (None, code) for x in codes)
        rows.append({"name": name, "command": command, "status": p.returncode,
                     "expected_rejection": reject, "valid": valid,
                     "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                     "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
                     "diagnostics": errors})
        (output / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(name, "PASS" if valid else "FAIL", flush=True)
        if not valid:
            print(p.stderr.decode(errors="backslashreplace"), end="")
    return all(row["valid"] for row in rows)
