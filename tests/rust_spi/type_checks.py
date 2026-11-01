# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile external consumers against the actual configured ove dependency."""
import json
import os
import subprocess
import sys
from pathlib import Path

CASES = {
    "local_storage": ("let local = SpiStorage::<u8>::new(); let _owner = local.init(Config::default());", "E0597"),
    "owner_copy": ("fn needs<T: Copy>() {} needs::<Spi>();", "E0277"),
    "owner_clone": ("fn needs<T: Clone>() {} needs::<Spi>();", "E0277"),
    "owner_send": ("fn needs<T: Send>() {} needs::<Spi>();", "E0277"),
    "owner_sync": ("fn needs<T: Sync>() {} needs::<Spi16>();", "E0277"),
    "descriptor_copy": ("fn needs<T: Copy>() {} needs::<Transfer<'static>>();", "E0277"),
    "descriptor_clone": ("fn needs<T: Clone>() {} needs::<Transfer<'static, u16>>();", "E0277"),
    "tx_borrow": ("let mut tx = [1u8]; let entry = Transfer::write(&tx); tx[0] = 2; consume(entry);", "E0506"),
    "rx_borrow": ("let mut rx = [0u8]; let entry = Transfer::read(&mut rx); rx[0] = 2; consume(entry);", "E0506"),
    "rx_drop": ("let mut rx = vec![0u8]; let entry = Transfer::read(&mut rx); drop(rx); consume(entry);", "E0505"),
    "tx_drop": ("let tx = vec![0u8]; let entry = Transfer::write(&tx); drop(tx); consume(entry);", "E0505"),
    "overlapping_rx": ("let mut rx = [0u8; 2]; let a = Transfer::read(&mut rx); let b = Transfer::read(&mut rx); consume(a); consume(b);", "E0499"),
    "in_place_borrow": ("let mut rx = [0u16]; let entry = Transfer::in_place(&mut rx); rx[0] = 2; consume(entry);", "E0506"),
    "private_records": ("let mut entry = Transfer::write(&[1u8]); entry.raw[0].len = 9;", "E0616"),
    "raw_record_injection": ("fn inject(mut spi: Spi, tx: &[u8]) { let raw = ove::ffi::ove_spi_xfer { tx: tx.as_ptr().cast(), rx: core::ptr::null_mut(), len: tx.len() + 1 }; spi.transfer_seq(None, &mut [raw], std::time::Duration::ZERO).unwrap(); }", "E0308"),
    "private_storage": ("let storage = SpiStorage::<u8>::new(); let _ = storage.raw;", "E0616"),
    "raw_requires_unsafe": ("let _ = Spi::from_handle(core::ptr::null_mut());", "E0133"),
    "wrong_word": ("static S: SpiStorage<u16> = SpiStorage::new(); let mut s = S.init(Config::default()).unwrap(); let mut entries = [Transfer::write(&[1u8])]; s.transfer_seq(None, &mut entries, std::time::Duration::ZERO).unwrap();", "E0308"),
    "unsupported_word": ("let _ = SpiStorage::<u32>::new();", "E0277"),
    "word_is_sealed": ("#[derive(Copy, Clone)] struct Custom; impl ove::spi::Word for Custom { const BITS: u8 = 8; }", "E0277"),
}

PASSING = """
static S: SpiStorage = SpiStorage::new();
static S16: SpiStorage<u16> = SpiStorage::new();
let mut spi: Spi = S.init(Config::default()).unwrap();
let mut words: Spi16 = S16.init(Config::default()).unwrap();
let tx = [1u8, 2];
let mut rx = [0u8; 4];
let (left, right) = rx.split_at_mut(2);
let mut entries = [Transfer::duplex(&tx, left), Transfer::duplex(&tx, right)];
spi.transfer_seq(None, &mut entries, std::time::Duration::ZERO).unwrap();
spi.transfer_seq(None, &mut entries, std::time::Duration::ZERO).unwrap();
drop(entries);
rx[0] = 3;
words.transfer_in_place(None, &mut [0u16; 2], std::time::Duration::ZERO).unwrap();
"""


def main():
    root = Path(os.environ["OVE_DIR"])
    work = Path(sys.argv[1]).resolve()
    (work / "src").mkdir(parents=True, exist_ok=True)
    manifest = {
        "name": "ove-spi-type-checks", "version": "0.0.0", "edition": "2024",
    }
    (work / "Cargo.toml").write_text(
        "[package]\n" + "".join(f"{key} = {json.dumps(value)}\n" for key, value in manifest.items())
        + "[workspace]\n[dependencies]\nove = { path = "
        + json.dumps(str(root / "bindings/rust/ove"))
        + ', default-features = false, features = ["std", "embedded-hal"] }\n'
    )
    lock = work / "Cargo.lock"
    if not lock.exists():
        lock.write_bytes((root / "tests/rust_spi/Cargo.lock").read_bytes())
    prefix = "use ove::{Spi, Spi16, SpiStorage}; use ove::spi::{Config, Transfer};\nfn consume<W: ove::spi::Word>(entry: Transfer<'_, W>) { std::hint::black_box(entry); }\n"
    outcomes = []
    for name, (body, expected) in {"passing": (PASSING, None), **CASES}.items():
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
    print(f"SPI external type contracts: {len(outcomes)} passed")


if __name__ == "__main__":
    main()
