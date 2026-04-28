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
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleFmt {
    S16,
    S32,
    F32,
}

impl SampleFmt {
    /// Convert to the raw C enum value (`ove_audio_sample_fmt`).
    pub fn to_raw(self) -> u32 {
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
pub fn graph_init(graph: &mut bindings::ove_audio_graph, frames_per_period: u32) -> Result<()> {
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
    node_type: bindings::ove_audio_node_type,
) -> core::result::Result<i32, Error> {
    let rc =
        unsafe { bindings::ove_audio_graph_add_node(graph, ops, ctx, name.as_ptr(), node_type) };
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
pub fn graph_connect(graph: &mut bindings::ove_audio_graph, from: u32, to: u32) -> Result<()> {
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

// ---------------------------------------------------------------------------
// Audio device configuration builder
// ---------------------------------------------------------------------------

/// Build an `ove_audio_device_cfg` for I2S transport.
///
/// Constructs the C struct safely, including the transport-specific union
/// fields, without requiring `unsafe` in application code.
pub fn device_cfg_i2s(
    sample_rate: u32,
    channels: u32,
    input_device: u32,
) -> bindings::ove_audio_device_cfg {
    let mut cfg: bindings::ove_audio_device_cfg = unsafe { core::mem::zeroed() };
    cfg.transport = bindings::OVE_AUDIO_TRANSPORT_I2S;
    cfg.fmt.sample_rate = sample_rate;
    cfg.fmt.channels = channels;
    cfg.fmt.sample_fmt = bindings::OVE_AUDIO_FMT_S16;
    // Write i2s.input_device into the union via raw pointer
    unsafe {
        let union_ptr = &mut cfg as *mut _ as *mut u8;
        let i2s_offset = core::mem::offset_of!(bindings::ove_audio_device_cfg, __bindgen_anon_1);
        let i2s_ptr = union_ptr.add(i2s_offset) as *mut u32;
        *i2s_ptr = input_device;
    }
    cfg
}

// ---------------------------------------------------------------------------
// Safe Graph wrapper
// ---------------------------------------------------------------------------

/// Owned audio graph session.
///
/// Wraps the C `ove_audio_graph` in a safe API.  All `unsafe` FFI calls
/// are encapsulated — application code uses only safe methods.
///
/// # Example
///
/// ```ignore
/// let mut g = Graph::new(512)?;
/// let dev = device_cfg_i2s(16000, 1, 1);
/// let src  = g.device_source(&dev, b"mic\0")?;
/// let proc = g.add_processor(PROC.get_mut(), b"dsp\0")?;
/// let sink = g.device_sink(&dev, b"spk\0")?;
/// g.connect(src, proc)?;
/// g.connect(proc, sink)?;
/// g.build()?;
/// g.start()?;
/// ```
pub struct Graph {
    inner: bindings::ove_audio_graph,
}

impl Graph {
    /// Create and initialize a new audio graph.
    pub fn new(frames_per_period: u32) -> Result<Self> {
        let mut inner: bindings::ove_audio_graph = unsafe { core::mem::zeroed() };
        let rc = unsafe { bindings::ove_audio_graph_init(&mut inner, frames_per_period) };
        Error::from_code(rc)?;
        Ok(Self { inner })
    }

    /// Create and initialize a graph, attaching caller-owned buffer storage.
    /// Required for `CONFIG_OVE_ZERO_HEAP` builds where `build()` cannot
    /// `calloc` inter-node buffers.  In heap-mode builds the storage is
    /// unused but harmless.  Prefer the [`crate::audio_graph!`] macro,
    /// which emits the backing array automatically.
    pub fn new_with_storage(frames_per_period: u32, storage: &'static mut [u8]) -> Result<Self> {
        let mut inner: bindings::ove_audio_graph = unsafe { core::mem::zeroed() };
        let rc = unsafe { bindings::ove_audio_graph_init(&mut inner, frames_per_period) };
        Error::from_code(rc)?;
        let rc = unsafe {
            bindings::ove_audio_graph_set_buf_storage(
                &mut inner,
                storage.as_mut_ptr() as *mut _,
                storage.len(),
            )
        };
        Error::from_code(rc)?;
        Ok(Self { inner })
    }

    /// Add a hardware audio source node.  Returns the node index.
    pub fn device_source(
        &mut self,
        cfg: &bindings::ove_audio_device_cfg,
        name: &[u8],
    ) -> core::result::Result<u32, Error> {
        let rc = unsafe {
            bindings::ove_audio_device_source(&mut self.inner, cfg, name.as_ptr() as *const _)
        };
        if rc < 0 {
            Err(Error::from_code(rc).unwrap_err())
        } else {
            Ok(rc as u32)
        }
    }

    /// Add a hardware audio sink node.  Returns the node index.
    pub fn device_sink(
        &mut self,
        cfg: &bindings::ove_audio_device_cfg,
        name: &[u8],
    ) -> core::result::Result<u32, Error> {
        let rc = unsafe {
            bindings::ove_audio_device_sink(&mut self.inner, cfg, name.as_ptr() as *const _)
        };
        if rc < 0 {
            Err(Error::from_code(rc).unwrap_err())
        } else {
            Ok(rc as u32)
        }
    }

    /// Register a custom processor node.  Returns the node index.
    pub fn add_processor<T: AudioProcessor>(
        &mut self,
        processor: &'static mut T,
        name: &[u8],
    ) -> core::result::Result<u32, Error> {
        graph_add_processor(&mut self.inner, processor, name).map(|i| i as u32)
    }

    /// Connect two nodes.  `from` feeds into `to`.
    pub fn connect(&mut self, from: u32, to: u32) -> Result<()> {
        graph_connect(&mut self.inner, from, to)
    }

    /// Validate formats, resolve execution order, allocate buffers.
    pub fn build(&mut self) -> Result<()> {
        graph_build(&mut self.inner)
    }

    /// Start the graph (sink-driven mode).
    pub fn start(&mut self) -> Result<()> {
        graph_start(&mut self.inner)
    }

    /// Stop the graph.
    pub fn stop(&mut self) -> Result<()> {
        graph_stop(&mut self.inner)
    }

    /// Process one cycle (app-driven mode).
    pub fn process(&mut self) -> Result<()> {
        graph_process(&mut self.inner)
    }
}

impl Drop for Graph {
    fn drop(&mut self) {
        graph_deinit(&mut self.inner);
    }
}

// SAFETY: the C-side graph object is safe to hand off between threads once
// built — audio-thread callbacks only read immutable node state, and control
// APIs (`connect`, `start`, `stop`) are serialised by the caller.  Required
// so `Graph` can live inside `StaticCell` / `StaticMut` for FreeRTOS main-stack
// survival (see the hiroic apps' `GRAPH` static).
unsafe impl Send for Graph {}
unsafe impl Sync for Graph {}

// ---------------------------------------------------------------------------
// Safe audio processor trait
// ---------------------------------------------------------------------------

/// Audio buffer descriptor (borrowed from the C layer).
pub struct AudioBuf {
    raw: *const bindings::ove_audio_buf,
}

impl AudioBuf {
    /// Get a slice of interleaved S16 samples.
    pub fn data_s16(&self) -> &[i16] {
        unsafe {
            let buf = &*self.raw;
            let count = buf.frames as usize * (*buf.fmt).channels as usize;
            core::slice::from_raw_parts(buf.data as *const i16, count)
        }
    }

    /// Get a mutable slice of interleaved S16 samples.
    ///
    /// `&self` is intentional: the underlying buffer is C-owned and the
    /// AudioProcessor trait passes the output buf as `&AudioBuf` (so a
    /// processor can read in / write out through one reference each).
    /// Aliasing safety is the C side's responsibility.
    #[allow(clippy::mut_from_ref)]
    pub fn data_s16_mut(&self) -> &mut [i16] {
        unsafe {
            let buf = &*self.raw;
            let count = buf.frames as usize * (*buf.fmt).channels as usize;
            core::slice::from_raw_parts_mut(buf.data as *mut i16, count)
        }
    }

    /// Number of frames in this buffer.
    pub fn frames(&self) -> u32 {
        unsafe { (*self.raw).frames }
    }

    /// Number of interleaved channels in this buffer.
    pub fn channels(&self) -> u32 {
        unsafe { (*(*self.raw).fmt).channels }
    }
}

/// Trait for implementing custom audio processing nodes.
///
/// All FFI bridging is handled by the binding layer — implement this
/// trait on a plain Rust struct with no `unsafe` or `extern "C"`.
///
/// # Example
/// ```ignore
/// struct MyProcessor { ... }
/// impl ove::audio::AudioProcessor for MyProcessor {
///     fn process(&mut self, input: &AudioBuf, output: &AudioBuf) {
///         let src = input.data_s16();
///         let dst = output.data_s16_mut();
///         dst.copy_from_slice(src); // passthrough
///     }
/// }
/// ```
pub trait AudioProcessor {
    /// Process one audio period. Called from the audio thread.
    fn process(&mut self, input: &AudioBuf, output: &AudioBuf);
}

/// Register a custom processor node on the graph.
///
/// The processor must live in a `&'static mut` location (e.g. a
/// `StaticCell` or `static mut`).  All FFI trampolines are generated
/// internally — no `unsafe` or `extern "C"` needed in application code.
///
/// # Errors
/// Returns an error if the graph is full or not in IDLE state.
pub fn graph_add_processor<T: AudioProcessor>(
    graph: &mut bindings::ove_audio_graph,
    processor: &'static mut T,
    name: &[u8],
) -> core::result::Result<i32, Error> {
    unsafe extern "C" fn configure_trampoline(
        _ctx: *mut c_void,
        in_fmt: *const bindings::ove_audio_fmt,
        out_fmt: *mut bindings::ove_audio_fmt,
    ) -> core::ffi::c_int {
        if !in_fmt.is_null() && !out_fmt.is_null() {
            unsafe { *out_fmt = *in_fmt };
        }
        0
    }

    unsafe extern "C" fn process_trampoline<T: AudioProcessor>(
        ctx: *mut c_void,
        in_buf: *const bindings::ove_audio_buf,
        out_buf: *mut bindings::ove_audio_buf,
    ) -> core::ffi::c_int {
        let proc_: &mut T = unsafe { &mut *(ctx as *mut T) };
        let input = AudioBuf { raw: in_buf };
        let output = AudioBuf { raw: out_buf };
        proc_.process(&input, &output);
        0
    }

    // Build the ops vtable as a helper struct with static lifetime.
    // Function pointers are compile-time constants so this is safe.
    struct OpsHolder<U: AudioProcessor>(core::marker::PhantomData<U>);
    impl<U: AudioProcessor> OpsHolder<U> {
        // SAFETY: All fields are Option<fn ptr> or None — this is a valid
        // zero-initialization that we then patch with the real pointers.
        // The struct is never mutated after creation.
        #[allow(invalid_value)]
        const OPS: bindings::ove_audio_node_ops = {
            let mut ops: bindings::ove_audio_node_ops = unsafe { core::mem::zeroed() };
            ops.configure = Some(configure_trampoline);
            ops.process = Some(process_trampoline::<U>);
            ops
        };
    }

    let ctx = processor as *mut T as *mut c_void;
    let rc = unsafe {
        bindings::ove_audio_graph_add_node(
            graph,
            &OpsHolder::<T>::OPS,
            ctx,
            name.as_ptr() as *const _,
            bindings::OVE_AUDIO_NODE_PROCESSOR,
        )
    };
    if rc < 0 {
        Err(Error::from_code(rc).unwrap_err())
    } else {
        Ok(rc)
    }
}

/// A static wrapper for an [`AudioProcessor`] that provides safe
/// `&'static mut` access for [`graph_add_processor`].
///
/// # Example
///
/// ```ignore
/// static PROC: StaticProcessor<MyProc> = StaticProcessor::new(MyProc);
/// let idx = graph_add_processor(&mut graph, PROC.get_mut(), b"my-proc\0")?;
/// ```
pub struct StaticProcessor<T> {
    inner: core::cell::UnsafeCell<T>,
}

unsafe impl<T: Send> Send for StaticProcessor<T> {}
unsafe impl<T: Send> Sync for StaticProcessor<T> {}

impl<T> StaticProcessor<T> {
    /// Create a new static processor wrapper.
    pub const fn new(val: T) -> Self {
        Self {
            inner: core::cell::UnsafeCell::new(val),
        }
    }

    /// Obtain a `&'static mut` reference to the inner processor.
    ///
    /// Call once during graph setup before the audio thread starts.
    #[allow(clippy::mut_from_ref)]
    pub fn get_mut(&self) -> &mut T {
        unsafe { &mut *self.inner.get() }
    }
}
