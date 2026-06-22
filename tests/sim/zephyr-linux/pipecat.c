/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * /bin/pipecat fixture: copies stdin to stdout until EOF (a minimal `cat`),
 * the read end of `pipewrite | pipecat`.
 * Build: see sigwait.c header (same recipe, SYM=ove_test_pipecat_bflt).
 */
#include <unistd.h>
int main(void)
{
	char b[64];
	long n;
	while ((n = read(0, b, sizeof(b))) > 0)
		write(1, b, n);
	return 0;
}
