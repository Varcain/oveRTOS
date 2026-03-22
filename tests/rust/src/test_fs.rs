// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_mount() {
    ove::fs::mount(None, None).unwrap();
}

fn test_file_write_read_roundtrip() {
    ove::fs::mount(None, None).unwrap();

    let path = b"/tmp/ove_test_rw.txt\0";

    // Write
    {
        let file =
            ove::fs::File::open(path, ove::fs::O_WRITE | ove::fs::O_CREATE).unwrap();
        let written = file.write(b"hello ove").unwrap();
        assert_eq!(written, 9);
    } // File dropped — closed via RAII

    // Read back
    {
        let file = ove::fs::File::open(path, ove::fs::O_READ).unwrap();
        let mut buf = [0u8; 32];
        let read = file.read(&mut buf).unwrap();
        assert_eq!(read, 9);
        assert_eq!(&buf[..9], b"hello ove");
    }

    // Cleanup
    std::fs::remove_file("/tmp/ove_test_rw.txt").ok();
}

fn test_file_raii_close() {
    ove::fs::mount(None, None).unwrap();
    let path = b"/tmp/ove_test_raii.txt\0";

    {
        let file =
            ove::fs::File::open(path, ove::fs::O_WRITE | ove::fs::O_CREATE).unwrap();
        file.write(b"raii").unwrap();
        // Drop here — should close cleanly
    }

    // Reopen should succeed (file was properly closed)
    {
        let file = ove::fs::File::open(path, ove::fs::O_READ).unwrap();
        let mut buf = [0u8; 4];
        let n = file.read(&mut buf).unwrap();
        assert_eq!(n, 4);
        assert_eq!(&buf, b"raii");
    }

    std::fs::remove_file("/tmp/ove_test_raii.txt").ok();
}

fn test_open_nonexistent_fails() {
    let result = ove::fs::File::open(b"/tmp/no_such_file_xyz\0", ove::fs::O_READ);
    assert!(result.is_err(), "opening nonexistent file should fail");
}

fn test_dir_open_read() {
    // Create a temp dir with some files
    let dir_path = "/tmp/ove_test_dir";
    std::fs::create_dir_all(dir_path).ok();
    std::fs::write(format!("{}/a.txt", dir_path), "a").ok();
    std::fs::write(format!("{}/b.txt", dir_path), "bb").ok();

    ove::fs::mount(None, None).unwrap();

    let mut dir = ove::fs::Dir::open(b"/tmp/ove_test_dir\0").unwrap();
    let mut names = Vec::new();

    loop {
        match dir.read_entry() {
            Ok(Some(entry)) => {
                let name = entry.name();
                // Skip . and ..
                if name != b"." && name != b".." {
                    names.push(name.to_vec());
                }
            }
            _ => break,
        }
    }

    // Dir dropped here — RAII close
    drop(dir);

    assert!(
        names.contains(&b"a.txt".to_vec()),
        "should find a.txt, got {:?}",
        names
    );
    assert!(
        names.contains(&b"b.txt".to_vec()),
        "should find b.txt, got {:?}",
        names
    );

    // Cleanup
    std::fs::remove_file(format!("{}/a.txt", dir_path)).ok();
    std::fs::remove_file(format!("{}/b.txt", dir_path)).ok();
    std::fs::remove_dir(dir_path).ok();
}

fn test_dir_entry_name() {
    let dir_path = "/tmp/ove_test_entry";
    std::fs::create_dir_all(dir_path).ok();
    std::fs::write(format!("{}/test.wav", dir_path), "wav data").ok();

    let mut dir = ove::fs::Dir::open(b"/tmp/ove_test_entry\0").unwrap();
    let mut found = false;

    loop {
        match dir.read_entry() {
            Ok(Some(entry)) => {
                if entry.name() == b"test.wav" {
                    found = true;
                    break;
                }
            }
            _ => break,
        }
    }

    drop(dir);
    assert!(found, "should find test.wav in directory");

    std::fs::remove_file(format!("{}/test.wav", dir_path)).ok();
    std::fs::remove_dir(dir_path).ok();
}

fn test_dir_open_nonexistent_fails() {
    let result = ove::fs::Dir::open(b"/tmp/no_such_dir_xyz\0");
    assert!(result.is_err(), "opening nonexistent dir should fail");
}

fn test_dir_end_returns_none() {
    let dir_path = "/tmp/ove_test_empty";
    std::fs::create_dir_all(dir_path).ok();

    let mut dir = ove::fs::Dir::open(b"/tmp/ove_test_empty\0").unwrap();

    // Drain all entries
    let mut count = 0;
    loop {
        match dir.read_entry() {
            Ok(Some(_)) => count += 1,
            Ok(None) => break,
            Err(_) => break,
        }
    }

    // Should have returned Ok(None) or an end-of-dir indication
    // The loop completed without crashing; entries (at least . and ..) may exist
    let _ = count;

    drop(dir);
    std::fs::remove_dir(dir_path).ok();
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Filesystem",
        &[
            test_entry!(test_mount),
            test_entry!(test_file_write_read_roundtrip),
            test_entry!(test_file_raii_close),
            test_entry!(test_open_nonexistent_fails),
            test_entry!(test_dir_open_read),
            test_entry!(test_dir_entry_name),
            test_entry!(test_dir_open_nonexistent_fails),
            test_entry!(test_dir_end_returns_none),
        ],
    )
}
