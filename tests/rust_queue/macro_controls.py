# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe actual queue macro dispatch without native resources."""
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


def verify(output, env):
    """Run the actual queue macro against inert receivers; return one result per heap mode."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    path = output / "dispatch.rs"
    path.write_text("#[path = " + json.dumps(str(ROOT / "bindings/rust/ove/src/macros.rs")) + "] mod actual_macros;\n"
                    + SOURCE)
    results = []
    for zero in (False, True):
        binary = output / ("zero" if zero else "heap")
        command = ["rustc", "--edition=2024", "--cfg", "has_queue", str(path), "-o", str(binary)]
        if zero:
            command += ["--cfg", "zero_heap"]
        subprocess.run(command, env=env, timeout=60, check=True)
        ran = subprocess.run([str(binary)], env=env, capture_output=True, timeout=15, check=False)
        text = f"queue macro: heap={int(not zero) * 2} typed={int(zero) * 2} raw=0\n".encode()
        results.append(ran.returncode == 0 and ran.stdout == text and not ran.stderr)
        print(binary.name, "macro dispatch", "PASS" if results[-1] else "FAIL", flush=True)
        if not results[-1]:
            print((ran.stdout + ran.stderr).decode(errors="backslashreplace"), end="")
    return results
