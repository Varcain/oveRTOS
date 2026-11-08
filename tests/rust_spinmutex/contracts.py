# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check scoped SpinMutex source contracts without executing guard/view escape."""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "config/ove-cli"))
from ove.test import _run_owned_test_process

CASES = {
    "option_take": (True, "pub fn probe() { let value = SpinMutex::new(Some(3u32)); let _: Option<u32> = value.with(Option::take); }", ()),
    "scalar_result": (True, "pub fn probe() -> Result<u32, u8> { SpinMutex::new(1u32).with(|_| Err(4)) }", ()),
    "const_owner": (True, "pub static OWNER: SpinMutex<[u32; 2]> = SpinMutex::new([1, 2]);", ()),
    "borrowed_owner": (True, "pub fn probe<'a>(value: &'a mut u32) { let owner = SpinMutex::new(value); owner.with(|slot| **slot += 1); let _: &'a mut u32 = owner.into_inner(); }", ()),
    "unsized_owner": (True, "pub fn probe(owner: &SpinMutex<[u8]>) { owner.with(|slice| slice.fill(0)); }", ()),
    "cell_send_sync": (True, "pub fn probe() { fn need<T: Send + Sync>() {} need::<SpinMutex<std::cell::Cell<u32>>>(); }", ()),
    "lock_removed": (False, "pub fn probe(owner: &SpinMutex<u32>) { let _guard = owner.lock(); }", ("E0599",)),
    "guard_removed": (False, "pub fn probe() { let _: Option<SpinMutexGuard<'static, u32>> = None; }", ("E0412", "E0425")),
    "guard_escape_removed": (False, "pub fn probe(a: &SpinMutex<u32>, b: &SpinMutex<u32>) { let _escape = a.with(|_| b.lock()); }", ("E0599",)),
    "early_drop_removed": (False, "pub fn probe(a: &SpinMutex<u32>, b: &SpinMutex<u32>) { let first = a.lock(); let second = b.lock(); drop(first); drop(second); }", ("E0599",)),
    "forgotten_guard_removed": (False, "pub fn probe(owner: &SpinMutex<u32>) { std::mem::forget(owner.lock()); }", ("E0599",)),
    "data_escape": (False, "pub fn probe(owner: &SpinMutex<u32>) -> &mut u32 { owner.with(|value| value) }", ("lifetime may not live long enough",)),
    "future_escape": (False, "pub fn probe(owner: &SpinMutex<u32>) -> impl core::future::Future<Output = ()> + '_ { owner.with(|value| async move { *value += 1; }) }", ("lifetime may not live long enough",)),
    "exclusive_owner_borrow": (False, "pub fn probe(mut owner: SpinMutex<u32>) { owner.with(|value| { let _ = owner.get_mut(); *value += 1; }); }", ("E0502", "E0499")),
    "move_owner_borrow": (False, "pub fn probe(owner: SpinMutex<u32>) { owner.with(|_| owner.into_inner()); }", ("E0505",)),
    "non_send_payload": (False, "pub fn probe() { fn need<T: Send + Sync>() {} need::<SpinMutex<std::rc::Rc<u32>>>(); }", ("E0277",)),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, default=ROOT / "bindings/rust/ove/src/sync.rs")
    parser.add_argument("--parent", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = args.source.read_text()
    family = original[original.index("#[cfg(has_async)]\npub use spin_mutex"):]
    prelude = '#![allow(non_camel_case_types)]\nmod bindings { pub type ove_irq_key_t = u64; unsafe extern "C" { pub fn ove_irq_lock() -> u64; pub fn ove_irq_unlock(key: u64); } }\n'
    rows = []
    cases = dict(CASES)
    # These probes are inserted inside the actual module solely to inspect
    # private lease traits; no native function or data dereference is run.
    for trait in ("Send", "Sync"):
        key_type = "SpinMutexGuard<'static, u32>" if args.parent else "Lease<'static>"
        inner = f"pub fn probe_private() {{ fn need<T: {trait}>() {{}} need::<{key_type}>(); }}"
        cases["private_not_" + trait.lower()] = (False, inner, ("E0277",))
    for name, (positive, code, diagnostics) in cases.items():
        for zero in (False, True):
            label = name + ("-zero" if zero else "-heap")
            source = out / (label + ".rs")
            body = family
            if name.startswith("private_not_"):
                end = body.rfind("}")
                body = body[:end] + code + body[end:]
                code_outside = "pub fn probe() { spin_mutex::probe_private(); }"
            else:
                code_outside = code
            source.write_text(prelude + body + "\n" + code_outside + "\n")
            output = source.with_suffix(".rmeta")
            command = ["rustc", "--edition=2024", "--crate-type=lib", "--emit=metadata", "-Dwarnings",
                       "--crate-name", "spin_contract", "--cfg", "has_async", "--check-cfg", "cfg(has_async)",
                       "--check-cfg", "cfg(zero_heap)", str(source), "-o", str(output)]
            if zero:
                command += ["--cfg", "zero_heap"]
            log = source.with_suffix(".log")
            with log.open("wb") as stream:
                status = _run_owned_test_process(command, env=dict(os.environ), timeout=45, stdout=stream)
            text = log.read_text(errors="backslashreplace")
            valid = (status == 0 and output.is_file()) if positive else (
                status == 1 and any(needle in text for needle in diagnostics)
            )
            if name == "guard_removed":
                valid = valid and "cannot find type `SpinMutexGuard`" in text
            rows.append({"case": label, "expected_compile": positive, "status": status, "valid": valid,
                         "source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                         "log": str(log), "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
            print(label, "PASS" if valid else "FAIL", flush=True)
            if not valid:
                print(text, end="")
            (out / "contracts.json").write_text(json.dumps(rows, indent=2) + "\n")
            if status not in (0, 1):
                return 1
    failed = sum(not row["valid"] for row in rows)
    print(f"spinmutex compile contracts: observations={len(rows)} failed={failed}")
    return int(failed != 0)


if __name__ == "__main__":
    raise SystemExit(main())
