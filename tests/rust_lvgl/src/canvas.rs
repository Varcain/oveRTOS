// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use super::{SERIAL, screen};
use ove::lvgl::{self, Canvas, CanvasBuffer, Color, ColorFormat, Obj, Widget};
use ove::{Error, Result};
use std::ffi::c_void;

unsafe extern "C" {
    fn canvas_test_begin();
    fn canvas_test_end();
    fn canvas_test_fail(after: u32);
    fn canvas_test_protect(ptr: *const c_void, size: usize);
    fn canvas_test_violations() -> u32;
    fn canvas_test_layout(obj: *mut c_void, values: *mut usize) -> i32;
    fn canvas_test_read(obj: *mut c_void, offset: usize, out: *mut c_void, size: usize) -> i32;
    fn canvas_test_live(id: usize) -> u32;
    fn canvas_test_external(ptr: *mut c_void, size: usize) -> usize;
    fn canvas_test_watch_delete(obj: *mut c_void);
    fn canvas_test_detached() -> u32;
    fn canvas_test_cache_drops() -> usize;
    fn text_test_used() -> usize;
    fn canvas_test_pool_allocator() -> i32;
    fn canvas_test_draw_alignment() -> usize;
}
struct Case(bool);
impl Case {
    fn new() -> Self {
        unsafe { canvas_test_begin() };
        Self(false)
    }
    fn finish(mut self) {
        unsafe { canvas_test_end() };
        self.0 = true;
        assert_eq!(unsafe { canvas_test_violations() }, 0);
    }
}
impl Drop for Case {
    fn drop(&mut self) {
        if !self.0 {
            // Cleanup precedes any deferred physical release, also on a guarded
            // mutant assertion. No Rust panic crosses a native callback.
            unsafe { canvas_test_end() };
        }
    }
}
fn layout(canvas: Canvas) -> [usize; 10] {
    let mut result = [0; 10];
    assert_eq!(
        unsafe { canvas_test_layout(canvas.raw().cast(), result.as_mut_ptr()) },
        1,
        "native allocation identity/span guard"
    );
    result
}
fn bytes(canvas: Canvas) -> Vec<u8> {
    let meta = layout(canvas);
    let mut result = vec![0; meta[3]];
    assert_eq!(
        unsafe {
            canvas_test_read(
                canvas.raw().cast(),
                0,
                result.as_mut_ptr().cast(),
                result.len(),
            )
        },
        1
    );
    result
}
fn closed(canvas: Canvas) {
    let ids = layout(canvas);
    unsafe { canvas_test_watch_delete(canvas.raw().cast()) };
    canvas.del();
    assert_eq!(unsafe { canvas_test_detached() }, 1);
    assert_eq!(unsafe { canvas_test_live(ids[6]) }, 0);
    assert_eq!(unsafe { canvas_test_live(ids[7]) }, 0);
}
#[repr(align(32))]
struct Raw([u8; 64]);

// Keep the initializer live through native cleanup if an ownership guard fails.
struct PendingCanvas(Option<Canvas>);
impl Drop for PendingCanvas {
    fn drop(&mut self) {
        if let Some(canvas) = self.0.take() {
            canvas.del();
        }
    }
}

#[test]
fn seeded_formats_and_owned_source() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    assert_eq!(size_of::<Canvas>(), size_of::<usize>());
    for (format, bits) in [
        (ColorFormat::I1, 1usize),
        (ColorFormat::A8, 8),
        (ColorFormat::RGB565, 16),
        (ColorFormat::RGB888, 24),
        (ColorFormat::ARGB8888, 32),
        (ColorFormat::XRGB8888, 32),
    ] {
        let (width, height) = (9, 2);
        let row = (width * bits).div_ceil(8);
        let palette = usize::from(bits == 1) * 8;
        let mut source = (0..palette + row * height)
            .map(|i| (i * 13) as u8)
            .collect::<Vec<_>>();
        let original = source.clone();
        let address = source.as_ptr() as usize;
        unsafe { canvas_test_protect(source.as_ptr().cast(), source.len()) };
        let canvas = Canvas::create(root).set_buffer(CanvasBuffer::new(
            &source,
            width as i32,
            height as i32,
            format,
        )?)?;
        let meta = layout(canvas);
        assert_ne!(meta[5], address, "initializer must not be retained");
        assert_eq!(&meta[..2], &[width, height]);
        assert!(meta[2] >= row);
        assert_eq!(meta[3], palette + meta[2] * height);
        assert_eq!(meta[4], 0, "decoder must normalize a private copy");
        assert_eq!(meta[5] % unsafe { canvas_test_draw_alignment() }, 0);
        source.fill(0x77);
        let copied = bytes(canvas);
        assert_eq!(&copied[..palette], &original[..palette]);
        for y in 0..height {
            assert_eq!(
                &copied[palette + y * meta[2]..palette + y * meta[2] + row],
                &original[palette + y * row..palette + (y + 1) * row]
            );
            assert!(
                copied[palette + y * meta[2] + row..palette + (y + 1) * meta[2]]
                    .iter()
                    .all(|b| *b == 0)
            );
        }
        closed(canvas);
    }
    case.finish();
    Ok(())
}

#[test]
fn blank_pixels_and_unaligned_initializer() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    let source = Raw([0x55; 64]);
    let canvas = Canvas::create(root).set_buffer(CanvasBuffer::new(
        &source.0[1..17],
        2,
        2,
        ColorFormat::XRGB8888,
    )?)?;
    assert_eq!(bytes(canvas), vec![0x55; 16]);
    closed(canvas);
    for format in [
        ColorFormat::A8,
        ColorFormat::RGB565,
        ColorFormat::RGB888,
        ColorFormat::ARGB8888,
        ColorFormat::XRGB8888,
    ] {
        let canvas = Canvas::create(root).allocate_buffer(3, 2, format)?;
        assert!(bytes(canvas).iter().all(|b| *b == 0));
        closed(canvas);
    }
    let canvas = Canvas::create(root).allocate_buffer(9, 2, ColorFormat::I1)?;
    let data = bytes(canvas);
    assert_eq!(&data[..8], &[0, 0, 0, 255, 255, 255, 255, 255]);
    assert!(data[8..].iter().all(|b| *b == 0));
    closed(canvas);
    case.finish();
    Ok(())
}

#[test]
fn bounds_and_input_length() {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    for (w, h) in [
        (-1, 0),
        (0, 1),
        (1, 0),
        (65536, 1),
        (65536, 65536),
        (16384, 1),
    ] {
        assert!(matches!(
            CanvasBuffer::new(&[], w, h, ColorFormat::XRGB8888),
            Err(Error::InvalidParam)
        ));
    }
    for size in [0, 63, 65] {
        assert!(matches!(
            CanvasBuffer::new(&vec![0; size], 4, 4, ColorFormat::XRGB8888),
            Err(Error::InvalidParam)
        ));
    }
    assert!(matches!(
        CanvasBuffer::new(&[0; 64], 4, 4, ColorFormat(255)),
        Err(Error::InvalidParam)
    ));
    assert!(matches!(
        CanvasBuffer::new(&[], 1, 1, ColorFormat::I1),
        Err(Error::InvalidParam)
    ));
    let wide = vec![0; 65536];
    assert!(matches!(
        CanvasBuffer::new(&wide, 65536, 1, ColorFormat::A8),
        Err(Error::InvalidParam)
    ));
    assert!(matches!(
        CanvasBuffer::new(&wide, 16384, 1, ColorFormat::XRGB8888),
        Err(Error::InvalidParam)
    ));
    assert!(matches!(
        CanvasBuffer::new(&wide, 1, 65536, ColorFormat::A8),
        Err(Error::InvalidParam)
    ));
    assert!(matches!(
        CanvasBuffer::new(&vec![0; 8200], 65536, 1, ColorFormat::I1),
        Err(Error::InvalidParam)
    ));
    let canvas = Canvas::create(root);
    for (w, h) in [(-1, 1), (0, 1), (1, 0), (65536, 1), (16384, 1)] {
        assert!(matches!(
            canvas.allocate_buffer(w, h, ColorFormat::XRGB8888),
            Err(Error::InvalidParam)
        ));
    }
    canvas.del();
    case.finish();
}

#[test]
fn source_scope_and_forgotten_view() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    let canvas = {
        let source = Box::new(Raw([0x33; 64]));
        let address = source.0.as_ptr() as usize;
        let canvas = Canvas::create(root).set_buffer(CanvasBuffer::new(
            &source.0,
            4,
            4,
            ColorFormat::XRGB8888,
        )?)?;
        let mut pending = PendingCanvas(Some(canvas));
        assert_ne!(layout(canvas)[5], address);
        pending.0 = None;
        canvas
    };
    struct View {
        _canvas: Canvas,
    }
    std::mem::forget(View { _canvas: canvas });
    canvas.fill_bg(Color::hex(0x112233), 255);
    assert_eq!(&bytes(canvas)[..4], &[0x33, 0x22, 0x11, 0xff]);
    closed(canvas);
    case.finish();
    Ok(())
}

#[test]
fn replacement_and_new_allocation_failures() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    let canvas = Canvas::create(root)
        .allocate_buffer(4, 4, ColorFormat::XRGB8888)?
        .fill_bg(Color::hex(0x112233), 255);
    for failure in [1, 2] {
        let old = layout(canvas);
        let pixels = bytes(canvas);
        let used = unsafe { text_test_used() };
        unsafe { canvas_test_fail(failure) };
        assert!(matches!(
            canvas.allocate_buffer(8, 8, ColorFormat::ARGB8888),
            Err(Error::NoMemory)
        ));
        assert_eq!(layout(canvas), old);
        assert_eq!(bytes(canvas), pixels);
        assert_eq!(unsafe { text_test_used() }, used);
    }
    for i in 0..32 {
        let old = layout(canvas);
        canvas.allocate_buffer(2 + i % 4, 3, ColorFormat::XRGB8888)?;
        assert_eq!(unsafe { canvas_test_live(old[6]) }, 0);
        assert_eq!(unsafe { canvas_test_live(old[7]) }, 0);
    }
    closed(canvas);
    case.finish();
    Ok(())
}

#[test]
fn borrowed_and_owned_replacement() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    // On assertion unwinding, native cleanup must precede caller deallocation.
    let mut first = Box::new(Raw([7; 64]));
    let mut second = Box::new(Raw([9; 64]));
    let case = Case::new();
    let canvas = Canvas::create(root).allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    let old = layout(canvas);
    let first_id = unsafe { canvas_test_external(first.0.as_mut_ptr().cast(), 64) };
    let second_id = unsafe { canvas_test_external(second.0.as_mut_ptr().cast(), 64) };
    unsafe { canvas.buffer(first.0.as_mut_ptr().cast(), 4, 4, ColorFormat::XRGB8888.0) }?;
    assert_eq!(unsafe { canvas_test_live(old[6]) }, 0);
    assert_eq!(unsafe { canvas_test_live(old[7]) }, 0);
    assert_eq!(bytes(canvas), vec![7; 64]);
    unsafe { canvas.buffer(second.0.as_mut_ptr().cast(), 4, 4, ColorFormat::XRGB8888.0) }?;
    assert_eq!(unsafe { canvas_test_live(first_id) }, 1);
    assert_eq!(first.0, [7; 64]);
    assert_eq!(bytes(canvas), vec![9; 64]);
    canvas.allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    assert_eq!(unsafe { canvas_test_live(second_id) }, 1);
    assert_eq!(second.0, [9; 64]);
    closed(canvas);
    case.finish();
    Ok(())
}

unsafe extern "C" {
    fn canvas_test_handlers(mode: u32);
    fn canvas_test_allocator_frees(which: u32) -> usize;
    fn canvas_test_symbol(obj: *mut c_void) -> usize;
    fn canvas_test_display_delete();
    fn canvas_test_async_delete(obj: *mut c_void);
    fn canvas_test_render_setup();
    fn canvas_test_cache(bytes: u32);
    fn canvas_test_decode(obj: *mut c_void, pixel: *mut u32) -> i32;
    fn canvas_test_layer() -> *mut ove::ffi::lv_layer_t;
    fn canvas_test_rect(layer: *mut c_void, width: i32, height: i32, rgb: u32);
    fn canvas_test_layer_free(layer: *mut c_void) -> i32;
}

#[test]
fn allocator_snapshot_and_native_stride() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    unsafe { canvas_test_handlers(1) };
    let source = [1, 2, 3, 4, 5, 6];
    let canvas =
        Canvas::create(root).set_buffer(CanvasBuffer::new(&source, 3, 2, ColorFormat::A8)?)?;
    assert_eq!(layout(canvas)[2], 4);
    assert_eq!(bytes(canvas), vec![1, 2, 3, 0, 4, 5, 6, 0]);
    unsafe { canvas_test_handlers(2) };
    closed(canvas);
    assert_eq!(unsafe { canvas_test_allocator_frees(0) }, 1);
    assert_eq!(unsafe { canvas_test_allocator_frees(1) }, 0);
    unsafe { canvas_test_handlers(0) };
    case.finish();
    Ok(())
}

#[test]
fn missing_allocator_preserves_canvas() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    let canvas = Canvas::create(root).allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    let old = layout(canvas);
    let data = bytes(canvas);
    let used = unsafe { text_test_used() };
    unsafe { canvas_test_handlers(3) };
    assert!(matches!(
        canvas.allocate_buffer(4, 4, ColorFormat::XRGB8888),
        Err(Error::NotSupported)
    ));
    assert_eq!(layout(canvas), old);
    assert_eq!(bytes(canvas), data);
    assert_eq!(unsafe { text_test_used() }, used);
    unsafe { canvas_test_handlers(0) };
    closed(canvas);
    case.finish();
    Ok(())
}

use ove::lvgl::{EventCtx, EventTarget};
use std::sync::{
    Mutex,
    atomic::{AtomicUsize, Ordering},
};
static CALLBACK_CANVAS: Mutex<Option<Canvas>> = Mutex::new(None);
static CALLBACK_GOOD: AtomicUsize = AtomicUsize::new(0);
static CALLBACK_BAD: AtomicUsize = AtomicUsize::new(0);
fn during_delete(_: EventCtx<'_>) {
    let canvas = CALLBACK_CANVAS.lock().ok().and_then(|v| *v);
    let Some(canvas) = canvas else {
        CALLBACK_BAD.fetch_add(1, Ordering::Relaxed);
        return;
    };
    let mut data = [0; 10];
    if unsafe { canvas_test_layout(canvas.raw().cast(), data.as_mut_ptr()) } != 1 {
        CALLBACK_BAD.fetch_add(1, Ordering::Relaxed);
        return;
    }
    canvas
        .fill_bg(Color::hex(0x102030), 255)
        .set_pixel(1, 1, Color::white());
    if !matches!(
        canvas.allocate_buffer(2, 2, ColorFormat::XRGB8888),
        Err(Error::BadHandle)
    ) {
        CALLBACK_BAD.fetch_add(1, Ordering::Relaxed);
        return;
    }
    CALLBACK_GOOD.fetch_add(1, Ordering::Relaxed);
}

#[test]
fn delete_callbacks_keep_pixels_through_child_deletion() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    CALLBACK_GOOD.store(0, Ordering::Relaxed);
    CALLBACK_BAD.store(0, Ordering::Relaxed);
    let canvas = Canvas::create(root)
        .on_fn(ove::ffi::LV_EVENT_DELETE, during_delete)
        .allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    canvas.on_fn(ove::ffi::LV_EVENT_DELETE, during_delete);
    lvgl::Box::create(canvas).on_fn(ove::ffi::LV_EVENT_DELETE, during_delete);
    *CALLBACK_CANVAS.lock().unwrap() = Some(canvas);
    closed(canvas);
    *CALLBACK_CANVAS.lock().unwrap() = None;
    assert_eq!(CALLBACK_GOOD.load(Ordering::Relaxed), 3);
    assert_eq!(CALLBACK_BAD.load(Ordering::Relaxed), 0);
    case.finish();
    Ok(())
}

#[test]
fn native_parent_clean_display_and_async_deletion() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    for mode in 0..4 {
        let parent = Obj::create(root);
        let canvas = Canvas::create(parent).allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
        let ids = layout(canvas);
        unsafe { canvas_test_watch_delete(canvas.raw().cast()) };
        match mode {
            0 => parent.clean(),
            1 => {
                unsafe { canvas_test_async_delete(canvas.raw().cast()) };
            }
            2 => parent.del(),
            _ => root.clean(),
        }
        assert_eq!(unsafe { canvas_test_detached() }, 1);
        assert_eq!(unsafe { canvas_test_live(ids[6]) }, 0);
        assert_eq!(unsafe { canvas_test_live(ids[7]) }, 0);
        if mode < 2 {
            parent.del();
        }
    }
    let canvas = Canvas::create(root).allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    let ids = layout(canvas);
    unsafe {
        canvas_test_watch_delete(canvas.raw().cast());
        canvas_test_display_delete()
    };
    assert_eq!(unsafe { canvas_test_detached() }, 1);
    assert_eq!(unsafe { canvas_test_live(ids[6]) }, 0);
    assert_eq!(unsafe { canvas_test_live(ids[7]) }, 0);
    case.finish();
    Ok(())
}

#[test]
fn native_symbol_source_uses_base_image_cleanup() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    let canvas = Canvas::create(root).allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    let symbol = unsafe { canvas_test_symbol(canvas.raw().cast()) };
    assert_ne!(symbol, 0);
    assert_eq!(unsafe { canvas_test_live(symbol) }, 1);
    closed(canvas);
    assert_eq!(unsafe { canvas_test_live(symbol) }, 0);
    case.finish();
    Ok(())
}

fn decoded(canvas: Canvas) -> u32 {
    let mut value = 0;
    assert_eq!(
        unsafe { canvas_test_decode(canvas.raw().cast(), &raw mut value) },
        1
    );
    value
}
#[test]
fn alpha_normalization_and_redraw_cache() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    unsafe { canvas_test_cache(4096) };
    let source = [20, 50, 100, 128];
    let canvas = Canvas::create(root).set_buffer(CanvasBuffer::new(
        &source,
        1,
        1,
        ColorFormat::ARGB8888,
    )?)?;
    assert_eq!(decoded(canvas), 0x8032190a);
    assert_eq!(bytes(canvas), source);
    let drops = unsafe { canvas_test_cache_drops() };
    canvas.fill_bg(Color::hex(0x604020), 64);
    assert!(unsafe { canvas_test_cache_drops() } > drops);
    assert_eq!(decoded(canvas), 0x40181008);
    assert_eq!(bytes(canvas), vec![0x20, 0x40, 0x60, 0x40]);
    canvas.set_pixel(0, 0, Color::hex(0x0a141e));
    assert_eq!(decoded(canvas), 0xff0a141e);
    closed(canvas);
    case.finish();
    Ok(())
}

#[test]
fn unsafe_layer_finish_updates_cached_image() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    unsafe {
        canvas_test_render_setup();
        canvas_test_cache(4096)
    };
    let canvas = Canvas::create(root).allocate_buffer(4, 4, ColorFormat::ARGB8888)?;
    assert_eq!(decoded(canvas), 0);
    let layer = unsafe { canvas_test_layer() };
    assert!(!layer.is_null());
    unsafe {
        canvas.init_layer(layer);
        canvas_test_rect(layer.cast(), 4, 4, 0x203040);
        canvas.finish_layer(layer);
        assert_eq!(canvas_test_layer_free(layer.cast()), 1);
    }
    assert_eq!(decoded(canvas), 0xff203040);
    closed(canvas);
    case.finish();
    Ok(())
}

#[test]
fn native_pool_recovers_full_canvas_allocations() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    // Warm the native class/theme path before comparing actual pool occupancy.
    closed(Canvas::create(root).allocate_buffer(1, 1, ColorFormat::XRGB8888)?);
    let before = unsafe { text_test_used() };
    let mut peak = before;
    let mut resources = [0; 10];
    for _ in 0..64 {
        let canvas = Canvas::create(root).allocate_buffer(64, 64, ColorFormat::XRGB8888)?;
        resources = layout(canvas);
        peak = peak.max(unsafe { text_test_used() });
        canvas.fill_bg(Color::hex(0x123456), 255);
        closed(canvas);
        if unsafe { canvas_test_pool_allocator() } != 0 {
            assert_eq!(unsafe { text_test_used() }, before);
        }
    }
    println!(
        "canvas allocator: pool={} baseline={before} peak={peak} recovered={}; instance={} descriptor_and_handlers={} pixel_bytes={}",
        unsafe { canvas_test_pool_allocator() },
        unsafe { text_test_used() },
        resources[8],
        resources[9],
        resources[3]
    );
    case.finish();
    Ok(())
}

#[test]
fn invalid_native_configuration_preserves_pixels() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    let canvas = Canvas::create(root).allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    for mode in [4, 5, 6, 7, 8] {
        let old = layout(canvas);
        let pixels = bytes(canvas);
        let used = unsafe { text_test_used() };
        unsafe { canvas_test_handlers(mode) };
        let result = canvas.allocate_buffer(4, 4, ColorFormat::XRGB8888);
        let expected = if mode < 7 {
            Error::InvalidParam
        } else {
            Error::NotSupported
        };
        assert!(matches!(result,Err(error) if error==expected));
        assert_eq!(layout(canvas), old);
        assert_eq!(bytes(canvas), pixels);
        assert_eq!(unsafe { text_test_used() }, used);
        unsafe { canvas_test_handlers(0) };
    }
    closed(canvas);
    case.finish();
    Ok(())
}

#[test]
fn private_native_entry_rejects_wrong_class_and_shape() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lock = lvgl::lock();
    let case = Case::new();
    for obj in [core::ptr::null_mut(), root.raw().cast()] {
        let result =
            unsafe { ove::ffi::ove_lvgl_canvas_copy(obj, core::ptr::null(), 0, 4, 4, 0x11) };
        assert_eq!(Error::from_code(result), Err(Error::BadHandle));
    }
    let canvas = Canvas::create(root).allocate_buffer(4, 4, ColorFormat::XRGB8888)?;
    let old = layout(canvas);
    let data = [0u8; 64];
    for (w, h, size, format) in [
        (0, 4, 64, 0x11),
        (4, 0, 64, 0x11),
        (4, 4, 63, 0x11),
        (4, 4, 64, 255),
    ] {
        let result = unsafe {
            ove::ffi::ove_lvgl_canvas_copy(canvas.raw().cast(), data.as_ptr(), size, w, h, format)
        };
        assert_eq!(Error::from_code(result), Err(Error::InvalidParam));
        assert_eq!(layout(canvas), old);
    }
    closed(canvas);
    case.finish();
    Ok(())
}
