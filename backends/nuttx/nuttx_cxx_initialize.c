/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <nuttx/config.h>

#if !defined(CONFIG_BUILD_FLAT) || !defined(CONFIG_HAVE_CXXINITIALIZE)
#error "The constructor guard requires a flat image with static initialization"
#endif

void lib_cxx_initialize(void);
void ove_nuttx_native_cxx_initialize(void);

static unsigned char constructors_started;

_Static_assert(__atomic_always_lock_free(sizeof(constructors_started), 0),
	       "Constructor admission must not require a runtime lock");

void lib_cxx_initialize(void)
{
	/* A constructor can start a task and wait for its startup notification.
	 * That task enters here before its entry function can notify the creator.
	 * Claim the shared image before callbacks and let later entrants proceed;
	 * waiting for the outer pass would deadlock that startup handshake. This
	 * lets child code run before globals initialized later in the table. */
	if (__atomic_exchange_n(&constructors_started, 1, __ATOMIC_ACQ_REL))
		return;
	ove_nuttx_native_cxx_initialize();
}
