// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.
#![cfg(test)]

use ove::lvgl::{
    BufferedLabel, EventTarget, Image, ImageSrc, Label, List, Obj, State, TextStorage, Widget,
};
use ove::{Error, Result};
use std::ffi::{CStr, c_char, c_void};
use std::fmt;
use std::sync::Mutex;
use std::sync::atomic::{AtomicPtr, Ordering};

static SERIAL: Mutex<()> = Mutex::new(());

unsafe extern "C" {
    fn text_test_screen() -> *mut ove::ffi::lv_obj_t;
    fn text_test_label(obj: *mut ove::ffi::lv_obj_t) -> *const c_char;
    fn text_test_used() -> usize;
    fn text_test_alloc_calls() -> usize;
    fn text_test_subject_observer(obj: *mut c_void, subject: *mut c_void) -> *mut c_void;
    fn text_test_remove_observer(observer: *mut c_void);
    fn text_test_send(obj: *mut c_void);
}

fn screen() -> Obj {
    unsafe { Obj::from_raw(text_test_screen()) }
}
fn value(label: Label) -> String {
    unsafe { CStr::from_ptr(text_test_label(label.raw())) }
        .to_str()
        .unwrap()
        .into()
}

#[test]
fn native_text_and_lifetimes() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lvgl = ove::lvgl::lock();
    let label = Label::create(root).text("initial")?;
    let source = String::from("Hello, 世界");
    label.set_text(&source)?;
    drop(source);
    assert_eq!(value(label), "Hello, 世界");
    assert_eq!(label.set_text("bad\0suffix"), Err(Error::InvalidParam));
    assert_eq!(value(label), "Hello, 世界");
    for length in [0, 127, 128, 129, 4000] {
        let text = "λ".repeat(length);
        label.set_text(&text)?;
        assert_eq!(value(label), text);
    }
    label.set_text_fmt(format_args!("{} {:04x}", "rust", 31))?;
    assert_eq!(value(label), "rust 001f");
    struct Fails;
    impl fmt::Display for Fails {
        fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
            out.write_str("partial")?;
            Err(fmt::Error)
        }
    }
    assert_eq!(
        label.set_text_fmt(format_args!("{Fails}")),
        Err(Error::InvalidParam)
    );
    assert_eq!(value(label), "rust 001f");
    let before = unsafe { text_test_used() };
    assert_eq!(
        label.set_text(&"x".repeat(1024 * 1024)),
        Err(Error::NoMemory)
    );
    assert_eq!(unsafe { text_test_used() }, before);
    assert_eq!(value(label), "rust 001f");

    static STORAGE: TextStorage<16> = TextStorage::new();
    static EMPTY: TextStorage<0> = TextStorage::new();
    assert!(matches!(
        label.text_buffered(&EMPTY, format_args!("")),
        Err(Error::NoSpace)
    ));
    let buffered = label.text_buffered(&STORAGE, format_args!("Count: 0"))?;
    let before = unsafe { text_test_used() };
    let calls = unsafe { text_test_alloc_calls() };
    for count in 0..1000 {
        buffered.set_text_fmt(format_args!("Count: {count}"))?;
    }
    assert_eq!(unsafe { text_test_alloc_calls() }, calls);
    assert_eq!(unsafe { text_test_used() }, before);
    assert_eq!(value(label), "Count: 999");
    assert_eq!(buffered.set_text("1234567890123456"), Err(Error::NoSpace));
    assert_eq!(buffered.set_text("nul\0"), Err(Error::InvalidParam));
    assert_eq!(value(label), "Count: 999");
    label.set_text("replacement")?;
    assert_eq!(buffered.set_text("stale"), Err(Error::BadHandle));
    let newer = label.text_buffered(&STORAGE, format_args!("new"))?;
    assert_eq!(buffered.set_text("stale"), Err(Error::BadHandle));
    unsafe { ove::ffi::lv_obj_delete(label.raw()) };
    assert_eq!(newer.set_text("deleted"), Err(Error::BadHandle));
    let label = Label::create(root);
    let reuse = label.text_buffered(&STORAGE, format_args!("reused"))?;
    reuse.set_text("ready")?;

    let owner = Box::pin(State::new(7_i32));
    let state = owner.as_ref().get_ref();
    let label = label.bind_text(&state, |v, out| write!(out, "Value: {v}"))?;
    assert_eq!(reuse.set_text("replaced"), Err(Error::BadHandle));
    assert_eq!(value(label), "Value: 7");
    state.set(9);
    assert_eq!(value(label), "Value: 9");
    assert_eq!(label.text_status(), Ok(()));
    // A second native observer on the same subject must not remove our owner.
    let other =
        unsafe { text_test_subject_observer(label.raw().cast(), state.subject_ptr().cast()) };
    unsafe { text_test_remove_observer(other) };
    state.set(10);
    assert_eq!(value(label), "Value: 10");
    label.set_text("unbound")?;
    state.set(11);
    assert_eq!(value(label), "unbound");
    assert_eq!(label.text_status(), Err(Error::NotFound));
    let label = label.bind_text(&state, |v, out| {
        if v == 12 {
            Err(fmt::Error)
        } else {
            write!(out, "{v}")
        }
    })?;
    state.set(12);
    assert_eq!(value(label), "11");
    assert_eq!(label.text_status(), Err(Error::InvalidParam));
    state.set(13);
    assert_eq!(value(label), "13");
    assert_eq!(label.text_status(), Ok(()));
    drop(owner);
    assert_eq!(label.text_status(), Err(Error::NotFound));
    Ok(())
}

static TARGET: AtomicPtr<ove::ffi::lv_obj_t> = AtomicPtr::new(std::ptr::null_mut());
static SUBJECT: AtomicPtr<State<i32>> = AtomicPtr::new(std::ptr::null_mut());

struct Deletes;
impl fmt::Display for Deletes {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        unsafe { ove::ffi::lv_obj_delete(TARGET.load(Ordering::Relaxed)) };
        out.write_str("deleted")
    }
}

struct Swallows;
impl fmt::Display for Swallows {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        let _ = out.write_str("bad\0tail");
        let _ = out.write_str("later");
        Ok(())
    }
}

struct Replaces;
impl fmt::Display for Replaces {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        let label = unsafe { Label::from_raw(TARGET.load(Ordering::Relaxed)) };
        label.set_text("inner").unwrap();
        out.write_str("outer")
    }
}

struct Nested(BufferedLabel<32>);
impl fmt::Display for Nested {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        assert_eq!(self.0.set_text("nested"), Err(Error::Busy));
        out.write_str("outer")
    }
}

#[test]
fn reentrant_formatting() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lvgl = ove::lvgl::lock();
    let image = Image::create(root);
    let list = List::create(root);
    for text in ["", "\u{1}AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "\n"] {
        assert!(matches!(
            image.source(ImageSrc::from_path(text)),
            Err(Error::InvalidParam)
        ));
        assert!(matches!(
            image.source(ImageSrc::from_symbol(text)),
            Err(Error::InvalidParam)
        ));
        assert!(matches!(
            list.add_button(Some(ImageSrc::from_path(text)), "row"),
            Err(Error::InvalidParam)
        ));
    }
    assert!(matches!(
        image.source(ImageSrc::from_symbol("ASCII")),
        Err(Error::InvalidParam)
    ));
    assert!(matches!(
        image.source(ImageSrc::from_path("λ")),
        Err(Error::InvalidParam)
    ));
    let symbol = String::from("✓");
    image.source(ImageSrc::from_symbol(&symbol))?;
    list.add_button(Some(ImageSrc::from_symbol(&symbol)), "row")?;
    drop(symbol);
    let checked = Label::create(root).text("intact")?;
    assert_eq!(
        checked.set_text_fmt(format_args!("{Swallows}")),
        Err(Error::InvalidParam)
    );
    assert_eq!(value(checked), "intact");
    static CHECKED_TEXT: TextStorage<16> = TextStorage::new();
    let checked_buffer = checked.text_buffered(&CHECKED_TEXT, format_args!("intact"))?;
    assert_eq!(
        checked_buffer.set_text_fmt(format_args!("{Swallows}")),
        Err(Error::InvalidParam)
    );
    assert_eq!(value(checked), "intact");
    let label = Label::create(root);
    TARGET.store(label.raw(), Ordering::Relaxed);
    assert_eq!(
        label.set_text_fmt(format_args!("{Deletes}")),
        Err(Error::BadHandle)
    );
    let owner = Box::pin(State::new(0_i32));
    let state = owner.as_ref().get_ref();
    let label = Label::create(root);
    TARGET.store(label.raw(), Ordering::Relaxed);
    let bound = label.bind_text(&state, |v, out| {
        if v == 1 {
            write!(out, "{Deletes}")
        } else {
            write!(out, "{v}")
        }
    })?;
    state.set(1);
    assert_eq!(bound.text_status(), Err(Error::NotFound));
    let label = Label::create(root);
    SUBJECT.store((state as *const State<i32>).cast_mut(), Ordering::Relaxed);
    label.bind_text(&state, |v, out| {
        if v == 2 {
            unsafe { &*SUBJECT.load(Ordering::Relaxed) }.set(3);
        }
        write!(out, "{v}")
    })?;
    state.set(2);
    assert!(label.text_status() == Err(Error::Busy) || value(label) == "3");
    state.set(4);
    assert_eq!(value(label), "4");
    assert_eq!(label.text_status(), Ok(()));
    TARGET.store(label.raw(), Ordering::Relaxed);
    label.bind_text(&state, |v, out| {
        if v == 5 {
            write!(out, "{Replaces}")
        } else {
            write!(out, "{v}")
        }
    })?;
    state.set(5);
    assert_eq!(value(label), "inner");
    assert_eq!(label.text_status(), Err(Error::NotFound));
    label.bind_text(&state, |v, out| {
        if v == 6 {
            let _ = out.write_str("bad\0tail");
            return Ok(());
        }
        write!(out, "{v}")
    })?;
    state.set(6);
    assert_eq!(value(label), "5");
    assert_eq!(label.text_status(), Err(Error::InvalidParam));
    let label = Label::create(root);
    static STORAGE: TextStorage<32> = TextStorage::new();
    let buffered = label.text_buffered(&STORAGE, format_args!("ready"))?;
    buffered.set_text_fmt(format_args!("{}", Nested(buffered)))?;
    assert_eq!(value(label), "outer");
    TARGET.store(label.raw(), Ordering::Relaxed);
    assert_eq!(
        buffered.set_text_fmt(format_args!("{Deletes}")),
        Err(Error::BadHandle)
    );
    assert_eq!(buffered.set_text("stale"), Err(Error::BadHandle));
    Ok(())
}

#[test]
fn observer_replacement_and_delete_events() -> Result<()> {
    let _serial = SERIAL.lock().unwrap();
    let root = screen();
    let _lvgl = ove::lvgl::lock();
    let owner = Box::pin(State::new(1_i32));
    let state = owner.as_ref().get_ref();
    SUBJECT.store((state as *const State<i32>).cast_mut(), Ordering::Relaxed);
    let label = Label::create(root);
    TARGET.store(label.raw(), Ordering::Relaxed);
    assert!(matches!(
        label.bind_text(state, |_, out| write!(out, "{Deletes}")),
        Err(Error::NotFound)
    ));
    let label = Label::create(root);
    TARGET.store(label.raw(), Ordering::Relaxed);
    assert!(matches!(
        label.bind_text(state, |_, out| {
            let label = unsafe { Label::from_raw(TARGET.load(Ordering::Relaxed)) };
            let state = unsafe { &*SUBJECT.load(Ordering::Relaxed) };
            label
                .bind_text(state, |v, out| write!(out, "new {v}"))
                .unwrap();
            out.write_str("obsolete")
        }),
        Err(Error::NotFound)
    ));
    assert_eq!(value(label), "new 1");
    assert_eq!(label.text_status(), Ok(()));
    state.set(2);
    assert_eq!(value(label), "new 2");
    label.on_clicked(|event| {
        static FIRST: TextStorage<32> = TextStorage::new();
        static SECOND: TextStorage<32> = TextStorage::new();
        let label = unsafe { Label::from_raw(event.target().raw()) };
        let state = unsafe { &*SUBJECT.load(Ordering::Relaxed) };
        label
            .text_buffered(&FIRST, format_args!("first buffer"))
            .unwrap();
        label.set_text("first").unwrap();
        label
            .text_buffered(&SECOND, format_args!("second buffer"))
            .unwrap();
        label.set_text("copied").unwrap();
        label
            .text_buffered(&FIRST, format_args!("reused buffer"))
            .unwrap();
        label
            .bind_text(state, |v, out| write!(out, "old {v}"))
            .unwrap();
        label.set_text("second").unwrap();
        label
            .bind_text(state, |v, out| write!(out, "last {v}"))
            .unwrap();
    });
    unsafe { text_test_send(label.raw().cast()) };
    state.set(3);
    assert_eq!(value(label), "last 3");
    label.set_text("same")?;
    // Warm the event array before measuring repeated context cleanup.
    label.bind_text(state, |_, out| out.write_str("same"))?;
    label.set_text("same")?;
    let before = unsafe { text_test_used() };
    for _ in 0..100 {
        label.bind_text(state, |_, out| out.write_str("same"))?;
        label.set_text("same")?;
    }
    assert_eq!(unsafe { text_test_used() }, before);
    static LIVE: TextStorage<32> = TextStorage::new();
    label.text_buffered(&LIVE, format_args!("live"))?;
    static DELETING: TextStorage<32> = TextStorage::new();
    label.on_fn(ove::ffi::LV_EVENT_DELETE, |event| {
        let label = unsafe { Label::from_raw(event.target().raw()) };
        assert!(
            label
                .text_buffered(&DELETING, format_args!("late"))
                .is_err()
        );
        let state = unsafe { &*SUBJECT.load(Ordering::Relaxed) };
        assert!(label.bind_text(state, |v, out| write!(out, "{v}")).is_err());
    });
    unsafe { ove::ffi::lv_obj_delete(label.raw()) };
    let next = Label::create(root).text_buffered(&DELETING, format_args!("reusable"))?;
    assert_eq!(value(next.label()), "reusable");
    SUBJECT.store(std::ptr::null_mut(), Ordering::Relaxed);
    Ok(())
}

mod canvas;
