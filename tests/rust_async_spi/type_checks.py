# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check public async SPI contracts using the configured production dependency."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

root = Path(os.environ["OVE_DIR"])
work = Path(sys.argv[1]).resolve()
work.mkdir(parents=True, exist_ok=True)
consumer = (root / "tests/rust_async_spi/consumer.rs").read_text()
assert not re.search(r"\bunsafe\b|::ffi|\bCStr\b|\bPin\b", consumer)
cases={
 'copy':('fn f<T:Copy>(){} fn x(){f::<ove::AsyncSpi>();}',False,'Copy'),
 'clone':('fn f<T:Clone>(){} fn x(){f::<ove::AsyncSpi>();}',False,'Clone'),
 'send':('fn f<T:Send>(){} fn x(){f::<ove::AsyncSpi>();}',False,'Send'),
 'sync':('fn f<T:Sync>(){} fn x(){f::<ove::AsyncSpi>();}',False,'Sync'),
 'future_send':('fn f<T:Send>(_:T){} fn x(s:&mut ove::AsyncSpi){f(s.write(None,&[]));}',False,'Send'),
 'unsupported_word':('static S:ove::AsyncSpiStorage<8,u32>=ove::AsyncSpiStorage::new();',False,'Word'),
 'wrong_width':('fn x(s:&mut ove::AsyncSpi16){let mut b=[0u8;1];let _=s.read(None,&mut b);}',False,'mismatched types'),
 'wrong_trait':('fn f<T:embedded_hal_async::spi::SpiBus<u8>>(){} fn x(){f::<ove::AsyncSpi16>();}',False,'SpiBus'),
 'nonstatic_storage':('fn x(){let s=ove::AsyncSpiStorage::<8>::new();let _=s.init(ove::spi::Config::default());}',False,'does not live long enough'),
 'move_claimed_static':('static S:ove::AsyncSpiStorage<8>=ove::AsyncSpiStorage::new();fn x(){let _=S.init(ove::spi::Config::default());let moved=S;}',False,'cannot move out of static'),
 'two_live_operations':('fn x(s:&mut ove::AsyncSpi){let a=s.write(None,&[1]);let b=s.write(None,&[2]);drop(a);drop(b);}',False,'more than once'),
 'sync_conversion':('fn x(s:ove::Spi){let _:ove::AsyncSpi=s.into();}',False,'From'),
 'raw_cs':('fn x(s:&mut ove::AsyncSpi,c:&ove::ffi::ove_spi_cs){let _=s.write(Some(c),&[1]);}',False,'mismatched types'),
 'raw_handle_ctor':('fn x(){let _=ove::AsyncSpi::from_handle(core::ptr::null_mut());}',False,'from_handle'),
 'storage_sync':('fn f<T:Sync>(){}fn x(){f::<ove::AsyncSpiStorage<8>>();f::<ove::AsyncSpiStorage<8,u16>>();}',True,''),
 'typed_traits':('fn f<T:embedded_hal_async::spi::SpiBus<u8>>(){}fn g<T:embedded_hal_async::spi::SpiBus<u16>>(){}fn x(){f::<ove::AsyncSpi>();g::<ove::AsyncSpi16>();}',True,''),
 'future_unpin':('fn f<T:Unpin>(_:T){}fn x(s:&mut ove::AsyncSpi){f(s.write(None,&[1]));}',True,''),
 'public_consumer':(consumer,True,''),
}

zero = "#define CONFIG_OVE_ZERO_HEAP 1" in (Path(os.environ["OVE_GEN_DIR"]) / "ove_config.h").read_text()
deps = Path(os.environ["CARGO_TARGET_DIR"]) / "debug/deps"
def artifact(name):
    return str(max(deps.glob("lib" + name + "-*.rlib"), key=lambda path: path.stat().st_mtime_ns))

rows = []
selected = cases | {"heap_ctor": ('fn x(){let _=ove::AsyncSpi::new(ove::spi::Config::default());}', not zero, 'new' if zero else '')}
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
print(f"Async SPI external API/type contracts: {len(rows)} passed")
