/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include <stdio.h>
#include <string.h>

static void check(bool condition, const char *message, unsigned *failures)
{
	if (!condition) {
		++*failures;
		fprintf(stderr, "failed: %s\n", message);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "controls") && strcmp(argv[1], "canary")))
		return 2;
	unsigned failures = !strcmp(argv[1], "canary");
	if (failures)
		fputs("failed: explicit early failure canary\n", stderr);
	/* An inert aligned token tests unchanged-entry reporting, not a live
	 * native mutex: no disabled function may dereference it. */
	ove_mutex_storage_t token = {0};
	ove_mutex_t mutex = (ove_mutex_t)&token;
	bool held = false;
	check(ove_condvar_wait_checked(NULL, mutex, 0, &held) == OVE_ERR_NOT_SUPPORTED && held,
	      "disabled wait preserves entry ownership", &failures);
	check(ove_condvar_wait_checked(NULL, NULL, 0, &held) == OVE_ERR_NOT_SUPPORTED && !held,
	      "disabled wait initializes absent ownership", &failures);
	check(ove_condvar_wait_checked(NULL, NULL, 0, NULL) == OVE_ERR_NOT_SUPPORTED,
	      "disabled wait permits absent output", &failures);
	printf("%s: failures=%u controls=3 safety=0\n", argv[1], failures);
	return failures ? 1 : 0;
}
