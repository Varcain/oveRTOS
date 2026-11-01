// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;
use ove::{Error, audio, ffi, fs, nvs};
use std::fmt;
use std::sync::atomic::{AtomicUsize, Ordering};

unsafe extern "C" {
    fn ove_test_audio_layout(values: *mut usize);
}

fn test_paths_prefix_nul_and_native_bytes() {
    let root = std::env::temp_dir().join(format!("ove-native-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join("prefix");
    let path = path.to_str().unwrap();
    let backing = format!("{path}_suffix");
    let file = fs::File::open(
        &backing[..path.len()],
        fs::O_WRITE | fs::O_CREATE | fs::O_TRUNC,
    )
    .unwrap();
    file.write(b"A\0B").unwrap();
    drop(file);
    assert!(std::path::Path::new(path).exists());
    assert!(!std::path::Path::new(&backing).exists());
    let dest = format!("{path}-renamed");
    assert_eq!(
        fs::rename(path, format!("{dest}\0tail")),
        Err(Error::InvalidParam)
    );
    assert!(std::path::Path::new(path).exists());
    fs::rename(path, &dest).unwrap();
    assert!(std::path::Path::new(&dest).exists());
    let mut bytes = root.to_str().unwrap().as_bytes().to_vec();
    bytes.extend_from_slice(b"/name-\xff");
    let file = fs::File::open(&bytes, fs::O_CREATE | fs::O_WRITE).unwrap();
    drop(file);
    let mut directory = fs::Dir::open(root.to_str().unwrap()).unwrap();
    let mut found = false;
    while let Some(entry) = directory.read_entry().unwrap() {
        if entry.name() == b"name-\xff" {
            found = true;
        }
    }
    assert!(found);
    drop(directory);
    let file = fs::File::open(&bytes, fs::O_READ).unwrap();
    drop(file);
    fs::unlink(&bytes).unwrap();
    fs::unlink(dest).unwrap();
    fs::rmdir(root.to_str().unwrap()).unwrap();
    assert_eq!(fs::stat(vec![b'x'; 256]), Err(Error::NameTooLong));
    assert_ne!(fs::stat(vec![b'x'; 255]), Err(Error::NameTooLong));
    assert_eq!(
        fs::mount(Some("device"), Some("bad\0mount")),
        Err(Error::InvalidParam)
    );
    assert_eq!(fs::unmount(Some("bad\0mount")), Err(Error::InvalidParam));
    fs::mount(None, None).unwrap();
    fs::unmount(None).unwrap();
}

static FORMATS: AtomicUsize = AtomicUsize::new(0);
struct Once;
impl fmt::Display for Once {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        FORMATS.fetch_add(1, Ordering::Relaxed);
        out.write_str("native_fmt_key")
    }
}
struct Ignored;
impl fmt::Display for Ignored {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        let _ = out.write_str("valid");
        let _ = out.write_str("\0bad");
        let _ = out.write_str("later");
        Ok(())
    }
}
fn test_counted_nvs_and_single_pass_formatting() {
    nvs::init().unwrap();
    let key = "native_prefix_suffix";
    nvs::write(&key[..13], b"A\0B").unwrap();
    let mut data = [0; 3];
    assert_eq!(nvs::read("native_prefix", &mut data), Ok(3));
    assert_eq!(data, *b"A\0B");
    assert_eq!(nvs::read(key, &mut data), Err(Error::NotFound));
    let limit = nvs::key_max_len();
    assert_eq!(limit, 63);
    let key = "k".repeat(limit);
    nvs::write(&key, b"whole").unwrap();
    assert_eq!(
        nvs::write(&(key.clone() + "x"), b"bad"),
        Err(Error::InvalidParam)
    );
    assert_eq!(
        nvs::write("native_prefix\0suffix", b"bad"),
        Err(Error::InvalidParam)
    );
    assert_eq!(
        nvs::write_fmt(format_args!("{Ignored}"), b"bad"),
        Err(Error::InvalidParam)
    );
    assert_eq!(nvs::read("valid", &mut data), Err(Error::NotFound));
    FORMATS.store(0, Ordering::Relaxed);
    nvs::write_fmt(format_args!("{Once}"), b"once").unwrap();
    assert_eq!(FORMATS.load(Ordering::Relaxed), 1);
    nvs::write_fmt(format_args!("{}", "z".repeat(limit)), b"fit").unwrap();
    assert_eq!(
        nvs::write_fmt(format_args!("{}", "z".repeat(limit + 1)), b"bad"),
        Err(Error::InvalidParam)
    );
    nvs::erase("native_prefix").unwrap();
    nvs::erase(&key).unwrap();
    nvs::erase("native_fmt_key").unwrap();
    nvs::erase(&"z".repeat(limit)).unwrap();
}

fn test_audio_counted_spans_and_c_layout() {
    let mut native = [0; 6];
    unsafe { ove_test_audio_layout(native.as_mut_ptr()) };
    assert_eq!(
        native,
        [
            std::mem::size_of::<ffi::ove_audio_node>(),
            std::mem::align_of::<ffi::ove_audio_node>(),
            std::mem::offset_of!(ffi::ove_audio_node, name_span),
            std::mem::offset_of!(ffi::ove_audio_node, name_span.len),
            std::mem::size_of::<ffi::ove_audio_graph>(),
            std::mem::align_of::<ffi::ove_audio_graph>()
        ]
    );
    let mut graph: ffi::ove_audio_graph = unsafe { std::mem::zeroed() };
    audio::graph_init(&mut graph, 16).unwrap();
    static OPS: ffi::ove_audio_node_ops = unsafe { std::mem::zeroed() };
    static NAME: &str = concat!(
        "long-static-",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
    );
    let prefix = &NAME[..9];
    audio::graph_add_node(
        &mut graph,
        &OPS,
        std::ptr::null_mut(),
        prefix,
        ffi::OVE_AUDIO_NODE_SOURCE,
    )
    .unwrap();
    audio::graph_add_node(
        &mut graph,
        &OPS,
        std::ptr::null_mut(),
        NAME,
        ffi::OVE_AUDIO_NODE_SOURCE,
    )
    .unwrap();
    let mut moved = graph;
    assert_eq!(moved.nodes[0].name_span.len, prefix.len());
    assert_eq!(moved.nodes[1].name_span.len, NAME.len());
    let view = unsafe {
        std::slice::from_raw_parts(
            moved.nodes[1].name_span.data.cast::<u8>(),
            moved.nodes[1].name_span.len,
        )
    };
    assert_eq!(view, NAME.as_bytes());
    assert_eq!(
        audio::graph_add_node(
            &mut moved,
            &OPS,
            std::ptr::null_mut(),
            "nul\0suffix",
            ffi::OVE_AUDIO_NODE_SOURCE
        ),
        Err(Error::InvalidParam)
    );
    assert_eq!(moved.node_count, 2);
    audio::graph_add_node_cstr(
        &mut moved,
        &OPS,
        std::ptr::null_mut(),
        c"legacy",
        ffi::OVE_AUDIO_NODE_SOURCE,
    )
    .unwrap();
    assert_eq!(moved.nodes[2].name_span.len, 6);
    audio::graph_deinit(&mut moved);
    let mut graph = audio::Graph::new(16).unwrap();
    let cfg = audio::device_cfg_i2s(16000, 1, 1);
    assert_eq!(graph.device_source(&cfg, "source"), Ok(0));
    assert_eq!(graph.device_sink(&cfg, ""), Ok(1));
    assert_eq!(graph.build(), Err(Error::NotSupported));
}

pub fn run() -> (usize, usize) {
    run_suite(
        "NativeStrings",
        &[
            test_entry!(test_paths_prefix_nul_and_native_bytes),
            test_entry!(test_counted_nvs_and_single_pass_formatting),
            test_entry!(test_audio_counted_spans_and_c_layout),
        ],
    )
}
