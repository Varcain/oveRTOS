/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LINUX_INTEROP_NETWORK_SMOKE_H
#define LINUX_INTEROP_NETWORK_SMOKE_H

#include "ove/lxp_host.h"

/* Report configured host addressing when networking is enabled. */
void linux_interop_network_report(const ove_lxp_host_t *host);

/* Perform the demo's bounded TCP readiness probe when networking is enabled. */
void linux_interop_network_smoke(const ove_lxp_host_t *host);

#endif /* LINUX_INTEROP_NETWORK_SMOKE_H */
