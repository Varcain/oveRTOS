/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/hal/hal_spi.h"
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ove_spi_storage_t storage;
static ove_spi_t spi;
static unsigned char tx[4] = {1, 2, 3, 4}, rx[4];
static atomic_t callbacks, closes, callback_running, gate_enabled, resubmit_error;
static int resubmit;
static atomic_t submit_gate_enabled, submit_result;
K_SEM_DEFINE(entered, 0, 1);
K_SEM_DEFINE(release_gate, 0, 1);
K_SEM_DEFINE(done, 0, 1);
K_SEM_DEFINE(submit_entered, 0, 1);
K_SEM_DEFINE(submit_release, 0, 1);
K_SEM_DEFINE(submit_done, 0, 1);

static int test_transceive(const struct device *device, const struct spi_config *config,
			   const struct spi_buf_set *write, const struct spi_buf_set *read)
{
	(void)device;
	(void)config;
	if (write->count && read->count)
		memcpy(read->buffers[0].buf, write->buffers[0].buf, read->buffers[0].len);
	return 0;
}
static const struct spi_driver_api test_api = {.transceive = test_transceive};
DEVICE_DT_DEFINE(DT_NODELABEL(spi1), NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,
		 &test_api);

int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	(void)port;
	(void)pin;
	(void)value;
	return OVE_OK;
}
void __real_ove_spi_async_complete(ove_spi_t handle, int result);
void __wrap_ove_spi_async_complete(ove_spi_t handle, int result)
{
	__real_ove_spi_async_complete(handle, result);
	if (atomic_get(&gate_enabled)) {
		k_sem_give(&entered);
		(void)k_sem_take(&release_gate, K_FOREVER);
	}
}
void __real_ove_hal_spi_close(ove_spi_t handle);
void __wrap_ove_hal_spi_close(ove_spi_t handle)
{
	atomic_inc(&closes);
	__real_ove_hal_spi_close(handle);
}
int __real_ove_hal_spi_transfer_async(ove_spi_t handle, const void *write, void *read, size_t len);
int __wrap_ove_hal_spi_transfer_async(ove_spi_t handle, const void *write, void *read, size_t len)
{
	int rc = __real_ove_hal_spi_transfer_async(handle, write, read, len);
	if (atomic_get(&submit_gate_enabled)) {
		k_sem_give(&submit_entered);
		(void)k_sem_take(&submit_release, K_FOREVER);
	}
	return rc;
}
static void completed(int result, void *context)
{
	(void)context;
	if (result != OVE_OK)
		atomic_set(&resubmit_error, result);
	if (k_work_busy_get(&storage.async_work) & K_WORK_RUNNING)
		atomic_inc(&callback_running);
	unsigned count = (unsigned)atomic_inc(&callbacks) + 1;
	if (resubmit && count == 1) {
		int rc = ove_spi_transfer_async(spi, NULL, tx, rx, sizeof(tx), completed, NULL);
		atomic_set(&resubmit_error, rc);
	} else {
		k_sem_give(&done);
	}
}
static void submit_once(void *one, void *two, void *three)
{
	(void)one;
	(void)two;
	(void)three;
	int rc = ove_spi_transfer_async(spi, NULL, tx, rx, sizeof(tx), completed, NULL);
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
	struct ove_spi_cfg cfg = {.word_size = 8, .clock_hz = 1000000};
	atomic_clear(&callbacks);
	atomic_clear(&closes);
	atomic_clear(&callback_running);
	atomic_clear(&resubmit_error);
	atomic_set(&gate_enabled, 1);
	resubmit = again;
	k_sem_reset(&entered);
	k_sem_reset(&release_gate);
	k_sem_reset(&done);
	int rc = ove_spi_init(&spi, &storage, &cfg);
	if (check(rc == OVE_OK, "actual Zephyr SPI initialization"))
		return 1;
	rc = ove_spi_transfer_async(spi, NULL, tx, rx, sizeof(tx), completed, NULL);
	if (check(rc == OVE_OK, "actual Zephyr async submission"))
		return 1;
	if (check(k_sem_take(&entered, K_SECONDS(2)) == 0, "completion dependency gate"))
		return 1;
	unsigned early = (unsigned)atomic_get(&callbacks);
	if (close_pending)
		ove_spi_deinit(spi);
	unsigned early_close = (unsigned)atomic_get(&closes);
	atomic_clear(&gate_enabled);
	k_sem_give(&release_gate);
	int waited = k_sem_take(&done, K_SECONDS(2));
	/* Parent callbacks run in the handler. Join its actual kernel tail before
	 * cleanup/reinitialization rather than depending on scheduler priorities. */
	struct k_work_sync sync;
	(void)k_work_flush(&storage.async_work, &sync);
	if (!close_pending)
		ove_spi_deinit(spi);
	int failures = check(waited == 0, "final callback delivered");
	failures += check(early == 0, "notification waits for provider/kernel tail");
	failures += check(early_close == 0, "pending close waits for provider/kernel tail");
	failures +=
		check(atomic_get(&callback_running) == 0, "kernel work still running at callback");
	failures += check(atomic_get(&callbacks) == (again ? 2 : 1), "exact callback count");
	failures += check(atomic_get(&closes) == 1, "exact native close count");
	failures += check(atomic_get(&resubmit_error) == 0, "retired callback can resubmit");
	failures += check(memcmp(tx, rx, sizeof(tx)) == 0, "actual transfer data");
	return failures;
}
static int queue_plug_case(void)
{
	atomic_clear(&callbacks);
	atomic_clear(&closes);
	atomic_clear(&callback_running);
	atomic_clear(&resubmit_error);
	atomic_clear(&gate_enabled);
	atomic_set(&submit_gate_enabled, 1);
	resubmit = 0;
	k_sem_reset(&done);
	struct ove_spi_cfg cfg = {.word_size = 8, .clock_hz = 1000000};
	if (check(ove_spi_init(&spi, &storage, &cfg) == OVE_OK, "plug test initialization"))
		return 1;
	k_thread_start(submitter);
	if (check(k_sem_take(&submit_entered, K_SECONDS(2)) == 0, "submitter last-access gate"))
		return 1;
	int drained = k_work_queue_drain(&k_sys_work_q, true);
	k_sem_give(&submit_release);
	int submitted = k_sem_take(&submit_done, K_SECONDS(2));
	k_sleep(K_MSEC(25));
	unsigned early = (unsigned)atomic_get(&callbacks);
	ove_spi_deinit(spi);
	unsigned early_close = (unsigned)atomic_get(&closes);
	int unplugged = k_work_queue_unplug(&k_sys_work_q);
	int finished = k_sem_take(&done, K_SECONDS(2));
	struct k_work_sync sync;
	(void)k_work_flush(&storage.async_work, &sync);
	int failures = check(drained >= 0 && unplugged == 0, "real system queue plug/unplug");
	failures += check(submitted == 0 && atomic_get(&submit_result) == OVE_OK,
			  "accepted operation after held submitter");
	failures += check(early == 0 && early_close == 0, "plugged queue retains callback/backing");
	failures += check(finished == 0 && atomic_get(&callbacks) == 1,
			  "periodic retry recovers rejected retirement dispatch");
	failures += check(atomic_get(&closes) == 1, "retry finishes pending close exactly once");
	failures += check(atomic_get(&callback_running) == 0, "retry follows actual kernel tail");
	return failures;
}
int main(void)
{
	int failures = run_case(0, 0) + run_case(1, 0);
#ifndef OVE_SPI_CAPTURE_PARENT
	failures += run_case(0, 1);
#endif
	failures += queue_plug_case();
	printf("Zephyr SPI retirement: %d failed assertion(s)\n", failures);
	exit(failures ? 1 : 0);
}
