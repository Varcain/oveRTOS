/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/spi.h"
#include "ove/storage.h"
#include "framework/semihosting_exit.h"
#include "ove/app.h"
#include <nuttx/config.h>
#include <nuttx/spi/spi_transfer.h>
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
static atomic_uint submissions, callbacks, cs_changes;
static atomic_int callback_result, callback_closed;
static int active_fd, failures;
static pid_t application_pid;
static unsigned char tx[4] = {1, 2, 3, 4}, rx[4];
static const struct ove_spi_cs cs = {.active_low = 1};
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

int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	(void)port;
	(void)pin;
	(void)value;
	atomic_fetch_add(&cs_changes, 1);
	return OVE_OK;
}

int spi_probe_open(const char *path, int flags, ...)
{
	(void)path;
	/* Retain real per-group descriptor allocation and native close. */
	return open("/dev/null", flags);
}

int spi_probe_ioctl(int fd, int request, ...)
{
	va_list arguments;
	va_start(arguments, request);
	struct spi_sequence_s *sequence = (void *)va_arg(arguments, unsigned long);
	va_end(arguments);
	atomic_fetch_add(&submissions, 1);
	require(request == SPIIOC_TRANSFER && fd == active_fd, "native ioctl arguments");
	check(getpid() == application_pid, "worker retains application file group");
	sem_post(&entered);
	wait_for(&release_worker);
	struct spi_trans_s *transfer = sequence->trans;
	memcpy(transfer->rxbuffer, transfer->txbuffer, transfer->nwords * (sequence->nbits / 8));
	return 0;
}

static void completed(int result, void *context)
{
	(void)context;
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
	static ove_spi_storage_t storage;
	ove_spi_t spi;
	struct ove_spi_cfg config = {.word_size = 8, .clock_hz = 1000000};
	require(ove_spi_init(&spi, &storage, &config) == OVE_OK, "native open succeeds");
	active_fd = storage.fd;
	size_t before = available();
	require(ove_spi_transfer_async(spi, &cs, tx, rx, sizeof(tx), completed, NULL) == OVE_OK,
		"actual pthread submission succeeds");
	wait_for(&entered);
	if (measured)
		check(available() < before, "actual worker consumes native allocation capacity");
	ove_spi_deinit(spi);
	check(fcntl(active_fd, F_GETFD) >= 0, "pending close retains native descriptor");
	sem_post(&release_worker);
	wait_for(&notified);
	check(atomic_load(&callback_result) == OVE_OK, "native result reaches callback");
	check(atomic_load(&callback_closed), "deferred native close precedes callback");
	check(memcmp(tx, rx, sizeof(tx)) == 0, "native ioctl byte count and data");
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf("NuttX SPI retirement starting\n");
	application_pid = getpid();
	sem_init(&entered, 0, 0);
	sem_init(&release_worker, 0, 0);
	sem_init(&notified, 0, 0);
#ifdef CONFIG_OVE_ZERO_HEAP
	observe_reserved_heap();
#endif
#if defined(CONFIG_OVE_ZERO_HEAP) && defined(CONFIG_DEBUG_ASSERTIONS)
	ove_spi_storage_t storage;
	ove_spi_t spi;
	struct ove_spi_cfg config = {.word_size = 8};
	require(ove_spi_init(&spi, &storage, &config) == OVE_OK, "open before heap lock");
	ove_heap_lock();
	size_t capacity = available();
	check(ove_spi_async_prepare() == OVE_ERR_NOT_SUPPORTED, "debug zero-heap capability");
	check(ove_spi_transfer_async(spi, &cs, tx, rx, sizeof(tx), completed, NULL) ==
		      OVE_ERR_NOT_SUPPORTED,
	      "unsupported operation rejected before publication");
	check(!atomic_load(&submissions) && !atomic_load(&callbacks) && !atomic_load(&cs_changes) &&
		      !storage.async_busy && !storage.pending_cb && available() == capacity,
	      "rejection preserves state and allocation capacity");
	ove_spi_deinit(spi);
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
	static ove_spi_storage_t storage;
	ove_spi_t spi;
	struct ove_spi_cfg config = {.word_size = 8};
	require(ove_spi_init(&spi, &storage, &config) == OVE_OK, "open before reserve exhaustion");
	unsigned held = reserve_all();
	unsigned previous_callbacks = atomic_load(&callbacks);
	unsigned previous_submissions = atomic_load(&submissions);
	unsigned previous_cs = atomic_load(&cs_changes);
	int result = ove_spi_transfer_async(spi, &cs, tx, rx, sizeof(tx), completed, NULL);
	require(result == OVE_ERR_NO_MEMORY, "actual pthread allocation exhaustion is reported");
	check(atomic_load(&callbacks) == previous_callbacks &&
		      atomic_load(&submissions) == previous_submissions &&
		      atomic_load(&cs_changes) == previous_cs + 2 && !storage.async_busy &&
		      !storage.pending_cb,
	      "rejection balances CS and retains no operation");
	release_all(held);
	ove_spi_deinit(spi);
	transfer_once(1);
	usleep(50000);
	check(available() == baseline, "reserve recovers after rejected worker creation");
#endif
#endif
	printf("NuttX SPI retirement: failures=%d callbacks=%u submissions=%u\n", failures,
	       atomic_load(&callbacks), atomic_load(&submissions));
	fflush(stdout);
	semihosting_exit(failures != 0);
}
