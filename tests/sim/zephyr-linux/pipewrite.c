/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * /bin/pipewrite fixture: writes a line to stdout, for `pipewrite | pipecat`.
 * Build: see sigwait.c header (same recipe, SYM=ove_test_pipewrite_bflt).
 */
#include <unistd.h>
int main(void)
{
	write(1, "from-writer\n", 12);
	return 0;
}
