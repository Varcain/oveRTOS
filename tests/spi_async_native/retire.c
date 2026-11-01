/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "ove/hal/hal_spi.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* A missing retirement entrypoint is used only by the parent comparison. */
extern void ove_spi_async_retired(ove_spi_t spi) __attribute__((weak));
enum { DELAYED, INLINE_GATE, REJECT_GATE, INLINE_COMPLETE };
static unsigned mode;
static atomic_uint callbacks, closes, frees, submissions, depth, overlap, selected;
static atomic_int completion_result;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int entered, released, prepare_error;
static int resubmit, quarantine, destroy_in_callback, block_close, block_callback;
static int close_entered, close_released, callback_entered, callback_released;
static atomic_int close_returned;
static ove_spi_storage_t *reinit_storage;
static void *allocation;
static ove_spi_t active;
static unsigned char tx[4] = {1, 2, 3, 4}, rx[4];
static const struct ove_spi_cs cs = {2, 7, 1};

static void reset(void)
{
	atomic_store(&callbacks, 0);
	atomic_store(&closes, 0);
	atomic_store(&frees, 0);
	atomic_store(&submissions, 0);
	atomic_store(&depth, 0);
	atomic_store(&overlap, 0);
	atomic_store(&selected, 0);
	atomic_store(&completion_result, 0);
	entered = released = prepare_error = resubmit = 0;
	quarantine = 1;
	destroy_in_callback = block_close = block_callback = 0;
	close_entered = close_released = callback_entered = callback_released = 0;
	reinit_storage = NULL;
	atomic_store(&close_returned, 0);
	mode = DELAYED;
	allocation = NULL;
}
void *spi_test_malloc(size_t size)
{
	allocation = malloc(size);
	return allocation;
}
void spi_test_free(void *pointer)
{
	assert_ptr_equal(pointer, allocation);
	atomic_fetch_add(&frees, 1);
	/* Quarantine allows parent timing assertions without an invalid free. */
	if (!quarantine) {
		free(pointer);
		allocation = NULL;
	}
}
int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	assert_int_equal(port, 2);
	assert_int_equal(pin, 7);
	atomic_store(&selected, !value);
	return OVE_OK;
}
int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
	(void)cfg;
	spi->fd = 1;
	return OVE_OK;
}
static void wait_gate(int *entry, int *release)
{
	pthread_mutex_lock(&gate_lock);
	*entry = 1;
	pthread_cond_broadcast(&gate_cv);
	while (!*release)
		pthread_cond_wait(&gate_cv, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}
static void wait_entered(int *entry)
{
	pthread_mutex_lock(&gate_lock);
	while (!*entry)
		pthread_cond_wait(&gate_cv, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}
static void release_gate(int *release)
{
	pthread_mutex_lock(&gate_lock);
	*release = 1;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&gate_lock);
}
void ove_hal_spi_close(ove_spi_t spi)
{
	atomic_fetch_add(&closes, 1);
	if (block_close)
		wait_gate(&close_entered, &close_released);
	spi->fd = -1;
}
int ove_hal_spi_validate_transfer(ove_spi_t spi, size_t len, int asynchronous)
{
	(void)spi;
	(void)len;
	(void)asynchronous;
	return OVE_OK;
}
int ove_hal_spi_transfer(ove_spi_t spi, const void *write, void *read, size_t len, uint64_t timeout)
{
	(void)spi;
	(void)write;
	(void)read;
	(void)len;
	(void)timeout;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_spi_async_prepare(void)
{
	return prepare_error;
}
void ove_hal_spi_async_retire(ove_spi_t spi)
{
	assert_non_null(ove_spi_async_retired);
	ove_spi_async_retired(spi);
}
static void wait_in_hal(void)
{
	pthread_mutex_lock(&gate_lock);
	entered = 1;
	pthread_cond_broadcast(&gate_cv);
	while (!released)
		pthread_cond_wait(&gate_cv, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}
int ove_hal_spi_transfer_async(ove_spi_t spi, const void *write, void *read, size_t len)
{
	atomic_fetch_add(&submissions, 1);
	if (atomic_fetch_add(&depth, 1))
		atomic_fetch_add(&overlap, 1);
	if (mode == INLINE_GATE || mode == INLINE_COMPLETE) {
		if (read && write)
			memmove(read, write, len);
		ove_spi_async_complete(spi, OVE_OK);
	}
	if (mode == INLINE_GATE || mode == REJECT_GATE)
		wait_in_hal();
	atomic_fetch_sub(&depth, 1);
	return mode == REJECT_GATE ? OVE_ERR_IO : OVE_OK;
}
static void complete(int result, void *user)
{
	assert_ptr_equal(user, rx);
	if (block_callback)
		wait_gate(&callback_entered, &callback_released);
	unsigned count = atomic_fetch_add(&callbacks, 1) + 1;
	atomic_store(&completion_result, result);
	if (reinit_storage && count == 1) {
		ove_spi_deinit(active);
		struct ove_spi_cfg cfg = {.word_size = 8};
		int rc = ove_spi_init(&active, reinit_storage, &cfg);
		assert_int_equal(rc, OVE_OK);
	}
	if ((resubmit || reinit_storage) && count == 1) {
		int rc = ove_spi_transfer_async(active, &cs, tx, rx, sizeof(tx), complete, rx);
		assert_int_equal(rc, OVE_OK);
	}
#ifdef OVE_HEAP_SPI
	if (destroy_in_callback)
		ove_spi_destroy(active);
#endif
}
static ove_spi_t init(ove_spi_storage_t *storage)
{
	struct ove_spi_cfg cfg = {.word_size = 8, .clock_hz = 1000000};
	ove_spi_t spi = NULL;
	assert_int_equal(ove_spi_init(&spi, storage, &cfg), OVE_OK);
	active = spi;
	return spi;
}
static void *submit(void *arg)
{
	int *result = arg;
	*result = ove_spi_transfer_async(active, &cs, tx, rx, sizeof(tx), complete, rx);
	return NULL;
}
static pthread_t start_submit(int *result)
{
	pthread_t thread;
	assert_int_equal(pthread_create(&thread, NULL, submit, result), 0);
	pthread_mutex_lock(&gate_lock);
	while (!entered)
		pthread_cond_wait(&gate_cv, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
	return thread;
}
static void finish_submit(pthread_t thread)
{
	pthread_mutex_lock(&gate_lock);
	released = 1;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&gate_lock);
	assert_int_equal(pthread_join(thread, NULL), 0);
}
static void test_notification_waits_for_submit_access(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	mode = INLINE_GATE;
	int result = 1;
	pthread_t thread = start_submit(&result);
	unsigned early = atomic_load(&callbacks);
	finish_submit(thread);
	ove_spi_deinit(active);
	assert_int_equal(early, 0);
	assert_int_equal(result, OVE_OK);
	assert_int_equal(atomic_load(&callbacks), 1);
}
static void test_pending_close_waits_for_retirement(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	int result = 1;
	assert_null(submit(&result));
	assert_int_equal(result, OVE_OK);
	ove_spi_deinit(active);
	unsigned early = atomic_load(&closes);
	ove_spi_async_complete(active, OVE_OK);
	assert_int_equal(early, 0);
	assert_int_equal(atomic_load(&closes), 1);
	assert_int_equal(atomic_load(&callbacks), 1);
}
static void test_rejection_with_close_releases_without_notification(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	mode = REJECT_GATE;
	int result = 1;
	pthread_t thread = start_submit(&result);
	ove_spi_deinit(active);
	unsigned early = atomic_load(&closes);
	finish_submit(thread);
	assert_int_equal(early, 0);
	assert_int_equal(result, OVE_ERR_IO);
	assert_int_equal(atomic_load(&closes), 1);
	assert_int_equal(atomic_load(&callbacks), 0);
	assert_int_equal(atomic_load(&selected), 0);
}
static void test_reentrant_submit_waits_for_provider_access(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	mode = INLINE_COMPLETE;
	resubmit = 1;
	int result = 1;
	submit(&result);
	ove_spi_deinit(active);
	assert_int_equal(result, OVE_OK);
	assert_int_equal(atomic_load(&callbacks), 2);
	assert_int_equal(atomic_load(&overlap), 0);
}
static void test_prepare_failure_does_not_publish_operation(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	prepare_error = OVE_ERR_NO_MEMORY;
	int result = 1;
	submit(&result);
	if (result == OVE_OK)
		ove_spi_async_complete(active, OVE_OK);
	ove_spi_deinit(active);
	assert_int_equal(result, OVE_ERR_NO_MEMORY);
	assert_int_equal(atomic_load(&submissions), 0);
	assert_int_equal(atomic_load(&callbacks), 0);
}
static void test_busy_preserves_pending_metadata(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	int rc = 1;
	submit(&rc);
	assert_int_equal(rc, OVE_OK);
	const struct ove_spi_cs *saved_cs = storage.pending_cs;
	ove_dma_complete_cb saved_cb = storage.pending_cb;
	void *saved_user = storage.pending_ud;
	int busy = ove_spi_transfer_async(active, NULL, tx, NULL, 1, complete, rx);
	int unchanged = storage.pending_cs == saved_cs && storage.pending_cb == saved_cb &&
			storage.pending_ud == saved_user;
	ove_spi_async_complete(active, OVE_OK);
	ove_spi_deinit(active);
	assert_int_equal(busy, OVE_ERR_BUS_BUSY);
	assert_true(unchanged);
	assert_int_equal(atomic_load(&submissions), 1);
	assert_int_equal(atomic_load(&callbacks), 1);
}
static void test_inactive_completion_does_not_replay(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	int rc = 1;
	submit(&rc);
	ove_spi_async_complete(active, OVE_ERR_IO);
	ove_spi_async_complete(active, OVE_OK);
	ove_spi_deinit(active);
	assert_int_equal(atomic_load(&callbacks), 1);
	assert_int_equal(atomic_load(&completion_result), OVE_ERR_IO);
	assert_null(storage.pending_cb);
	assert_null(storage.pending_ud);
	assert_null(storage.pending_cs);
	assert_null(storage.pending_tx);
	assert_null(storage.pending_rx);
}
static void test_callback_can_deinit_and_reinitialize(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	mode = INLINE_COMPLETE;
	reinit_storage = &storage;
	int rc = 1;
	submit(&rc);
	ove_spi_deinit(active);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(atomic_load(&callbacks), 2);
	assert_int_equal(atomic_load(&closes), 2);
	assert_int_equal(atomic_load(&overlap), 0);
}
static void *finish_io(void *argument)
{
	(void)argument;
	ove_spi_async_complete(active, OVE_OK);
	return NULL;
}
static void *close_controller(void *argument)
{
	(void)argument;
	ove_spi_deinit(active);
	atomic_store(&close_returned, 1);
	return NULL;
}
static void test_captured_close_finishes_before_callback(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	int rc = 1;
	submit(&rc);
	ove_spi_deinit(active);
	block_close = 1;
	pthread_t worker;
	assert_int_equal(pthread_create(&worker, NULL, finish_io, NULL), 0);
	wait_entered(&close_entered);
	unsigned early = atomic_load(&callbacks);
	release_gate(&close_released);
	assert_int_equal(pthread_join(worker, NULL), 0);
	assert_int_equal(early, 0);
	assert_int_equal(atomic_load(&callbacks), 1);
	assert_int_equal(atomic_load(&closes), 1);
}
static void test_late_close_call_and_callback_are_separate_obligations(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	int rc = 1;
	submit(&rc);
	block_callback = 1;
	pthread_t worker, closer;
	assert_int_equal(pthread_create(&worker, NULL, finish_io, NULL), 0);
	wait_entered(&callback_entered);
	block_close = 1;
	assert_int_equal(pthread_create(&closer, NULL, close_controller, NULL), 0);
	wait_entered(&close_entered);
	release_gate(&callback_released);
	assert_int_equal(pthread_join(worker, NULL), 0);
	int returned = atomic_load(&close_returned);
	unsigned notified = atomic_load(&callbacks);
	/* Backing remains live until both independent obligations are complete. */
	release_gate(&close_released);
	assert_int_equal(pthread_join(closer, NULL), 0);
	assert_int_equal(returned, 0);
	assert_int_equal(notified, 1);
	assert_int_equal(atomic_load(&close_returned), 1);
}
static void test_no_completion_retains_one_static_resource(void **state)
{
	(void)state;
	reset();
	static ove_spi_storage_t backing;
	init(&backing);
	int rc = 1;
	submit(&rc);
	ove_spi_deinit(active);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(atomic_load(&closes), 0);
	assert_int_equal(atomic_load(&callbacks), 0);
	assert_true(__atomic_load_n(&backing.async_busy, __ATOMIC_ACQUIRE) != 0);
	assert_non_null(backing.pending_cb);
	/* This deliberately never-completed fixture remains permanently rooted. */
}

#ifdef OVE_HEAP_SPI
static void test_pending_destroy_keeps_native_backing(void **state)
{
	(void)state;
	reset();
	struct ove_spi_cfg cfg = {.word_size = 8};
	assert_int_equal(ove_spi_create(&active, &cfg), OVE_OK);
	int result = 1;
	submit(&result);
	ove_spi_destroy(active);
	unsigned early = atomic_load(&frees);
	ove_spi_async_complete(active, OVE_OK);
	unsigned final = atomic_load(&frees);
	free(allocation);
	assert_int_equal(early, 0);
	assert_int_equal(final, 1);
	assert_int_equal(atomic_load(&callbacks), 1);
}
static void test_callback_can_reclaim_native_heap_storage(void **state)
{
	(void)state;
	reset();
	quarantine = 0;
	destroy_in_callback = 1;
	mode = INLINE_COMPLETE;
	struct ove_spi_cfg cfg = {.word_size = 8};
	assert_int_equal(ove_spi_create(&active, &cfg), OVE_OK);
	int rc = 1;
	submit(&rc);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(atomic_load(&callbacks), 1);
	assert_int_equal(atomic_load(&frees), 1);
	assert_int_equal(atomic_load(&closes), 1);
	assert_null(allocation);
}
static void test_rejected_submit_with_destroy_reclaims_without_callback(void **state)
{
	(void)state;
	reset();
	quarantine = 0;
	mode = REJECT_GATE;
	struct ove_spi_cfg cfg = {.word_size = 8};
	assert_int_equal(ove_spi_create(&active, &cfg), OVE_OK);
	int rc = 1;
	pthread_t worker = start_submit(&rc);
	ove_spi_destroy(active);
	unsigned early = atomic_load(&frees);
	finish_submit(worker);
	assert_int_equal(early, 0);
	assert_int_equal(rc, OVE_ERR_IO);
	assert_int_equal(atomic_load(&callbacks), 0);
	assert_int_equal(atomic_load(&frees), 1);
	assert_null(allocation);
}

#endif

struct contender {
	int result;
	unsigned index;
	unsigned char write[4], read[4];
};
static pthread_barrier_t admission_barrier;
static atomic_uint admitted_index;

static void contender_complete(int result, void *user)
{
	struct contender *operation = user;
	atomic_store(&admitted_index, operation->index);
	atomic_store(&completion_result, result);
	atomic_fetch_add(&callbacks, 1);
}

static void *contend(void *user)
{
	struct contender *operation = user;
	pthread_barrier_wait(&admission_barrier);
	operation->result = ove_spi_transfer_async(active, &cs, operation->write, operation->read,
						   sizeof(operation->write), contender_complete,
						   operation);
	return NULL;
}

static void test_concurrent_admission_retains_only_the_winner(void **state)
{
	(void)state;
	reset();
	ove_spi_storage_t storage;
	init(&storage);
	struct contender operations[16] = {0};
	pthread_t threads[16];
	assert_int_equal(pthread_barrier_init(&admission_barrier, NULL, 16), 0);
	for (unsigned i = 0; i < 16; ++i) {
		operations[i].index = i;
		assert_int_equal(pthread_create(&threads[i], NULL, contend, &operations[i]), 0);
	}
	for (unsigned i = 0; i < 16; ++i)
		assert_int_equal(pthread_join(threads[i], NULL), 0);
	pthread_barrier_destroy(&admission_barrier);
	unsigned accepted = 0, winner = 0;
	for (unsigned i = 0; i < 16; ++i) {
		if (operations[i].result == OVE_OK) {
			++accepted;
			winner = i;
		} else {
			assert_int_equal(operations[i].result, OVE_ERR_BUS_BUSY);
		}
	}
	/* Release the admitted operation before evaluating the race's assertions. */
	void *retained = storage.pending_ud;
	ove_spi_async_complete(active, OVE_ERR_IO);
	ove_spi_deinit(active);
	assert_int_equal(accepted, 1);
	assert_int_equal(atomic_load(&submissions), 1);
	assert_ptr_equal(retained, &operations[winner]);
	assert_int_equal(atomic_load(&callbacks), 1);
	assert_int_equal(atomic_load(&admitted_index), winner);
	assert_int_equal(atomic_load(&completion_result), OVE_ERR_IO);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		cmocka_set_test_filter(argv[1]);
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_notification_waits_for_submit_access),
		cmocka_unit_test(test_pending_close_waits_for_retirement),
		cmocka_unit_test(test_rejection_with_close_releases_without_notification),
		cmocka_unit_test(test_reentrant_submit_waits_for_provider_access),
		cmocka_unit_test(test_prepare_failure_does_not_publish_operation),
		cmocka_unit_test(test_busy_preserves_pending_metadata),
		cmocka_unit_test(test_inactive_completion_does_not_replay),
		cmocka_unit_test(test_callback_can_deinit_and_reinitialize),
		cmocka_unit_test(test_captured_close_finishes_before_callback),
		cmocka_unit_test(test_late_close_call_and_callback_are_separate_obligations),
		cmocka_unit_test(test_no_completion_retains_one_static_resource),
		cmocka_unit_test(test_concurrent_admission_retains_only_the_winner),
#ifdef OVE_HEAP_SPI
		cmocka_unit_test(test_pending_destroy_keeps_native_backing),
		cmocka_unit_test(test_callback_can_reclaim_native_heap_storage),
		cmocka_unit_test(test_rejected_submit_with_destroy_reclaims_without_callback),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
