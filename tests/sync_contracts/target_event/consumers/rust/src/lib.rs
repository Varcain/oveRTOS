// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#![no_std]

static EVENT: ove::EventStorage = ove::EventStorage::new();

fn event_binding_task_consumer() -> i32 {
    let Ok(event) = ove::Event::create(&EVENT) else {
        return 1;
    };
    event.signal();
    i32::from(
        event
            .wait_for(core::time::Duration::from_millis(1))
            .is_err(),
    )
}

fn event_binding_isr_consumer(event: &ove::Event) {
    event.signal_from_isr();
}

#[used]
#[unsafe(export_name = "event_compile_task_anchor")]
pub static EVENT_COMPILE_TASK_ANCHOR: fn() -> i32 = event_binding_task_consumer;

#[used]
#[unsafe(export_name = "event_compile_isr_anchor")]
pub static EVENT_COMPILE_ISR_ANCHOR: fn(&ove::Event) = event_binding_isr_consumer;

#[ove::main]
fn app_main() {
    ove::run();
}
