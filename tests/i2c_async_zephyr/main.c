/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/hal/hal_i2c.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_ARM
#include "framework/semihosting_exit.h"
#define test_exit semihosting_exit
#else
#define test_exit exit
#endif

static ove_i2c_storage_t storage;
static ove_i2c_t i2c;
static unsigned char tx[4] = {1, 2, 3, 4}, rx[4];
static atomic_t callbacks, closes, callback_running, gate_enabled, resubmit_error, metadata_errors;
static int resubmit;
static atomic_t submit_gate_enabled, submit_result;
K_SEM_DEFINE(entered, 0, 1);
K_SEM_DEFINE(release_gate, 0, 1);
K_SEM_DEFINE(done, 0, 1);
K_SEM_DEFINE(submit_entered, 0, 1);
K_SEM_DEFINE(submit_release, 0, 1);
K_SEM_DEFINE(submit_done, 0, 1);

static int test_configure(const struct device *device, uint32_t config)
{
	(void)device;
	(void)config;
	return 0;
}
static int test_transfer(const struct device *device, struct i2c_msg *messages, uint8_t count,
			 uint16_t addr)
{
	(void)device;
	(void)addr;
	const uint8_t *write = NULL;
	size_t write_len = 0;
	for (uint8_t i = 0; i < count; ++i) {
		if (messages[i].flags & I2C_MSG_READ) {
			memset(messages[i].buf, 0, messages[i].len);
			if (write)
				memcpy(messages[i].buf, write,
				       write_len < messages[i].len ? write_len : messages[i].len);
		} else {
			write = messages[i].buf;
			write_len = messages[i].len;
		}
	}
	return 0;
}
static const struct i2c_driver_api test_api = {.configure = test_configure,
					       .transfer = test_transfer};
DEVICE_DT_DEFINE(DT_NODELABEL(i2c1), NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,
		 &test_api);
void __real_ove_i2c_async_complete(ove_i2c_t handle, int result);
void __wrap_ove_i2c_async_complete(ove_i2c_t handle, int result)
{
	__real_ove_i2c_async_complete(handle, result);
	if (atomic_get(&gate_enabled)) {
		k_sem_give(&entered);
		(void)k_sem_take(&release_gate, K_FOREVER);
	}
}
void __real_ove_hal_i2c_close(ove_i2c_t handle);
void __wrap_ove_hal_i2c_close(ove_i2c_t handle)
{
	atomic_inc(&closes);
	__real_ove_hal_i2c_close(handle);
}
int __real_ove_hal_i2c_write_read_async(ove_i2c_t handle, uint16_t addr, const void *write,
					size_t tx_len, void *read, size_t rx_len);
int __wrap_ove_hal_i2c_write_read_async(ove_i2c_t handle, uint16_t addr, const void *write,
					size_t tx_len, void *read, size_t rx_len)
{
	int rc = __real_ove_hal_i2c_write_read_async(handle, addr, write, tx_len, read, rx_len);
	if (atomic_get(&submit_gate_enabled)) {
		k_sem_give(&submit_entered);
		(void)k_sem_take(&submit_release, K_FOREVER);
	}
	return rc;
}
static void completed(int result, void *context)
{
	(void)context;
	if (storage.pending_tx || storage.pending_rx || storage.pending_tx_len ||
	    storage.pending_rx_len || storage.pending_addr || storage.pending_cb ||
	    storage.pending_ud)
		atomic_inc(&metadata_errors);
	if (result != OVE_OK)
		atomic_set(&resubmit_error, result);
	if (k_work_busy_get(&storage.async_work) & K_WORK_RUNNING)
		atomic_inc(&callback_running);
	unsigned count = (unsigned)atomic_inc(&callbacks) + 1;
	if (resubmit && count == 1) {
		int rc = OVE_OK;
		if (resubmit == 2) {
			ove_i2c_deinit(i2c);
			struct ove_i2c_cfg cfg = {.speed = OVE_I2C_SPEED_FAST};
			rc = ove_i2c_init(&i2c, &storage, &cfg);
		}
		if (rc == OVE_OK)
			rc = ove_i2c_write_read_async(i2c, 0x42, tx, sizeof(tx), rx, sizeof(rx),
						      completed, NULL);
		atomic_set(&resubmit_error, rc);
		if (rc != OVE_OK)
			k_sem_give(&done);
	} else {
		k_sem_give(&done);
	}
}
static void submit_once(void *one, void *two, void *three)
{
	(void)one;
	(void)two;
	(void)three;
	int rc = ove_i2c_write_read_async(i2c, 0x42, tx, sizeof(tx), rx, sizeof(rx), completed,
					  NULL);
	atomic_set(&submit_result, rc);
	k_sem_give(&submit_done);
}
K_THREAD_DEFINE(submitter, 2048, submit_once, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0,
		SYS_FOREVER_MS);
static int check(int value, const char *message)
{
	if (!value) {
		printf("FAIL: %s\n", message);
		return 1;
	}
	return 0;
}
static int run_case(int close_pending, int again)
{
	struct ove_i2c_cfg cfg = {.speed = OVE_I2C_SPEED_FAST};
	atomic_clear(&callbacks);
	atomic_clear(&closes);
	atomic_clear(&callback_running);
	atomic_clear(&resubmit_error);
	atomic_clear(&metadata_errors);
	atomic_set(&gate_enabled, 1);
	resubmit = again;
	k_sem_reset(&entered);
	k_sem_reset(&release_gate);
	k_sem_reset(&done);
	int rc = ove_i2c_init(&i2c, &storage, &cfg);
	if (check(rc == OVE_OK, "actual Zephyr I2C initialization"))
		test_exit(1);
	rc = ove_i2c_write_read_async(i2c, 0x42, tx, sizeof(tx), rx, sizeof(rx), completed, NULL);
	if (check(rc == OVE_OK, "actual Zephyr async submission"))
		test_exit(1);
	if (check(k_sem_take(&entered, K_SECONDS(2)) == 0, "completion dependency gate"))
		test_exit(1);
	unsigned early = (unsigned)atomic_get(&callbacks);
	if (close_pending)
		ove_i2c_deinit(i2c);
	unsigned early_close = (unsigned)atomic_get(&closes);
	atomic_clear(&gate_enabled);
	k_sem_give(&release_gate);
	int waited = k_sem_take(&done, K_SECONDS(2));
	/* Parent callbacks run in the handler. Join its actual kernel tail before
	 * cleanup/reinitialization rather than depending on scheduler priorities. */
	struct k_work_sync sync;
	(void)k_work_flush(&storage.async_work, &sync);
	if (check(waited == 0, "retirement before backing reuse"))
		test_exit(1);
	if (!close_pending)
		ove_i2c_deinit(i2c);
	int failures = check(waited == 0, "final callback delivered");
	failures += check(early == 0, "notification waits for provider/kernel tail");
	failures += check(early_close == 0, "pending close waits for provider/kernel tail");
	failures +=
		check(atomic_get(&callback_running) == 0, "kernel work still running at callback");
	failures += check(atomic_get(&callbacks) == (again ? 2 : 1), "exact callback count");
	failures += check(atomic_get(&closes) == (again == 2 ? 2 : 1), "exact native close count");
	failures += check(atomic_get(&resubmit_error) == 0, "retired callback can resubmit");
	failures += check(memcmp(tx, rx, sizeof(tx)) == 0, "actual transfer data");
	failures += check(atomic_get(&metadata_errors) == 0,
			  "provider metadata clears before notification");
	return failures;
}
static int queue_plug_case(void)
{
	atomic_clear(&callbacks);
	atomic_clear(&closes);
	atomic_clear(&callback_running);
	atomic_clear(&resubmit_error);
	atomic_clear(&metadata_errors);
	atomic_clear(&gate_enabled);
	atomic_set(&submit_gate_enabled, 1);
	resubmit = 0;
	k_sem_reset(&done);
	struct ove_i2c_cfg cfg = {.speed = OVE_I2C_SPEED_FAST};
	if (check(ove_i2c_init(&i2c, &storage, &cfg) == OVE_OK, "plug test initialization"))
		test_exit(1);
	k_thread_start(submitter);
	if (check(k_sem_take(&submit_entered, K_SECONDS(2)) == 0, "submitter last-access gate"))
		test_exit(1);
	int drained = k_work_queue_drain(&k_sys_work_q, true);
	k_sem_give(&submit_release);
	int submitted = k_sem_take(&submit_done, K_SECONDS(2));
	k_sleep(K_MSEC(25));
	unsigned early = (unsigned)atomic_get(&callbacks);
	ove_i2c_deinit(i2c);
	unsigned early_close = (unsigned)atomic_get(&closes);
	int unplugged = k_work_queue_unplug(&k_sys_work_q);
	int finished = k_sem_take(&done, K_SECONDS(2));
	struct k_work_sync sync;
	(void)k_work_flush(&storage.async_work, &sync);
	if (check(finished == 0, "retry retirement before backing reuse"))
		test_exit(1);
	int failures = check(drained >= 0 && unplugged == 0, "real system queue plug/unplug");
	failures += check(submitted == 0 && atomic_get(&submit_result) == OVE_OK,
			  "accepted operation after held submitter");
	failures += check(early == 0 && early_close == 0, "plugged queue retains callback/backing");
	failures += check(finished == 0 && atomic_get(&callbacks) == 1,
			  "periodic retry recovers rejected retirement dispatch");
	failures += check(atomic_get(&closes) == 1, "retry finishes pending close exactly once");
	failures += check(atomic_get(&callback_running) == 0, "retry follows actual kernel tail");
	failures += check(atomic_get(&metadata_errors) == 0,
			  "provider metadata clears before notification");
	return failures;
}
int main(void)
{
	int failures = run_case(0, 0) + run_case(1, 0);
#ifndef OVE_I2C_CAPTURE_PARENT
	failures += run_case(0, 1);
	failures += run_case(0, 2);
#endif
	failures += queue_plug_case();
	size_t stack_free = 0;
	failures += check(k_thread_stack_space_get(&k_sys_work_q.thread, &stack_free) == 0,
			  "actual system workqueue stack accounting");
#ifdef CONFIG_ARM
	printf("ARM system workqueue stack: %zu bytes free of %u\n", stack_free,
	       CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE);
#else
	printf("Simulated stack accounting (host pthread execution): %zu of %u\n", stack_free,
	       CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE);
#endif
	printf("Zephyr I2C retirement: %d failed assertion(s)\n", failures);
	test_exit(failures ? 1 : 0);
}
