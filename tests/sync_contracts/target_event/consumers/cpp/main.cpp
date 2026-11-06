// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ove/sync.hpp"
#include "ove/app.hpp"

static int event_binding_task_consumer()
{
	ove::Event event;
	event.signal();
	return event.try_wait_for(std::chrono::milliseconds{1}).has_value() ? 0 : 1;
}

static void event_binding_isr_consumer(ove::Event &event)
{
	event.signal_from_isr();
}

extern "C"
	__attribute__((used)) int (*const event_compile_task_anchor)() = event_binding_task_consumer;
extern "C" __attribute__((used)) void (*const event_compile_isr_anchor)(ove::Event &) =
	event_binding_isr_consumer;

extern "C" void ove_main(void)
{
	ove::run();
}
