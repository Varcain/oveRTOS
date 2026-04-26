/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Hardware-only checks for the STM32F746G-Discovery test target.
 *
 * Each test here is gated on `OVE_HW && OVE_RENODE_STM32F746` (i.e.
 * the FreeRTOS HW build).  On Renode/QEMU/stub the runner emits a
 * single skip line and returns 0 — these checks fail in interesting
 * ways only when run against actual silicon.
 *
 * Coverage:
 *   1. Wall-clock accuracy of `ove_thread_sleep_ms` — verifies
 *      SysTick @ HCLK/configTICK_RATE_HZ is calibrated correctly.
 *      Renode's virtual time is fake, so PLL/prescaler bugs are
 *      undetectable there.
 *   2. DWT cycle counter actually advances — `freertos_time.c::
 *      ove_time_delay_us` busy-waits on `DWT->CYCCNT`, which
 *      Renode 1.16's STM32F7 model leaves stuck at 0.  Real silicon
 *      should advance the counter at HCLK rate.
 *   3. IWDG actually resets the MCU on starvation — runs FIRST in
 *      this suite so the first-boot path arms the watchdog before
 *      any other tests print PASSED/FAILED frames; on the second
 *      boot we detect `RCC->CSR.IWDGRSTF` and pass cleanly.  Renode
 *      doesn't model the IWDG at all, so this whole code path is
 *      invisible there.
 */

#include "../framework/ove_test.h"

#if defined(OVE_HW) && defined(OVE_RENODE_STM32F746)

#include "stm32f7xx_hal.h"
#include "ove/time.h"
#include "ove/thread.h"
#include "ove/watchdog.h"

/* ── 1. IWDG actually resets the MCU ──────────────────────────────── */

/* This test runs FIRST in the suite (and the suite runs FIRST in
 * suites.inc) on purpose.  On boot #1 the test arms a 1s IWDG and
 * busy-waits 2.5s — the MCU resets before anything else has had a
 * chance to print a CMocka summary.  On boot #2 RCC->CSR.IWDGRSTF
 * is set, we clear it and the test passes immediately, then the
 * remaining suites run normally.  The host runner exits on the
 * second boot's `=== Summary:` line; only one set of CMocka
 * counters is parsed. */
static void test_hw_watchdog_real_reset(void **state)
{
	(void)state;

	if (RCC->CSR & RCC_CSR_IWDGRSTF) {
		RCC->CSR |= RCC_CSR_RMVF;  /* clear all reset flags */
		return;
	}

	static ove_watchdog_storage_t storage;
	ove_watchdog_t wdt = NULL;
	int rc = ove_watchdog_init(&wdt, &storage, 1000);
	assert_int_equal(rc, OVE_OK);
	rc = ove_watchdog_start(wdt);
	assert_int_equal(rc, OVE_OK);

	/* Busy-wait long enough that even with conservative IWDG drift
	 * (LSI ±50 % over temperature) the dog has bitten.  Use
	 * HAL_GetTick rather than vTaskDelay so the IDLE task can't
	 * inadvertently call anything that kicks the dog. */
	uint32_t start = HAL_GetTick();
	while ((uint32_t)(HAL_GetTick() - start) < 2500U) {
		__NOP();
	}

	fail_msg("IWDG did not reset MCU within 2.5 s after a 1 s timeout");
}

/* ── 2. ove_thread_sleep_ms wall-clock accuracy ───────────────────── */

static void test_hw_time_sleep_accuracy(void **state)
{
	(void)state;
	uint64_t t0_us = 0, t1_us = 0;

	int rc = ove_time_get_us(&t0_us);
	assert_int_equal(rc, OVE_OK);
	ove_thread_sleep_ms(1000);
	rc = ove_time_get_us(&t1_us);
	assert_int_equal(rc, OVE_OK);

	int64_t delta_us = (int64_t)(t1_us - t0_us);

	/* ±1 % drift = 990–1010 ms.  HCLK=216 MHz / configTICK_RATE_HZ=1 kHz
	 * gives 216 000 cycles per tick; a real PLL miscompute (e.g. wrong
	 * HSE, missing PLLM divider) would land far outside this band. */
	assert_in_range(delta_us, 990000, 1010000);
}

/* ── 3. DWT cycle counter advances ────────────────────────────────── */

static void test_hw_dwt_cycle_counter(void **state)
{
	(void)state;

	/* sim_main.c::main() already enables CoreDebug DEMCR.TRCENA and
	 * DWT CTRL.CYCCNTENA; this test just samples the counter. */
	uint32_t cyc0 = DWT->CYCCNT;
	for (volatile int i = 0; i < 10000; ++i) {
		__NOP();
	}
	uint32_t cyc1 = DWT->CYCCNT;
	uint32_t delta = cyc1 - cyc0;

	/* 10 000 NOP iterations on a 216 MHz core cost roughly 30 000
	 * cycles.  Allow a generous lower bound (5 000) to absorb
	 * compiler / pipeline variability; the failure mode we care
	 * about is `delta == 0`, which is what Renode produces. */
	assert_int_not_equal(delta, 0);
	assert_true(delta > 5000);
}

#endif /* OVE_HW && OVE_RENODE_STM32F746 */

/* ── Runner ────────────────────────────────────────────────────────── */

int test_hw_stm32f746_run(void)
{
#if !(defined(OVE_HW) && defined(OVE_RENODE_STM32F746))
	printf("  [SKIP] hw_stm32f746 — non-HW or non-FreeRTOS-STM32F7 target\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		/* Watchdog test must run FIRST — see comment above its
		 * definition for the reset-and-resume protocol. */
		cmocka_unit_test(test_hw_watchdog_real_reset),
		cmocka_unit_test(test_hw_time_sleep_accuracy),
		cmocka_unit_test(test_hw_dwt_cycle_counter),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
