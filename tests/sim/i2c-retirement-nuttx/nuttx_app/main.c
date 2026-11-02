/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/i2c.h"
#include "ove/storage.h"
#include "framework/semihosting_exit.h"
#include "ove/app.h"
#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sem_t entered, release_worker, notified;
static atomic_uint submissions, callbacks;
static atomic_int callback_result, callback_closed;
static int active_fd, failures;
static ove_i2c_t observed_controller;
static pid_t application_pid;
static unsigned char tx[4] = {1, 2, 3, 4}, rx[7];
static void wait_for(sem_t *semaphore);

static void check(int condition, const char *message)
{
	if (!condition) {
		printf("FAIL: %s\n", message);
		++failures;
	}
}

static void require(int condition, const char *message)
{
	check(condition, message);
	if (!condition) {
		fflush(stdout);
		semihosting_exit(1);
	}
}

int i2c_probe_open(const char *path, int flags, ...)
{
	(void)path;
	/* Retain real per-group descriptor allocation and native close. */
	return open("/dev/null", flags);
}

int i2c_probe_ioctl(int fd, int request, ...)
{
	va_list arguments;
	va_start(arguments, request);
	struct i2c_transfer_s *sequence = (void *)va_arg(arguments, unsigned long);
	va_end(arguments);
	atomic_fetch_add(&submissions, 1);
	require(request == I2CIOC_TRANSFER && fd == active_fd, "native ioctl arguments");
	check(getpid() == application_pid, "worker retains application file group");
	sem_post(&entered);
	wait_for(&release_worker);
	check(sequence->msgc == 2, "independent two-phase native transfer");
	struct i2c_msg_s *messages = sequence->msgv;
	check(messages[0].length == sizeof(tx) && messages[1].length == sizeof(rx),
	      "independent TX and RX byte counts");
	check(messages[0].flags == I2C_M_NOSTOP && messages[1].flags == I2C_M_READ,
	      "repeated-start and final-read flags");
	memset(messages[1].buffer, 0xa5, messages[1].length);
	memcpy(messages[1].buffer, messages[0].buffer, messages[0].length);
	return 0;
}

static void completed(int result, void *context)
{
	(void)context;
	check(observed_controller->pending_tx == NULL && observed_controller->pending_rx == NULL &&
		      observed_controller->pending_tx_len == 0 &&
		      observed_controller->pending_rx_len == 0 &&
		      observed_controller->pending_addr == 0 &&
		      observed_controller->pending_cb == NULL &&
		      observed_controller->pending_ud == NULL,
	      "provider and callback metadata retire before notification");
	atomic_store(&callback_result, result);
	atomic_store(&callback_closed, fcntl(active_fd, F_GETFD) == -1 && errno == EBADF);
	atomic_fetch_add(&callbacks, 1);
	sem_post(&notified);
}

static void wait_for(sem_t *semaphore)
{
	int result;
	do {
		result = sem_wait(semaphore);
	} while (result != 0 && errno == EINTR);
	require(result == 0, "native semaphore wait succeeds");
}

#ifdef CONFIG_OVE_ZERO_HEAP
extern void *ove_heap_lock_post_alloc_(size_t size);
extern int ove_heap_lock_post_free_(void *pointer);
#include <nuttx/mm/mm.h>
static struct mm_heap_s *reserved_heap;
static _Atomic(pid_t) observing_thread = -1;
extern void *__real_mm_malloc(struct mm_heap_s *heap, size_t size);

void *__wrap_mm_malloc(struct mm_heap_s *heap, size_t size)
{
	if (atomic_load(&observing_thread) == gettid())
		reserved_heap = heap;
	return __real_mm_malloc(heap, size);
}

static void observe_reserved_heap(void)
{
	atomic_store(&observing_thread, gettid());
	void *allocation = ove_heap_lock_post_alloc_(1);
	atomic_store(&observing_thread, -1);
	require(allocation != NULL && reserved_heap != NULL, "observe actual reserved native heap");
	ove_heap_lock_post_free_(allocation);
}

static void *reserved[256];

static unsigned reserve_all(void)
{
	unsigned count = 0;
	while (count < 256 && (reserved[count] = ove_heap_lock_post_alloc_(128)) != NULL)
		++count;
	return count;
}

static void release_all(unsigned count)
{
	while (count != 0)
		ove_heap_lock_post_free_(reserved[--count]);
}

static size_t available(void)
{
	/* Native task exit may defer frees until this heap's next allocation. */
	void *probe = ove_heap_lock_post_alloc_(1);
	if (probe != NULL)
		ove_heap_lock_post_free_(probe);
	return mm_mallinfo(reserved_heap).fordblks;
}
#else
static size_t available(void)
{
	return mallinfo().fordblks;
}
#endif

static void transfer_once(int measured)
{
	static ove_i2c_storage_t storage;
	ove_i2c_t i2c;
	struct ove_i2c_cfg config = {.speed = OVE_I2C_SPEED_FAST};
	require(ove_i2c_init(&i2c, &storage, &config) == OVE_OK, "native open succeeds");
	active_fd = storage.fd;
	observed_controller = i2c;
	size_t before = available();
	require(ove_i2c_write_read_async(i2c, 0x42, tx, sizeof(tx), rx, sizeof(rx), completed,
					 NULL) == OVE_OK,
		"actual pthread submission succeeds");
	wait_for(&entered);
	if (measured) {
		size_t during = available();
		printf("Native worker allocation: %lu bytes, pthread stack %u bytes\n",
		       (unsigned long)(before - during), CONFIG_PTHREAD_STACK_DEFAULT);
		check(during < before, "actual worker consumes native allocation capacity");
	}
	ove_i2c_deinit(i2c);
	check(fcntl(active_fd, F_GETFD) >= 0, "pending close retains native descriptor");
	sem_post(&release_worker);
	wait_for(&notified);
	check(atomic_load(&callback_result) == OVE_OK, "native result reaches callback");
	check(atomic_load(&callback_closed), "deferred native close precedes callback");
	check(memcmp(tx, rx, sizeof(tx)) == 0 && rx[sizeof(rx) - 1] == 0xa5,
	      "native ioctl independent byte counts and data");
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("NuttX I2C retirement starting\n");
	application_pid = getpid();
	sem_init(&entered, 0, 0);
	sem_init(&release_worker, 0, 0);
	sem_init(&notified, 0, 0);
#ifdef CONFIG_OVE_ZERO_HEAP
	observe_reserved_heap();
#endif
#if defined(CONFIG_OVE_ZERO_HEAP) && defined(CONFIG_DEBUG_ASSERTIONS)
	ove_i2c_storage_t storage;
	ove_i2c_t i2c;
	struct ove_i2c_cfg config = {.speed = OVE_I2C_SPEED_FAST};
	require(ove_i2c_init(&i2c, &storage, &config) == OVE_OK, "open before heap lock");
	ove_heap_lock();
	size_t capacity = available();
	check(ove_i2c_async_prepare() == OVE_ERR_NOT_SUPPORTED, "debug zero-heap capability");
	check(ove_i2c_write_read_async(i2c, 0x42, tx, sizeof(tx), rx, sizeof(rx), completed,
				       NULL) == OVE_ERR_NOT_SUPPORTED,
	      "unsupported operation rejected before publication");
	check(!atomic_load(&submissions) && !atomic_load(&callbacks) && !storage.async_busy &&
		      !storage.pending_cb && available() == capacity,
	      "rejection preserves state and allocation capacity");
	ove_i2c_deinit(i2c);
#else
	/* Warm libc, file-group and pthread state before measuring retirement. */
	transfer_once(0);
	usleep(50000);
#ifdef CONFIG_OVE_ZERO_HEAP
	(void)available();
	ove_heap_lock();
#endif
	size_t baseline = available();
	for (unsigned iteration = 0; iteration < 12; ++iteration) {
		transfer_once(1);
		unsigned retry = 0;
		while (available() != baseline && retry++ < 200)
			usleep(10000);
		if (available() != baseline)
			printf("native free bytes: baseline=%lu actual=%lu iteration=%u\n",
			       (unsigned long)baseline, (unsigned long)available(), iteration);
		check(available() == baseline, "detached native worker and descriptor reclaimed");
	}
#ifdef CONFIG_OVE_ZERO_HEAP
	static ove_i2c_storage_t storage;
	ove_i2c_t i2c;
	struct ove_i2c_cfg config = {.speed = OVE_I2C_SPEED_FAST};
	require(ove_i2c_init(&i2c, &storage, &config) == OVE_OK, "open before reserve exhaustion");
	unsigned held = reserve_all();
	unsigned previous_callbacks = atomic_load(&callbacks);
	unsigned previous_submissions = atomic_load(&submissions);
	int result = ove_i2c_write_read_async(i2c, 0x42, tx, sizeof(tx), rx, sizeof(rx), completed,
					      NULL);
	require(result == OVE_ERR_NO_MEMORY, "actual pthread allocation exhaustion is reported");
	check(atomic_load(&callbacks) == previous_callbacks &&
		      atomic_load(&submissions) == previous_submissions && !storage.async_busy &&
		      !storage.pending_cb && storage.pending_tx == NULL &&
		      storage.pending_rx == NULL && storage.pending_tx_len == 0 &&
		      storage.pending_rx_len == 0 && storage.pending_addr == 0,
	      "rejection retains no operation");
	release_all(held);
	ove_i2c_deinit(i2c);
	transfer_once(1);
	usleep(50000);
	check(available() == baseline, "reserve recovers after rejected worker creation");
#endif
#endif
	printf("NuttX I2C retirement: failures=%d callbacks=%u submissions=%u\n", failures,
	       atomic_load(&callbacks), atomic_load(&submissions));
	fflush(stdout);
	semihosting_exit(failures != 0);
}
