/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/queue.h"
#include "ove/irq.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
static void check(int ok, const char *name)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "canary")))
		return 2;
	if (argc == 2)
		check(0, "early failure survives controls");
	check((ove_sync_context() & OVE_SYNC_CONTEXT_RESOURCE) != 0,
	      "queue-only native resource context available");
	ove_queue_storage_t storage;
	unsigned buffer[4];
	for (unsigned cycle = 0; cycle < 4; ++cycle) {
		ove_queue_t q = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
		int rc = ove_queue_init(&q, &storage, buffer, sizeof(unsigned), 4);
#else
		(void)storage;
		(void)buffer;
		int rc = ove_queue_create(&q, sizeof(unsigned), 4);
#endif
		check(rc == OVE_OK && q != NULL, "native queue initializes or reuses backing");
		if (rc != OVE_OK || q == NULL)
			continue;
		for (unsigned value = 1; value <= 4; ++value)
			check(ove_queue_send(q, &value, 0) == OVE_OK, "native enqueue succeeds");
		unsigned value = 9;
		check(ove_queue_send(q, &value, 0) == OVE_ERR_QUEUE_FULL,
		      "full queue is observed without blocking");
		check(ove_queue_count(q) == 4 && ove_queue_capacity(q) == 4,
		      "native count and capacity are exact");
		for (unsigned expected = 1; expected <= 4; ++expected) {
			value = 0;
			int received = ove_queue_receive(q, &value, 0);
			check(received == OVE_OK && value == expected,
			      "native FIFO payload is preserved");
		}
		check(ove_queue_receive(q, &value, 0) == OVE_ERR_QUEUE_EMPTY,
		      "empty queue is observed without blocking");
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_queue_deinit(q);
#else
		ove_queue_destroy(q);
#endif
	}
	printf("queue integration: checks=%u failures=%u\n", checks, failures);
	return failures != 0;
}
