/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/irq.h"
#include <stdio.h>
#include <string.h>

static unsigned failures, checks;
static void check(int value, const char *name)
{
	++checks;
	if (!value) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
#ifdef CONFIG_OVE_RTOS_POSIX
void posix_irq_enter_isr(void) __attribute__((weak));
void posix_irq_leave_isr(void) __attribute__((weak));
#else
static bool inside;
#ifdef CONFIG_OVE_RTOS_FREERTOS
#include "FreeRTOS.h"
BaseType_t xPortIsInsideInterrupt(void)
{
	return inside;
}
#elif defined(CONFIG_OVE_RTOS_NUTTX)
bool up_interrupt_context(void)
{
	return inside;
}
#else
bool k_is_in_isr(void)
{
	return inside;
}
#endif
#endif

int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "canary")))
		return 2;
	if (argc == 2)
		check(0, "early failure survives controls");
	check(!ove_is_in_isr(), "ordinary context observed");
#ifdef CONFIG_OVE_RTOS_POSIX
	check(ove_sync_context() == 7, "queue-only task resource query available");
	bool have_hooks = posix_irq_enter_isr && posix_irq_leave_isr;
	check(have_hooks, "queue-only simulator observation hooks available");
	if (have_hooks) {
		posix_irq_enter_isr();
		check(ove_is_in_isr(), "queue-only interrupt context observed");
		check(ove_sync_context() == 0, "queue-only interrupt resource rejection");
		posix_irq_leave_isr();
	} else {
		check(0, "queue-only interrupt context observed");
		check(0, "queue-only interrupt resource rejection");
	}
#else
	/* Host SDK observers deliberately use an unsupported architecture domain. */
	check(ove_sync_context() == 0, "unsupported resource domain stays inert");
	inside = true;
	check(ove_is_in_isr(), "queue-only interrupt context observed");
	check(ove_sync_context() == 0, "interrupt resource domain stays inert");
	inside = false;
#endif
	check(!ove_is_in_isr(), "ordinary context restored");
	printf("queue context: checks=%u failures=%u\n", checks, failures);
	return failures != 0;
}
