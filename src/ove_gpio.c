/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"
#include "ove/gpio.h"
#include "ove/hal/hal_gpio.h"
#include "board_desc.h"
#include <stdatomic.h>

#define GPIO_IRQ_MAX 8

/* `registered` and `enabled` are read from ISR context by
 * `ove_gpio_irq_dispatch` and written from thread context by register /
 * enable / disable.  Marking them `volatile` keeps the compiler from
 * caching or reordering their accesses across function boundaries.
 *
 * `registered` is additionally fenced — writers release-fence after
 * filling the data fields (port/pin/callback/...) and before storing
 * registered=1, so a dispatch that observes registered=1 is guaranteed
 * to see the data fields too.
 *
 * `enabled` is volatile-but-unfenced (a single word, effectively atomic on
 * every supported target). A disable racing an already-in-flight dispatch
 * may let one more callback fire — inherent to disarming a live interrupt
 * and benign, so no stronger ordering is imposed. */
struct gpio_irq_entry {
	unsigned int port;
	unsigned int pin;
	ove_gpio_irq_mode_t mode;
	ove_gpio_irq_cb callback;
	void *user_data;
	volatile int registered;
	volatile int enabled;
};

static struct gpio_irq_entry irq_table[GPIO_IRQ_MAX];

static int validate_port_pin(unsigned int port, unsigned int pin)
{
	if (port >= OVE_GPIO_PORT_COUNT || pin >= OVE_GPIO_PINS_PER_PORT) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
{
	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}
	return ove_hal_gpio_configure(port, pin, mode);
}

int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}
	return ove_hal_gpio_set(port, pin, value);
}

int ove_gpio_get(unsigned int port, unsigned int pin)
{
	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}
	return ove_hal_gpio_get(port, pin);
}

int ove_gpio_irq_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode,
			  ove_gpio_irq_cb callback, void *user_data)
{
	unsigned int i;

	if (validate_port_pin(port, pin) != OVE_OK) {
		return OVE_ERR_INVALID_PARAM;
	}

	/* Find free slot */
	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (!irq_table[i].registered) {
			break;
		}
	}
	if (i >= GPIO_IRQ_MAX) {
		return OVE_ERR_NO_MEMORY;
	}

	irq_table[i].port = port;
	irq_table[i].pin = pin;
	irq_table[i].mode = mode;
	irq_table[i].callback = callback;
	irq_table[i].user_data = user_data;
	/* Publish the data fields before marking the slot registered so
	 * a concurrent dispatch sees a consistent entry. */
	atomic_thread_fence(memory_order_release);
	irq_table[i].registered = 1;
	irq_table[i].enabled = 0;

	return ove_hal_gpio_irq_hw_enable(port, pin, mode, callback, user_data);
}

/*
 * NB (cross-backend caveats, tracked):
 *  - `enabled` is the authoritative dispatch gate (see ove_gpio_irq_dispatch);
 *    enable/disable toggle it in software. enable() does NOT re-arm the line in
 *    hardware, so on backends whose hw_disable() actually masks the IRQ
 *    (freertos NVIC, zephyr gpio_pin_interrupt_configure) a disable→enable
 *    cycle leaves the hardware line masked even though dispatch is re-gated on.
 *    The host/posix backend (hw_disable is a no-op) relies purely on the gate,
 *    so it round-trips correctly. Re-arming HW on enable would need a
 *    hw_reenable HAL hook — deferred.
 *  - hw_disable() return codes diverge: posix OVE_OK (no-op), freertos OVE_OK,
 *    zephyr OVE_OK, nuttx OVE_ERR_NOT_SUPPORTED (GPIO IRQ masking unimplemented).
 */
int ove_gpio_irq_enable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (irq_table[i].registered && irq_table[i].port == port &&
		    irq_table[i].pin == pin) {
			irq_table[i].enabled = 1;
			return OVE_OK;
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_gpio_irq_disable(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (irq_table[i].registered && irq_table[i].port == port &&
		    irq_table[i].pin == pin) {
			irq_table[i].enabled = 0;
			return ove_hal_gpio_irq_hw_disable(port, pin);
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_gpio_irq_unregister(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (irq_table[i].registered && irq_table[i].port == port &&
		    irq_table[i].pin == pin) {
			/* Disable the line, then free the slot so the (port,pin)
			 * can be re-registered.  Clearing `enabled` before
			 * `registered` keeps a concurrent dispatch from firing a
			 * half-torn-down entry (it gates on `enabled`). */
			irq_table[i].enabled = 0;
			irq_table[i].registered = 0;
			/* Permanent teardown — hw_unregister (not hw_disable) so
			 * the backend also releases per-registration HW state
			 * (e.g. Zephyr's gpio_callback), otherwise re-registering
			 * the same (port,pin) double-registers and double-fires. */
			return ove_hal_gpio_irq_hw_unregister(port, pin);
		}
	}
	return OVE_ERR_NOT_SUPPORTED;
}

/* Called by backend HAL when a GPIO interrupt fires */
void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < GPIO_IRQ_MAX; i++) {
		if (!irq_table[i].registered)
			continue;
		/* Pair with the release fence in ove_gpio_irq_register so
		 * the data fields read below are observed in their
		 * post-registration state. */
		atomic_thread_fence(memory_order_acquire);
		if (irq_table[i].enabled && irq_table[i].port == port && irq_table[i].pin == pin) {
			if (irq_table[i].callback) {
				irq_table[i].callback(port, pin, irq_table[i].user_data);
			}
			break;
		}
	}
}
