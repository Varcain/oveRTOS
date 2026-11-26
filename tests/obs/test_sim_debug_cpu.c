/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "../../sim/hal/sim_debug.c"

static struct ove_thread_info script[3];
static size_t script_count;
static uint64_t now_us;
static bool clock_failed;
static uint8_t snapshot[SNAPSHOT_BUF_SIZE];

int ove_thread_list(struct ove_thread_info *out, size_t max, size_t *count)
{
	assert_true(script_count <= max);
	memcpy(out, script, script_count * sizeof(*out));
	*count = script_count;
	return OVE_OK;
}

int ove_sys_get_mem_stats(struct ove_mem_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	return OVE_OK;
}

int ove_time_get_us(uint64_t *out)
{
	if (clock_failed)
		return OVE_ERR_NOT_SUPPORTED;
	*out = now_us;
	return OVE_OK;
}

struct ove_sim_transport *ove_sim_get_transport(void)
{
	return NULL;
}

int ove_sim_plugin_dispatch_cmd(const struct ove_sim_cmd *cmd)
{
	return OVE_OK;
}

int ove_sim_plugin_emit_event(uint32_t id, const struct ove_sim_event *event)
{
	return OVE_OK;
}

int ove_sim_plugin_register(const struct ove_sim_plugin_ops *ops, void *ctx, const void *config,
			    size_t config_len)
{
	return 1;
}

int ove_thread_create(ove_thread_t *handle, const char *name, ove_thread_fn entry, void *arg,
		      ove_prio_t prio, size_t stack_size)
{
	return OVE_OK;
}

void ove_thread_sleep_ms(uint32_t ms)
{
}

static void capture_snapshot(void)
{
	assert_true(build_snapshot(snapshot, sizeof(snapshot)) >= 17u);
	assert_int_equal(snapshot[0], script_count);
}

static const uint8_t *row_fields(size_t index)
{
	assert_true(index < snapshot[0]);
	const uint8_t *row = snapshot + 17; /* Count and heap stats. */
	for (size_t i = 0; i < index; i++)
		row += 43u + row[0];
	return row + 1u + row[0] + 2u; /* First field after the name, state and priority. */
}

static uint32_t read_u32(const uint8_t *src)
{
	uint32_t value;
	memcpy(&value, src, sizeof(value));
	return value;
}

static void assert_cpu(size_t index, bool valid, uint32_t expected)
{
	const uint8_t *fields = row_fields(index);
	assert_int_equal(read_u32(fields + 12), expected);
	assert_int_equal((read_u32(fields) & OVE_THREAD_INFO_VALID_CPU_PERCENT) != 0, valid);
}

static int setup(void **state)
{
	clock_failed = false;
	now_us = 60000000;
	script_count = 0;
	capture_snapshot(); /* Discard samples from the preceding case. */
	script_count = 1;
	script[0] = (struct ove_thread_info){
		.name = "IDLE",
		.identity = 0x1000,
		.valid_fields = OVE_THREAD_INFO_VALID_STATE_TIMES |
				OVE_THREAD_INFO_VALID_CPU_PERCENT,
		.cpu_percent_x100 = 9000,
		.state_times = {.running_us = 54000000, .ready_us = 6000000},
	};
	return 0;
}

static void test_interval_cpu(void **state)
{
	capture_snapshot();
	assert_cpu(0, false, 0);

	now_us += 500000;
	script[0].state_times.running_us += 50000;
	script[0].state_times.ready_us += 450000;
	script[0].cpu_percent_x100 = 8933;
	capture_snapshot();
	assert_cpu(0, true, 1000);
	assert_int_equal(read_u32(row_fields(0) + 16), 8933);
}

static void test_measured_interval(void **state)
{
	capture_snapshot();
	now_us += 600000;
	script[0].state_times.running_us += 60000;
	capture_snapshot();
	assert_cpu(0, true, 1000);
}

static void test_new_generation(void **state)
{
	capture_snapshot();
	now_us += 500000;
	script[0].generation++;
	script[0].state_times.running_us += 50000;
	capture_snapshot();
	assert_cpu(0, false, 0);
}

static void test_terminated_thread(void **state)
{
	strcpy(script[0].name, "worker");
	script[0].generation = 1;
	capture_snapshot();

	now_us += 500000;
	script[0].state = OVE_THREAD_STATE_TERMINATED;
	script[0].valid_fields = OVE_THREAD_INFO_VALID_RUNNING_TIME |
				 OVE_THREAD_INFO_VALID_CPU_PERCENT;
	script[0].state_times.running_us += 600000;
	capture_snapshot();
	assert_cpu(0, false, 0);
}

static void test_backwards_counter_and_clamp(void **state)
{
	capture_snapshot();
	now_us += 500000;
	script[0].state_times.running_us -= 50000;
	capture_snapshot();
	assert_cpu(0, false, 0);

	now_us += 500000;
	script[0].state_times.running_us += 600000;
	capture_snapshot();
	assert_cpu(0, true, 10000);

	now_us += 500000;
	script[0].state_times.running_us += 50000;
	capture_snapshot();
	assert_cpu(0, true, 1000);
}

static void test_reorder_and_missing_running_time(void **state)
{
	script_count = 3;
	script[1] = script[0];
	script[1].identity = 0x2000;
	strcpy(script[1].name, "worker");
	script[1].state_times.running_us = 3000000;
	script[2] = script[1];
	script[2].identity = 0x3000;
	strcpy(script[2].name, "pending");
	capture_snapshot();

	now_us += 500000;
	struct ove_thread_info idle = script[0];
	script[0] = script[1];
	script[1] = idle;
	script[0].state_times.running_us += 150000;
	script[1].state_times.running_us += 50000;
	script[2].valid_fields = OVE_THREAD_INFO_VALID_CPU_PERCENT;
	script[2].state_times.running_us = 0;
	capture_snapshot();
	assert_cpu(0, true, 3000);
	assert_cpu(1, true, 1000);
	assert_cpu(2, false, 0);

	now_us += 500000;
	script[2].valid_fields |= OVE_THREAD_INFO_VALID_RUNNING_TIME;
	script[2].state_times.running_us = 3010000;
	capture_snapshot();
	assert_cpu(2, false, 0);
}

static void test_clock_failure_and_zero_interval(void **state)
{
	capture_snapshot();
	now_us += 500000;
	clock_failed = true;
	capture_snapshot();
	assert_cpu(0, false, 0);

	clock_failed = false;
	capture_snapshot();
	assert_cpu(0, false, 0);
	capture_snapshot();
	assert_cpu(0, false, 0);

	now_us += 500000;
	capture_snapshot();
	assert_cpu(0, true, 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_interval_cpu, setup),
		cmocka_unit_test_setup(test_measured_interval, setup),
		cmocka_unit_test_setup(test_new_generation, setup),
		cmocka_unit_test_setup(test_terminated_thread, setup),
		cmocka_unit_test_setup(test_backwards_counter_and_clamp, setup),
		cmocka_unit_test_setup(test_reorder_and_missing_running_time, setup),
		cmocka_unit_test_setup(test_clock_failure_and_zero_interval, setup),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
