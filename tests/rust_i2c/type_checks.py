# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile external consumers against the actual configured ove dependency."""
import json
import os
import subprocess
import sys
from pathlib import Path

CASES = {
    "local_storage": ("let local = I2cStorage::new(); let _owner = local.init(Config::default());", "E0597"),
    "owner_copy": ("fn needs<T: Copy>() {} needs::<I2c>();", "E0277"),
    "owner_clone": ("fn needs<T: Clone>() {} needs::<I2c>();", "E0277"),
    "owner_send": ("fn needs<T: Send>() {} needs::<I2c>();", "E0277"),
    "owner_sync": ("fn needs<T: Sync>() {} needs::<I2c>();", "E0277"),
    "private_handle": ("fn f(bus: I2c) { let _ = bus.handle; }", "E0616"),
    "private_ownership": ("fn f(bus: I2c) { let _ = bus.ownership; }", "E0616"),
    "private_claim": ("let storage = I2cStorage::new(); let _ = storage.claimed;", "E0616"),
    "no_ten_bit_trait": ("fn f<T: embedded_hal::i2c::I2c<embedded_hal::i2c::TenBitAddress>>() {} f::<I2c>();", "E0277"),
    "overlapping_owner_borrows": ("fn f(mut bus: I2c) { let a=&mut bus; let b=&mut bus; let _=a.probe(0x42,core::time::Duration::ZERO); let _=b.probe(0x42,core::time::Duration::ZERO); }", "E0499"),
    "private_storage": ("let storage = I2cStorage::new(); let _ = storage.raw;", "E0616"),
    "raw_requires_unsafe": ("let _ = I2c::from_handle(core::ptr::null_mut());", "E0133"),
    "moved_owner": ("fn f(bus: I2c) { drop(bus); drop(bus); }", "E0382"),
}
for name, args in {
    "write": "0x42, &[1], d", "read": "0x42, &mut [0], d",
    "write_read": "0x42, &[1], &mut [0], d",
    "reg_write": "0x42, 1, &[1], d", "reg_read": "0x42, 1, &mut [0], d",
    "probe": "0x42, d",
}.items():
    CASES["shared_" + name] = (
        "fn f(bus: &I2c) { let d=core::time::Duration::ZERO; let _=bus."
        + name + "(" + args + "); }", "E0596")

PASSING = """
static S: I2cStorage = I2cStorage::new();
let mut bus: I2c = S.init(Config::default()).unwrap();
let tx = [1u8, 2, 3];
let mut rx = [0u8; 7];
bus.write_read(0x42, &tx, &mut rx, core::time::Duration::ZERO).unwrap();
embedded_hal::i2c::I2c::write_read(&mut bus, 0x42, &tx, &mut rx).unwrap();
drop(bus);
let again = S.init(Config::default()).unwrap();
drop(again);
"""


def main():
    root = Path(os.environ["OVE_DIR"])
    work = Path(sys.argv[1]).resolve()
    (work / "src").mkdir(parents=True, exist_ok=True)
    manifest = {
        "name": "ove-i2c-type-checks", "version": "0.0.0", "edition": "2024",
    }
    (work / "Cargo.toml").write_text(
        "[package]\n" + "".join(f"{key} = {json.dumps(value)}\n" for key, value in manifest.items())
        + "[workspace]\n[dependencies]\nove = { path = "
        + json.dumps(str(root / "bindings/rust/ove"))
        + ', default-features = false, features = ["std", "embedded-hal"] }\nembedded-hal = "1.0"\n'
    )
    lock = work / "Cargo.lock"
    if not lock.exists():
        lock.write_bytes((root / "tests/rust_i2c/Cargo.lock").read_bytes())
    prefix = "use ove::{I2c, I2cStorage}; use ove::i2c::Config;\n"
    config = (Path(os.environ["OVE_GEN_DIR"]) / "ove_config.h").read_text()
    zero_heap = "#define CONFIG_OVE_ZERO_HEAP 1" in config
    cases = {"passing": (PASSING, None), **CASES}
    cases["heap_constructor"] = (
        "let _ = I2c::new(Config::default());", "E0599" if zero_heap else None)
    outcomes = []
    for name, (body, expected) in cases.items():
        (work / "src/main.rs").write_text(prefix + "fn main() {\n" + body + "\n}\n")
        result = subprocess.run(
            ["cargo", "check", "--offline", "--manifest-path", str(work / "Cargo.toml")],
            capture_output=True, text=True, timeout=120, check=False,
        )
        (work / f"{name}.log").write_text(result.stdout + result.stderr)
        if expected is None:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0 and f"error[{expected}]" in result.stderr, (name, result.stderr)
        outcomes.append({"case": name, "expected_diagnostic": expected, "passed": True})
    (work / "results.json").write_text(json.dumps(outcomes, indent=2) + "\n")
    print(f"I2C external type contracts: {len(outcomes)} passed")


if __name__ == "__main__":
    main()
