/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

void lib_cxx_initialize(void);
void ove_nuttx_native_cxx_initialize(void);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned int calls;
static int entered;
static int release;

void ove_nuttx_native_cxx_initialize(void)
{
	pthread_mutex_lock(&lock);
	if (++calls == 1) {
		entered = 1;
		pthread_cond_broadcast(&changed);
		while (!release)
			pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);
}

static void *invoke(void *unused)
{
	(void)unused;
	lib_cxx_initialize();
	return NULL;
}

static void create(pthread_t *thread)
{
	if (pthread_create(thread, NULL, invoke, NULL) != 0)
		exit(2);
}

int main(void)
{
	pthread_t owner;
	pthread_t followers[8];
	create(&owner);
	pthread_mutex_lock(&lock);
	while (!entered)
		pthread_cond_wait(&changed, &lock);
	pthread_mutex_unlock(&lock);
	for (unsigned int i = 0; i < 8; ++i)
		create(&followers[i]);
	for (unsigned int i = 0; i < 8; ++i)
		if (pthread_join(followers[i], NULL) != 0)
			return 2;
	pthread_mutex_lock(&lock);
	release = 1;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	if (pthread_join(owner, NULL) != 0)
		return 2;
	lib_cxx_initialize();
	printf("NATIVE_INITIALIZER_CALLS=%u\n", calls);
	return calls != 1;
}
