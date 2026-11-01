// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use core::ffi::{c_char, c_void};
use embedded_hal::spi::SpiBus;
use ove::spi::{ChipSelect, Config, Transfer, Word};
use ove::{Error, Spi, Spi16, SpiStorage};
use std::ffi::CStr;
use std::sync::{Arc, Barrier, Mutex};
use std::time::Duration;

unsafe extern "C" {
    fn spi_test_reset();
    fn spi_test_stat(index: u32) -> u32;
    fn spi_test_trace() -> *const c_char;
    fn spi_test_fail(stage: u32);
    fn spi_test_fail_segment(index: u32);
    fn spi_test_segment(index: u32, field: u32) -> usize;
    fn spi_test_raw(bits: u32) -> *mut c_void;
    fn spi_test_raw_close();
    fn spi_test_malformed(handle: *mut c_void, misalign: u32) -> i32;
    fn spi_test_layout(field: u32) -> usize;
    fn spi_test_gate_start();
    fn spi_test_gate_wait();
    fn spi_test_gate_release();
}
const ALLOC: u32 = 0;
const FREE: u32 = 1;
const INIT: u32 = 2;
const DEINIT: u32 = 3;
const OPEN: u32 = 4;
const CLOSE: u32 = 5;
const HAL: u32 = 6;
const INVALID: u32 = 10;
const TIMEOUT: Duration = Duration::from_millis(23);
const CS: ChipSelect = ChipSelect::new(2, 7, true);
static SERIAL: Mutex<()> = Mutex::new(());
static STORAGE8: SpiStorage = SpiStorage::new();
static STORAGE16: SpiStorage<u16> = SpiStorage::new();

// Each test holds SERIAL; concurrent claim tests synchronize native callbacks.
fn reset() {
    unsafe { spi_test_reset() }
}
fn stat(index: u32) -> u32 {
    unsafe { spi_test_stat(index) }
}
fn segment(index: u32, field: u32) -> usize {
    unsafe { spi_test_segment(index, field) }
}
fn trace() -> String {
    unsafe { CStr::from_ptr(spi_test_trace()) }
        .to_str()
        .unwrap()
        .to_owned()
}
fn check_trace(expected: &str) {
    assert_eq!(trace(), expected, "one real mutex/software CS interval");
    assert_eq!(stat(INVALID), 0, "real POSIX mutex held at each HAL entry");
}
trait ProbeWord: Word + From<u8> + core::fmt::Debug + Eq {
    const FILL: Self;
}
impl ProbeWord for u8 {
    const FILL: Self = 0xa5;
}
impl ProbeWord for u16 {
    const FILL: Self = 0xa5a5;
}

fn spans<W: ProbeWord>(storage: &'static SpiStorage<W>) {
    let mut spi = storage.init(Config::default()).unwrap();
    let tx = [W::from(1), W::from(2), W::from(0xe1), W::from(0xe2)];
    let word_bytes = size_of::<W>();
    for via_trait in [false, true] {
        reset();
        let mut rx = [W::from(0xcc); 4];
        if via_trait {
            SpiBus::transfer(&mut spi, &mut rx, &tx[..2]).unwrap();
        } else {
            spi.transfer(Some(&CS), &tx[..2], &mut rx, TIMEOUT).unwrap();
        }
        assert_eq!(
            rx,
            [W::from(1), W::from(2), W::FILL, W::FILL],
            "TX subslice sentinel bytes must never reach RX"
        );
        assert_eq!(stat(HAL), 2);
        assert_eq!(segment(0, 0), tx.as_ptr() as usize);
        assert_eq!(segment(0, 1), rx.as_ptr() as usize);
        assert_eq!(segment(1, 0), 0);
        assert_eq!(segment(1, 1), rx.as_ptr() as usize + 2 * word_bytes);
        assert_eq!(segment(0, 2), 2 * word_bytes);
        assert_eq!(segment(1, 2), 2 * word_bytes);
        assert_eq!(segment(0, 4), word_bytes * 8, "native word configuration");
        if !via_trait {
            assert_eq!(segment(0, 3), TIMEOUT.as_nanos() as usize);
        }
        check_trace(if via_trait { "LHHU" } else { "LAHHDU" });

        reset();
        rx.fill(W::from(0xcc));
        if via_trait {
            SpiBus::transfer(&mut spi, &mut rx[..1], &tx).unwrap();
        } else {
            spi.transfer(Some(&CS), &tx, &mut rx[..1], TIMEOUT).unwrap();
        }
        assert_eq!(
            rx,
            [W::from(1), W::from(0xcc), W::from(0xcc), W::from(0xcc)],
            "C must preserve bytes outside the Rust RX subslice"
        );
        assert_eq!(segment(1, 0), tx.as_ptr() as usize + word_bytes);
        assert_eq!(segment(1, 1), 0);
        assert_eq!(segment(0, 2), word_bytes);
        assert_eq!(segment(1, 2), 3 * word_bytes);
        check_trace(if via_trait { "LHHU" } else { "LAHHDU" });
    }
    reset();
    let mut rx = [W::from(0); 4];
    spi.transfer(None, &tx, &mut rx, TIMEOUT).unwrap();
    assert_eq!(rx, tx);
    check_trace("LHU");
    for via_trait in [false, true] {
        reset();
        if via_trait {
            SpiBus::read(&mut spi, &mut rx).unwrap();
            SpiBus::write(&mut spi, &tx).unwrap();
            SpiBus::transfer_in_place(&mut spi, &mut rx).unwrap();
            SpiBus::flush(&mut spi).unwrap();
        } else {
            spi.read(None, &mut rx, TIMEOUT).unwrap();
            spi.write(None, &tx, TIMEOUT).unwrap();
            spi.transfer_in_place(None, &mut rx, TIMEOUT).unwrap();
        }
        assert_eq!(rx, [W::FILL; 4]);
        assert_eq!(segment(2, 0), segment(2, 1), "exact in-place pointers");
        assert_eq!(segment(2, 2), 4 * word_bytes);
        check_trace("LHULHULHU");
    }
    reset();
    spi.read(Some(&CS), &mut [], TIMEOUT).unwrap();
    spi.write(Some(&CS), &[], TIMEOUT).unwrap();
    spi.transfer(Some(&CS), &[], &mut [], TIMEOUT).unwrap();
    spi.transfer_in_place(Some(&CS), &mut [], TIMEOUT).unwrap();
    spi.transfer_seq(Some(&CS), &mut [], TIMEOUT).unwrap();
    spi.transfer_seq(
        Some(&CS),
        &mut [Transfer::write(&[]), Transfer::read(&mut [])],
        TIMEOUT,
    )
    .unwrap();
    SpiBus::read(&mut spi, &mut []).unwrap();
    SpiBus::write(&mut spi, &[]).unwrap();
    SpiBus::transfer(&mut spi, &mut [], &[]).unwrap();
    SpiBus::transfer_in_place(&mut spi, &mut []).unwrap();
    SpiBus::flush(&mut spi).unwrap();
    check_trace("");
    assert_eq!(stat(HAL), 0, "all empty operations skip native effects");
}

#[test]
fn byte_spans_and_actual_traits() {
    let _serial = SERIAL.lock().unwrap();
    spans(&STORAGE8);
}
#[test]
fn word_spans_and_actual_traits() {
    let _serial = SERIAL.lock().unwrap();
    spans(&STORAGE16);
}

fn sequences<W: ProbeWord>(storage: &'static SpiStorage<W>) {
    let mut spi = storage.init(Config::default()).unwrap();
    reset();
    let tx = [W::from(7), W::from(8)];
    let mut rx = vec![[W::from(0xcc); 3]; 137];
    let mut entries: Vec<_> = rx
        .iter_mut()
        .enumerate()
        .map(|(i, row)| {
            if i == 0 || i == 68 || i == 136 {
                Transfer::write(&[])
            } else {
                Transfer::duplex(&tx, &mut row[..if i % 2 == 0 { 3 } else { 1 }])
            }
        })
        .collect();
    spi.transfer_seq(Some(&CS), &mut entries, TIMEOUT).unwrap();
    drop(entries);
    assert_eq!(
        stat(HAL),
        268,
        "all 137 logical entries retain their native stride"
    );
    assert_eq!(trace(), format!("LA{}DU", "H".repeat(268)));
    for (i, row) in rx.iter().enumerate() {
        let expected = if i == 0 || i == 68 || i == 136 {
            [W::from(0xcc); 3]
        } else if i % 2 == 0 {
            [W::from(7), W::from(8), W::FILL]
        } else {
            [W::from(7), W::from(0xcc), W::from(0xcc)]
        };
        assert_eq!(*row, expected, "logical descriptor {i}");
    }
    reset();
    let mut read = [W::from(0); 2];
    let mut inplace = tx;
    let mut descriptors = [
        Transfer::write(&tx),
        Transfer::read(&mut read),
        Transfer::in_place(&mut inplace),
    ];
    spi.transfer_seq(Some(&CS), &mut descriptors, TIMEOUT)
        .unwrap();
    spi.transfer_seq(Some(&CS), &mut descriptors, TIMEOUT)
        .unwrap();
    drop(descriptors);
    assert_eq!(read, [W::FILL; 2]);
    assert_eq!(inplace, tx);
    check_trace("LAHHHDULAHHHDU");
    reset();
    unsafe { spi_test_fail_segment(2) };
    let result = spi.transfer_seq(
        Some(&CS),
        &mut [
            Transfer::write(&tx),
            Transfer::write(&tx),
            Transfer::write(&tx),
        ],
        TIMEOUT,
    );
    assert_eq!(result, Err(Error::Io));
    check_trace("LAHHDU");
    unsafe { spi_test_fail_segment(0) };
    spi.write(Some(&CS), &tx, TIMEOUT).unwrap();
    check_trace("LAHHDULAHDU");
}
#[test]
fn arbitrary_byte_sequences_and_error_cleanup() {
    let _serial = SERIAL.lock().unwrap();
    sequences(&STORAGE8);
}
#[test]
fn arbitrary_word_sequences_and_error_cleanup() {
    let _serial = SERIAL.lock().unwrap();
    sequences(&STORAGE16);
}

#[test]
fn static_ownership_busy_reuse_and_failures() {
    let _serial = SERIAL.lock().unwrap();
    for stage in [2, 3] {
        reset();
        unsafe { spi_test_fail(stage) };
        assert!(STORAGE8.init(Config::default()).is_err());
        assert_eq!(stat(INIT), 1);
        assert_eq!(stat(DEINIT), u32::from(stage == 3));
        assert_eq!(stat(CLOSE), 0, "failed out handles are never owners");
        unsafe { spi_test_fail(0) };
        let owner = STORAGE8.init(Config::default()).unwrap();
        let moved = owner;
        let pointer = moved.raw();
        assert_eq!(STORAGE8.init(Config::default()).unwrap_err(), Error::Busy);
        assert_eq!(stat(INIT), 2, "busy admission cannot touch native backing");
        assert_eq!(pointer, moved.raw());
        drop(moved);
        assert_eq!(stat(CLOSE), 1);
        assert_eq!(stat(DEINIT), 1 + u32::from(stage == 3));
        drop(STORAGE8.init(Config::default()).unwrap());
        assert_eq!(stat(CLOSE), 2);
        assert_eq!(stat(ALLOC), 0);
        assert_eq!(stat(FREE), 0);
    }
}
#[cfg(not(zero_heap))]
#[test]
fn heap_ownership_and_failed_out_handles() {
    let _serial = SERIAL.lock().unwrap();
    for stage in 0..=3 {
        reset();
        unsafe { spi_test_fail(stage) };
        let result = Spi::new(Config::default());
        assert_eq!(result.is_ok(), stage == 0);
        drop(result);
        assert_eq!(stat(ALLOC), 1);
        assert_eq!(stat(FREE), u32::from(stage != 1));
        assert_eq!(stat(CLOSE), u32::from(stage == 0));
        assert_eq!(stat(DEINIT), u32::from(stage == 0 || stage == 3));
    }
    reset();
    let mut spi = Spi16::new(Config::default()).unwrap();
    let moved_address = spi.raw();
    spi.write(None, &[0x1234], TIMEOUT).unwrap();
    let moved = spi;
    assert_eq!(moved.raw(), moved_address);
    assert_eq!(segment(0, 4), 16);
    drop(moved);
    assert_eq!((stat(ALLOC), stat(FREE), stat(CLOSE)), (1, 1, 1));
}

#[test]
fn forgotten_static_owner_retains_claim() {
    let _serial = SERIAL.lock().unwrap();
    static FORGOTTEN: SpiStorage = SpiStorage::new();
    reset();
    std::mem::forget(FORGOTTEN.init(Config::default()).unwrap());
    assert_eq!(FORGOTTEN.init(Config::default()).unwrap_err(), Error::Busy);
    assert_eq!(
        (stat(INIT), stat(OPEN), stat(CLOSE), stat(DEINIT)),
        (1, 1, 0, 0)
    );
    assert_eq!(
        stat(ALLOC),
        0,
        "intentionally retained resource has permanently rooted backing"
    );
}

#[test]
fn simultaneous_static_claims_admit_exactly_one_owner() {
    let _serial = SERIAL.lock().unwrap();
    static SHARED: SpiStorage = SpiStorage::new();
    reset();
    let barrier = Arc::new(Barrier::new(8));
    let results: Vec<_> = (0..8)
        .map(|_| {
            let barrier = barrier.clone();
            std::thread::spawn(move || {
                barrier.wait();
                let result = SHARED.init(Config::default());
                let success = result.is_ok();
                barrier.wait();
                drop(result);
                success
            })
        })
        .collect();
    let count = results
        .into_iter()
        .map(|thread| usize::from(thread.join().unwrap()))
        .sum::<usize>();
    assert_eq!(count, 1);
    assert_eq!(
        (stat(INIT), stat(OPEN), stat(CLOSE), stat(DEINIT)),
        (1, 1, 1, 1)
    );
    drop(SHARED.init(Config::default()).unwrap());
}

#[test]
fn native_deinit_finishes_before_static_claim_is_released() {
    let _serial = SERIAL.lock().unwrap();
    static SHARED: SpiStorage = SpiStorage::new();
    reset();
    unsafe { spi_test_gate_start() };
    let worker = std::thread::spawn(|| {
        drop(SHARED.init(Config::default()).unwrap());
    });
    unsafe { spi_test_gate_wait() };
    let result = SHARED.init(Config::default());
    let native_init = stat(INIT);
    unsafe { spi_test_gate_release() };
    worker.join().unwrap();
    let error = match result {
        Err(error) => Some(error),
        Ok(unexpected) => {
            std::mem::forget(unexpected);
            None
        }
    };
    assert_eq!(
        error,
        Some(Error::Busy),
        "claim must remain occupied through native cleanup"
    );
    assert_eq!(native_init, 1, "no C backing access during cleanup");
    drop(SHARED.init(Config::default()).unwrap());
}

#[test]
fn unsafe_raw_wrapper_is_nonowning_and_real_c_rejects_malformed_words() {
    let _serial = SERIAL.lock().unwrap();
    reset();
    let raw = unsafe { spi_test_raw(16) };
    assert!(!raw.is_null());
    {
        let mut borrowed = unsafe { Spi16::from_handle(raw.cast()) };
        borrowed.write(None, &[1, 2], TIMEOUT).unwrap();
    }
    assert_eq!(
        (stat(CLOSE), stat(DEINIT)),
        (0, 0),
        "raw Drop cannot destroy externally owned storage"
    );
    reset();
    let odd = unsafe { spi_test_malformed(raw, 0) };
    let misaligned = unsafe { spi_test_malformed(raw, 1) };
    unsafe { spi_test_raw_close() };
    assert_eq!((odd, misaligned), (-2, -2));
    check_trace("");
    assert_eq!((stat(CLOSE), stat(DEINIT)), (1, 1));
}

#[test]
fn actual_generated_layout_matches_native_and_transparent_descriptors() {
    let _serial = SERIAL.lock().unwrap();
    use ove::ffi::{ove_spi_storage_t as Storage, ove_spi_xfer as Record};
    let native = |field| unsafe { spi_test_layout(field) };
    assert_eq!(size_of::<Record>(), native(0));
    assert_eq!(align_of::<Record>(), native(1));
    assert_eq!(size_of::<Storage>(), native(2));
    assert_eq!(align_of::<Storage>(), native(3));
    assert_eq!(core::mem::offset_of!(Record, tx), native(4));
    assert_eq!(core::mem::offset_of!(Record, rx), native(5));
    assert_eq!(core::mem::offset_of!(Record, len), native(6));
    assert_eq!(size_of::<Transfer<'_, u8>>(), 2 * native(0));
    assert_eq!(size_of::<Transfer<'_, u16>>(), 2 * native(0));
    assert_eq!(align_of::<Transfer<'_, u8>>(), native(1));
    assert_eq!(align_of::<Transfer<'_, u16>>(), native(1));
    println!(
        "owner={} storage={} descriptor={} native_storage={}",
        size_of::<Spi>(),
        size_of::<SpiStorage>(),
        size_of::<Transfer<'_, u8>>(),
        native(2)
    );
}
