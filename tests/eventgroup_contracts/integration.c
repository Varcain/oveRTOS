/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.h"
#include "ove/irq.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
static void check(int value, const char *name)
{
	++checks;
	if (!value) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static void notified(void *data)
{
	++*(unsigned *)data;
}
int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "canary")))
		return 2;
	if (argc == 2)
		check(0, "early failure survives controls");
	check((ove_sync_context() & OVE_SYNC_CONTEXT_RESOURCE) != 0,
	      "eventgroup-only native resource query available");
	ove_eventgroup_storage_t storage;
	for (unsigned cycle = 0; cycle < 4; ++cycle) {
		ove_eventgroup_t group = NULL;
#ifdef CONFIG_OVE_ZERO_HEAP
		int rc = ove_eventgroup_init(&group, &storage);
#else
		(void)storage;
		int rc = ove_eventgroup_create(&group);
#endif
		check(rc == OVE_OK && group != NULL, "native constructor or reuse succeeds");
		if (rc != OVE_OK || group == NULL)
			continue;
		check(ove_eventgroup_get_bits(group) == 0, "new group is empty");
		check(ove_eventgroup_set_bits(group, 5) == 5, "ordinary bits publish");
		check(ove_eventgroup_clear_bits(group, 1) == 5, "clear returns full prior value");
		check(ove_eventgroup_get_bits(group) == 4, "unaffected bit remains");
		ove_eventbits_t value = 0;
		rc = ove_eventgroup_wait_bits(group, 4, OVE_EG_WAIT_ALL | OVE_EG_CLEAR_ON_EXIT, 0,
					      &value);
		check(rc == OVE_OK && value == 4, "ordinary wait consumes matching bits");
		check(ove_eventgroup_get_bits(group) == 0, "clear on exit completes");
		check(ove_eventgroup_wait_bits(group, 1, 0, 0, &value) == OVE_ERR_TIMEOUT,
		      "unsatisfied ordinary poll times out");
		unsigned count = 0;
		check(ove_eventgroup_set_notify(group, notified, &count) == OVE_OK,
		      "ordinary callback installs");
		check(ove_eventgroup_set_bits(group, 2) == 2 && count == 1,
		      "ordinary callback follows publication");
		check(ove_eventgroup_set_notify(group, NULL, NULL) == OVE_OK,
		      "ordinary callback removes");
		check(ove_eventgroup_clear_bits(group, 2) == 2 && count == 1,
		      "ordinary clear does not notify");
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_eventgroup_deinit(group);
#else
		ove_eventgroup_destroy(group);
#endif
	}
	printf("eventgroup integration: checks=%u failures=%u\n", checks, failures);
	return failures != 0;
}
