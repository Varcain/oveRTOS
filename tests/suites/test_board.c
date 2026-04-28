/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "../framework/ove_test.h"
#include "board_desc.h"

static void test_board_init(void **state)
{
	(void)state;
	int rc = ove_board_init();
	assert_int_equal(rc, OVE_OK);
}

static void test_board_name(void **state)
{
	(void)state;
	ove_board_init();
	const char *name = ove_board_name();
	assert_non_null(name);
}

static void test_board_desc(void **state)
{
	(void)state;
	ove_board_init();
	const struct ove_board_desc *desc = ove_board_desc();
	assert_non_null(desc);
	assert_non_null(desc->name);
	assert_int_equal(desc->gpio_port_count, OVE_GPIO_PORT_COUNT);
	assert_int_equal(desc->gpio_pins_per_port, OVE_GPIO_PINS_PER_PORT);
	assert_int_equal(desc->led_count, OVE_LED_COUNT);
	assert_non_null(desc->leds);
}

int test_board_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_board_init),
		cmocka_unit_test(test_board_name),
		cmocka_unit_test(test_board_desc),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
