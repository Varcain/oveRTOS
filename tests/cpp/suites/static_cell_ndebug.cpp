/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Release-build (NDEBUG) half of the StaticCell double-init test in
 * test_static_cell.cpp.  CMake compiles this one TU with NDEBUG so
 * OVE_STATIC_INIT_ASSERT expands to ((void)(cond)), exactly as in a
 * release firmware build.  The probe type has internal linkage, so its
 * StaticCell instantiation cannot merge with any asserting one elsewhere.
 * The test runs this in a forked child: the second init() must trap.
 */

#ifndef NDEBUG
#error "static_cell_ndebug.cpp must be compiled with NDEBUG"
#endif

#include "ove/sync.hpp"
#include <unistd.h>

namespace
{

int g_ctor_fd = -1;

/* Reports every construction to the parent through a pipe, since the child
 * is expected to die inside the second init(). */
struct Probe {
	int v;
	explicit Probe(int x) : v(x)
	{
		const char c = 'c';
		ssize_t n = write(g_ctor_fd, &c, 1);
		(void)n;
	}
};

} /* namespace */

void static_cell_ndebug_double_init(int ctor_fd)
{
	g_ctor_fd = ctor_fd;
	ove::StaticCell<Probe> cell;
	(void)cell.init(1);
	(void)cell.init(2); /* must trap, never construct a second Probe */
}
