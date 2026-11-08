# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe actual queue macro dispatch without native resources."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = """
use core::{marker::PhantomData, sync::atomic::{AtomicUsize, Ordering}};
static HEAP: AtomicUsize = AtomicUsize::new(0);
static TYPED: AtomicUsize = AtomicUsize::new(0);
static RAW: AtomicUsize = AtomicUsize::new(0);
pub mod ffi { #[allow(non_camel_case_types)] pub type ove_queue_storage_t = [u8; 1]; }
pub struct QueueStorage<T: Copy, const N: usize>(PhantomData<T>);
impl<T: Copy, const N: usize> QueueStorage<T,N> {
    pub const fn new() -> Self { Self(PhantomData) }
}
pub struct Queue<T: Copy, const N: usize>(PhantomData<T>);
impl<T: Copy, const N: usize> Queue<T,N> {
    pub fn new() -> Result<Self, &'static str> { HEAP.fetch_add(1, Ordering::Relaxed); Ok(Self(PhantomData)) }
    pub fn create(_: &'static QueueStorage<T,N>) -> Result<Self, &'static str> { TYPED.fetch_add(1, Ordering::Relaxed); Ok(Self(PhantomData)) }
    pub unsafe fn from_static(_: *mut ffi::ove_queue_storage_t, _: *mut core::ffi::c_void) -> Result<Self, &'static str> { RAW.fetch_add(1, Ordering::Relaxed); Ok(Self(PhantomData)) }
}
fn create() -> Queue<u32,2> { queue!(u32,2) }
fn main() {
    let _first = create();
    let _second = create();
    println!("queue macro: heap={} typed={} raw={}", HEAP.load(Ordering::Relaxed), TYPED.load(Ordering::Relaxed), RAW.load(Ordering::Relaxed));
}
"""


def verify(output, env, *, parent=False):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    macros = ROOT / "bindings/rust/ove/src/macros.rs"
    path = output / "dispatch.rs"
    path.write_text("#[path = " + json.dumps(str(macros)) + "] mod actual_macros;\n" + SOURCE)
    rows = []
    for zero in (False, True):
        name = "zero" if zero else "heap"
        binary = output / name
        command = ["rustc", "--edition=2024", "--cfg", "has_queue", str(path), "-o", str(binary)]
        if zero:
            command += ["--cfg", "zero_heap"]
        compiled = subprocess.run(command, env=env, capture_output=True, timeout=60, check=False)
        (output / (name + "-build.log")).write_bytes(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            print(compiled.stderr.decode(errors="backslashreplace"), end="")
            return False
        ran = subprocess.run([str(binary)], env=env, capture_output=True, timeout=15, check=False)
        text = (f"queue macro: heap={int(not zero) * 2} "
                f"typed={int(zero and not parent) * 2} raw={int(zero and parent) * 2}\n").encode()
        log = output / (name + ".log")
        log.write_bytes(ran.stdout + ran.stderr)
        valid = ran.returncode == 0 and ran.stdout == text and not ran.stderr
        rows.append({"zero": zero, "valid": valid, "build_command": command,
                     "build_status": compiled.returncode, "status": ran.returncode,
                     "source_sha256": hashlib.sha256(macros.read_bytes()).hexdigest(),
                     "elf_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                     "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        print(name, "macro dispatch", "PASS" if valid else "FAIL", flush=True)
    (output / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
    return all(row["valid"] for row in rows)
