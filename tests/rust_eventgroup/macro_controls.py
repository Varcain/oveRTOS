# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe actual constructor-macro dispatch through inert receivers."""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = """
use core::sync::atomic::{AtomicUsize,Ordering};
static HEAP:AtomicUsize=AtomicUsize::new(0);
static TYPED:AtomicUsize=AtomicUsize::new(0);
static RAW:AtomicUsize=AtomicUsize::new(0);
pub mod ffi {#[allow(non_camel_case_types)] pub type ove_eventgroup_storage_t=[u8;1];}
pub struct EventGroupStorage;
impl EventGroupStorage {pub const fn new()->Self{Self}}
pub struct EventGroup;
impl EventGroup {
 pub fn new()->Result<Self,&'static str>{HEAP.fetch_add(1,Ordering::Relaxed);Ok(Self)}
 pub fn create(_: &'static EventGroupStorage)->Result<Self,&'static str>{TYPED.fetch_add(1,Ordering::Relaxed);Ok(Self)}
 pub unsafe fn from_static(_: *mut ffi::ove_eventgroup_storage_t)->Result<Self,&'static str>{RAW.fetch_add(1,Ordering::Relaxed);Ok(Self)}
}
fn create()->EventGroup {eventgroup!()}
fn main(){let _first=create();let _second=create();println!("eventgroup macro: heap={} typed={} raw={}",HEAP.load(Ordering::Relaxed),TYPED.load(Ordering::Relaxed),RAW.load(Ordering::Relaxed));}
"""


def verify(output, env, *, macros=None):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    macros = Path(macros or ROOT / "bindings/rust/ove/src/macros.rs")
    path = output / "dispatch.rs"
    path.write_text("#[path = " + json.dumps(str(macros)) + "] mod actual_macros;\n" + SOURCE)
    rows = []
    for zero in (False, True):
        name = "zero" if zero else "heap"
        binary = output / name
        command = ["rustc", "--edition=2024", "--cfg", "has_eventgroup", str(path), "-o", str(binary)]
        if zero:
            command += ["--cfg", "zero_heap"]
        built = subprocess.run(command, env=env, capture_output=True, timeout=60, check=False)
        (output / (name + "-build.log")).write_bytes(built.stdout + built.stderr)
        if built.returncode:
            print(built.stderr.decode(errors="backslashreplace"), end="")
            return False
        image_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
        result = subprocess.run([str(binary)], env=env, capture_output=True, timeout=15, check=False)
        log = output / (name + ".log")
        log.write_bytes(result.stdout + result.stderr)
        valid = result.returncode == 0 and log.read_bytes() == f"eventgroup macro: heap={0 if zero else 2} typed={2 if zero else 0} raw=0\n".encode()
        rows.append({"zero": zero, "valid": valid, "build_command": command,
                     "build_status": built.returncode, "status": result.returncode,
                     "source_sha256": hashlib.sha256(macros.read_bytes()).hexdigest(),
                     "elf_sha256": image_hash, "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest()})
        print(name, "macro dispatch", "PASS" if valid else "FAIL", flush=True)
    (output / "observations.json").write_text(json.dumps(rows, indent=2) + "\n")
    return all(row["valid"] for row in rows)
