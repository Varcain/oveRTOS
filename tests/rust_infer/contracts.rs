// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
use ove::error::Error;
use ove::infer::{ModelConfig, ModelStorage};

unsafe extern "C" {
    fn infer_test_select(value: u32);
    fn infer_test_interrupt(value: bool);
    fn infer_test_failure(value: u32);
    fn infer_test_count(index: u32) -> u32;
    fn infer_test_occupied() -> bool;
    fn infer_test_capacity() -> usize;
}
ove::model_data!(
    model_bytes,
    infer_fixture_model_data,
    infer_fixture_model_data_len
);

fn config(capacity: usize) -> ModelConfig<'static> {
    // SAFETY: The fixture exports an aligned initialized immutable 64-byte array
    // with the matching length. Its synthetic setup has no parser or kernels;
    // every capacity used here either fits its bounded writes or is rejected.
    unsafe { ModelConfig::from_trusted_model(model_bytes(), capacity).unwrap() }
}
fn select(value: u32) {
    // SAFETY: The fixture setter only changes an atomic metadata selector.
    unsafe { infer_test_select(value) }
}
fn interrupt(value: bool) {
    // SAFETY: This is an atomic context simulation, without a hardware ISR.
    unsafe { infer_test_interrupt(value) }
}
fn count(index: u32) -> u32 {
    // SAFETY: The fixture reads a bounded atomic counter.
    unsafe { infer_test_count(index) }
}
fn occupied() -> bool {
    // SAFETY: The fixture reads its boolean admission flag without a handle.
    unsafe { infer_test_occupied() }
}
fn fail(value: u32) {
    // SAFETY: This only changes the fixture's ordinary error selector.
    unsafe { infer_test_failure(value) }
}

fn main() {
    let case = std::env::args().nth(1).expect("one isolated contract case");
    let cfg = config(128);
    let mut storage = ModelStorage::<128>::new();
    match case.as_str() {
        "small_storage" => {
            let mut small = ModelStorage::<64>::new();
            // Independent pre-write guard: if a broken binding enters native
            // init, it returns NoMemory before touching this smaller arena.
            fail(1);
            assert!(matches!(small.load(&cfg), Err(Error::InvalidParam)));
            assert_eq!(count(0), 0);
            assert_eq!(count(7), 0);
        }
        "qualified_capacity" => {
            let mut larger = ModelStorage::<256>::new();
            let model = larger.load(&cfg).unwrap();
            // SAFETY: Reads an atomic setup observation, without a handle.
            assert_eq!(unsafe { infer_test_capacity() }, 128);
            drop(model);
        }
        "construct_isr" => {
            interrupt(true);
            assert!(matches!(storage.load(&cfg), Err(Error::InvalidParam)));
            assert_eq!(count(0), 0);
            assert_eq!(count(7), 0);
            assert!(!occupied());
        }
        "setup_error" => {
            fail(1);
            assert!(matches!(storage.load(&cfg), Err(Error::NoMemory)));
            assert!(!occupied());
            fail(0);
            drop(storage.load(&cfg).unwrap());
            assert_eq!(count(2), 1);
        }
        #[cfg(not(zero_heap))]
        "heap" => {
            let model = ove::infer::Model::new(&config(128)).unwrap();
            assert_eq!(model.arena_used(), 64);
            assert!(occupied());
            drop(model);
            assert_eq!((count(1), count(3), count(4), count(5)), (1, 1, 1, 1));
            assert!(!occupied());
        }
        #[cfg(not(zero_heap))]
        "heap_drop_isr" | "heap_forget" => {
            let model = ove::infer::Model::new(&cfg).unwrap();
            if case == "heap_drop_isr" {
                interrupt(true);
                drop(model);
                interrupt(false);
            } else {
                std::mem::forget(model);
            }
            assert_eq!((count(2), count(3), count(4), count(5)), (0, 0, 1, 0));
            assert!(occupied());
            assert!(matches!(storage.load(&cfg), Err(Error::Busy)));
            assert_eq!(count(7), 1);
        }
        #[cfg(not(zero_heap))]
        "heap_construct_isr" => {
            interrupt(true);
            assert!(matches!(
                ove::infer::Model::new(&cfg),
                Err(Error::InvalidParam)
            ));
            assert_eq!((count(1), count(4), count(7)), (0, 0, 0));
            assert!(!occupied());
        }
        _ => {
            let mut model = storage.load(&cfg).unwrap();
            match case.as_str() {
                "normal" => {
                    assert_eq!(model.input(0).unwrap().data as usize % 16, 0);
                    assert_eq!(model.input_slice_mut::<i32>(0).unwrap(), [0; 4]);
                    model
                        .input_slice_mut::<i32>(0)
                        .unwrap()
                        .copy_from_slice(&[1, 2, 3, 4]);
                    // SAFETY: The fixture only copies initialized, in-bounds bytes.
                    unsafe { model.invoke().unwrap() };
                    assert_eq!(model.output_slice::<i32>(0).unwrap(), [1, 2, 3, 4]);
                    assert_eq!(model.last_inference_us(), 7);
                    assert_eq!(model.arena_used(), 64);
                }
                "wrong_type" => {
                    select(1);
                    assert!(matches!(
                        model.input_slice_mut::<i32>(0),
                        Err(Error::InvalidParam)
                    ));
                    assert_eq!(model.input_slice_mut::<i8>(0).unwrap().len(), 16);
                }
                "partial" => {
                    select(2);
                    assert!(matches!(
                        model.input_slice_mut::<i32>(0),
                        Err(Error::InvalidParam)
                    ));
                }
                "outside" | "constant_input" | "null" | "misaligned" | "too_large" | "straddle" => {
                    let variant = match case.as_str() {
                        "outside" => 3,
                        "constant_input" => 5,
                        "null" => 7,
                        "misaligned" => 8,
                        "too_large" => 9,
                        _ => 10,
                    };
                    select(variant);
                    assert!(matches!(
                        model.input_slice_mut::<i32>(0),
                        Err(Error::InvalidParam)
                    ));
                    if case != "constant_input" {
                        assert!(matches!(
                            model.output_slice::<i32>(0),
                            Err(Error::InvalidParam)
                        ));
                    }
                }
                "constant_output" => {
                    select(4);
                    assert_eq!(model.output_slice::<i32>(0).unwrap(), [0; 4]);
                }
                "empty" => {
                    select(6);
                    assert!(model.input_slice_mut::<i32>(0).unwrap().is_empty());
                    assert!(model.output_slice::<i32>(0).unwrap().is_empty());
                }
                "invoke_error" => {
                    fail(2);
                    // SAFETY: Even its ordinary error path performs the same
                    // bounded initialized copy and leaves all backing valid.
                    assert_eq!(unsafe { model.invoke() }, Err(Error::MlFailed));
                    assert_eq!(model.output_slice::<i32>(0).unwrap(), [0; 4]);
                }
                "invoke_isr" => {
                    interrupt(true);
                    // SAFETY: The same bounded fixture program is qualified;
                    // the binding must reject context before native invocation.
                    assert_eq!(unsafe { model.invoke() }, Err(Error::InvalidParam));
                    assert_eq!(count(6), 0);
                    interrupt(false);
                }
                "shared_readers" => {
                    std::thread::scope(|s| {
                        for _ in 0..4 {
                            let shared = &model;
                            s.spawn(move || {
                                assert_eq!(shared.output_slice::<i32>(0).unwrap(), [0; 4]);
                                assert_eq!(shared.arena_used(), 64);
                            });
                        }
                    });
                }
                "drop_isr" | "forget" => {
                    if case == "drop_isr" {
                        interrupt(true);
                        drop(model);
                        interrupt(false);
                    } else {
                        std::mem::forget(model);
                    }
                    assert_eq!((count(2), count(3), count(5)), (0, 0, 0));
                    assert!(occupied());
                    // No fixture operation can autonomously access old backing.
                    // A later claim checks only admission and must leave it alone.
                    drop(storage);
                    let mut next = ModelStorage::<128>::new();
                    assert!(matches!(next.load(&cfg), Err(Error::Busy)));
                    assert_eq!(count(7), 1);
                    println!("{case}: retained admission without old backing access");
                    return;
                }
                _ => panic!("unexpected case: {case}"),
            }
            drop(model);
            assert_eq!((count(2), count(3)), (1, 0));
            assert!(!occupied());
            drop(storage.load(&cfg).unwrap());
            assert_eq!(count(2), 2);
        }
    }
    println!("{case}: pass");
}
