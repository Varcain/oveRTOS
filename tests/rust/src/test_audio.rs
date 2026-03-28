// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

use crate::framework::run_suite;
use crate::test_entry;

fn test_graph_init_deinit() {
    let mut graph: ove::ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    ove::audio::graph_init(&mut graph, 256).unwrap();
    ove::audio::graph_deinit(&mut graph);
}

fn test_graph_init_zero_frames() {
    let mut graph: ove::ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    let result = ove::audio::graph_init(&mut graph, 0);
    assert!(result.is_err(), "graph_init with 0 frames should fail");
}

fn test_graph_build_empty() {
    let mut g: ove::ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    ove::audio::graph_init(&mut g, 256).unwrap();
    ove::audio::graph_build(&mut g).unwrap();
    ove::audio::graph_deinit(&mut g);
}

fn test_graph_connect_no_nodes() {
    let mut g: ove::ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    ove::audio::graph_init(&mut g, 256).unwrap();
    let result = ove::audio::graph_connect(&mut g, 0, 0);
    assert!(result.is_err(), "connect with no nodes should fail");
    ove::audio::graph_deinit(&mut g);
}

fn test_graph_start_not_ready() {
    let mut g: ove::ffi::ove_audio_graph = unsafe { core::mem::zeroed() };
    ove::audio::graph_init(&mut g, 256).unwrap();
    let result = ove::audio::graph_start(&mut g);
    assert!(result.is_err(), "start on IDLE graph should fail");
    ove::audio::graph_deinit(&mut g);
}

pub fn run() -> (usize, usize) {
    run_suite(
        "Audio",
        &[
            test_entry!(test_graph_init_deinit),
            test_entry!(test_graph_init_zero_frames),
            test_entry!(test_graph_build_empty),
            test_entry!(test_graph_connect_no_nodes),
            test_entry!(test_graph_start_not_ready),
        ],
    )
}
