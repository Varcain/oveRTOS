/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Simulated TLS — passthrough (no encryption).
 *
 * Handshake always succeeds.  send/recv proxy directly to the
 * underlying socket.  Used by the sim network stack where all
 * connections are loopback — encryption is meaningless.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_NET_TLS

#include "ove/net_tls.h"
#include "ove/net.h"
#include "ove_backend_common.h"
#include <string.h>

int ove_tls_init(ove_tls_t *tls, ove_tls_storage_t *storage)
{
	if (!tls || !storage) return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = (struct ove_tls *)storage;
	memset(t, 0, sizeof(*t));
	*tls = t;
	return OVE_OK;
}

void ove_tls_deinit(ove_tls_t tls)
{
	(void)tls;
}

int ove_tls_handshake(ove_tls_t tls, ove_socket_t sock,
		      const ove_tls_config_t *cfg)
{
	(void)cfg;
	if (!tls || !sock) return OVE_ERR_INVALID_PARAM;
	/* Store the socket for send/recv passthrough. */
	tls->sock = sock;
	return OVE_OK;
}

int ove_tls_send(ove_tls_t tls, const void *data, size_t len,
		 size_t *sent)
{
	if (!tls || !tls->sock) return OVE_ERR_INVALID_PARAM;
	return ove_socket_send(tls->sock, data, len, sent);
}

int ove_tls_recv(ove_tls_t tls, void *buf, size_t len,
		 size_t *received)
{
	if (!tls || !tls->sock) return OVE_ERR_INVALID_PARAM;
	return ove_socket_recv(tls->sock, buf, len, received, OVE_WAIT_FOREVER);
}

void ove_tls_close(ove_tls_t tls)
{
	(void)tls;
	/* No-op — underlying socket is closed separately. */
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_tls_create(ove_tls_t *tls)
{
	if (!tls) return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t) return OVE_ERR_NO_MEMORY;
	memset(t, 0, sizeof(*t));
	*tls = t;
	return OVE_OK;
}

void ove_tls_destroy(ove_tls_t tls)
{
	if (tls) OVE_BACKEND_FREE(tls);
}
#endif

#endif /* CONFIG_OVE_NET_TLS */
