// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Audio graph engine for oveRTOS.
//!
//! Provides safe wrappers around the C graph API: build a DAG of audio nodes
//! (sources, processors, sinks), validate formats, and execute in topological
//! order.

use crate::bindings;
use crate::error::{Error, Result};
use core::ffi::c_void;

/// Audio sample format.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SampleFmt {
    S16,
    S32,
    F32,
}

impl SampleFmt {
    fn to_raw(self) -> u32 {
        match self {
            SampleFmt::S16 => 0,
            SampleFmt::S32 => 1,
            SampleFmt::F32 => 2,
        }
    }
}

/// Audio format descriptor.
pub struct AudioFmt {
    pub sample_rate: u32,
    pub channels: u32,
    pub sample_fmt: SampleFmt,
}

/// Initialize the audio graph.
///
/// # Errors
/// Returns an error if `frames_per_period` is zero.
pub fn graph_init(
    graph: &mut bindings::ove_audio_graph,
    frames_per_period: u32,
) -> Result<()> {
    let rc = unsafe { bindings::ove_audio_graph_init(graph, frames_per_period) };
    Error::from_code(rc)
}

/// Tear down the graph and release all resources.
pub fn graph_deinit(graph: &mut bindings::ove_audio_graph) {
    unsafe { bindings::ove_audio_graph_deinit(graph) };
}

/// Add a node to the graph. Returns the node index.
///
/// # Errors
/// Returns an error if the graph is full or not in IDLE state.
pub fn graph_add_node(
    graph: &mut bindings::ove_audio_graph,
    ops: &bindings::ove_audio_node_ops,
    ctx: *mut c_void,
    name: &core::ffi::CStr,
    node_type: u32,
) -> core::result::Result<i32, Error> {
    let rc = unsafe {
        bindings::ove_audio_graph_add_node(graph, ops, ctx, name.as_ptr(), node_type)
    };
    if rc < 0 {
        Err(Error::from_code(rc).unwrap_err())
    } else {
        Ok(rc)
    }
}

/// Connect two nodes. `from` feeds into `to`.
///
/// # Errors
/// Returns an error on invalid indices or type violations.
pub fn graph_connect(
    graph: &mut bindings::ove_audio_graph,
    from: u32,
    to: u32,
) -> Result<()> {
    let rc = unsafe { bindings::ove_audio_graph_connect(graph, from, to) };
    Error::from_code(rc)
}

/// Validate formats, resolve execution order, allocate buffers.
///
/// # Errors
/// Returns an error on format mismatch, cycles, or OOM.
pub fn graph_build(graph: &mut bindings::ove_audio_graph) -> Result<()> {
    let rc = unsafe { bindings::ove_audio_graph_build(graph) };
    Error::from_code(rc)
}

/// Start the graph (sink-driven mode).
///
/// # Errors
/// Returns an error if graph is not in READY state.
pub fn graph_start(graph: &mut bindings::ove_audio_graph) -> Result<()> {
    let rc = unsafe { bindings::ove_audio_graph_start(graph) };
    Error::from_code(rc)
}

/// Stop the graph.
///
/// # Errors
/// Returns an error if graph is not in RUNNING state.
pub fn graph_stop(graph: &mut bindings::ove_audio_graph) -> Result<()> {
    let rc = unsafe { bindings::ove_audio_graph_stop(graph) };
    Error::from_code(rc)
}

/// Process one cycle (app-driven mode).
///
/// # Errors
/// Returns an error if graph is not built.
pub fn graph_process(graph: &mut bindings::ove_audio_graph) -> Result<()> {
    let rc = unsafe { bindings::ove_audio_graph_process(graph) };
    Error::from_code(rc)
}
