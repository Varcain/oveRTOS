# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check public async I2C contracts using the configured production dependency."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

root = Path(os.environ["OVE_DIR"])
work = Path(sys.argv[1]).resolve()
work.mkdir(parents=True, exist_ok=True)
consumer = (root / "tests/rust_async_i2c/consumer.rs").read_text()
assert not re.search(r"\bunsafe\b|::ffi|\bCStr\b|\bPin\b", consumer)
cases = {
    "copy": ("fn f<T:Copy>(){} fn x(){f::<ove::AsyncI2c>();}", False, "Copy"),
    "clone": ("fn f<T:Clone>(){} fn x(){f::<ove::AsyncI2c>();}", False, "Clone"),
    "send": ("fn f<T:Send>(){} fn x(){f::<ove::AsyncI2c>();}", False, "Send"),
    "sync": ("fn f<T:Sync>(){} fn x(){f::<ove::AsyncI2c>();}", False, "Sync"),
    "future_send": ("fn f<T:Send>(_:T){} fn x(s:&mut ove::AsyncI2c){f(s.write(0x42,&[1]));}", False, "Send"),
    "flush_send": ("fn f<T:Send>(_:T){} fn x(s:&mut ove::AsyncI2c){f(s.flush());}", False, "Send"),
    "ten_bit_trait": ("fn f<T:embedded_hal_async::i2c::I2c<embedded_hal_async::i2c::TenBitAddress>>(){} fn x(){f::<ove::AsyncI2c>();}", False, "I2c<u16>"),
    "nonstatic_storage": ("fn x(){let s=ove::AsyncI2cStorage::<3,7>::new();let _=s.init(ove::i2c::Config::default());}", False, "does not live long enough"),
    "private_storage": ("fn x(s:&ove::AsyncI2cStorage<8>){let _=&s.header;}", False, "private"),
    "move_claimed_static": ("static S:ove::AsyncI2cStorage<8>=ove::AsyncI2cStorage::new();fn x(){let _=S.init(ove::i2c::Config::default());let moved=S;}", False, "cannot move out of static"),
    "two_live_operations": ("fn x(s:&mut ove::AsyncI2c){let a=s.write(0x42,&[1]);let b=s.write(0x42,&[2]);drop(a);drop(b);}", False, "more than once"),
    "sync_conversion": ("fn x(s:ove::I2c){let _:ove::AsyncI2c=s.into();}", False, "From"),
    "raw_handle_ctor": ("fn x(){let _=ove::AsyncI2c::from_handle(core::ptr::null_mut());}", False, "from_handle"),
    "storage_sync": ("fn f<T:Sync>(){}fn x(){f::<ove::AsyncI2cStorage<3,7>>();f::<ove::AsyncI2cStorage<0,7>>();f::<ove::AsyncI2cStorage<3,0>>();}", True, ""),
    "seven_bit_trait": ("fn f<T:embedded_hal_async::i2c::I2c>(){}fn x(){f::<ove::AsyncI2c>();}", True, ""),
    "future_unpin": ("fn f<T:Unpin>(_:T){}fn x(s:&mut ove::AsyncI2c){f(s.write(0x42,&[1]));f(s.flush());}", True, ""),
    "public_consumer": (consumer, True, ""),
}
for method, arguments in [("write", "0x42,&[1]"), ("read", "0x42,&mut [0]"), ("write_read", "0x42,&[1],&mut [0]"), ("flush", "")]:
    cases["shared_" + method] = ("fn x(s:&ove::AsyncI2c){let _=s." + method + "(" + arguments + ");}", False, "mutable")

zero = "#define CONFIG_OVE_ZERO_HEAP 1" in (Path(os.environ["OVE_GEN_DIR"]) / "ove_config.h").read_text()
deps = Path(os.environ["CARGO_TARGET_DIR"]) / "debug/deps"
def artifact(name):
    return str(max(deps.glob("lib" + name + "-*.rlib"), key=lambda path: path.stat().st_mtime_ns))

rows = []
selected = cases | {"heap_ctor": ('fn x(){let _=ove::AsyncI2c::new(ove::i2c::Config::default());}', not zero, 'new' if zero else '')}
for label, (source, accepted, diagnostic) in selected.items():
    file = work / (label + ".rs")
    file.write_text("#![allow(dead_code,unused_variables)]\n" + source + "\n")
    command = ["rustc", "--edition", "2024", "--crate-type", "lib", "--crate-name", "contract_" + label,
               "--emit", "metadata", "-L", "dependency=" + str(deps),
               "--extern", "ove=" + artifact("ove"), "--extern", "embedded_hal_async=" + artifact("embedded_hal_async"),
               str(file), "-o", str(work / (label + ".rmeta"))]
    if zero:
        command += ["--cfg", "zero_heap"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=30, check=False)
    log = result.stdout + result.stderr
    (work / (label + ".log")).write_text(log)
    assert (result.returncode == 0) == accepted and (accepted or diagnostic in log), (label, log)
    rows.append({"case": label, "zero_heap": zero, "accepted": accepted, "passed": True})
(work / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
print(f"Async I2C external API/type contracts: {len(rows)} passed")
