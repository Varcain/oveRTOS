/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Portable TLS wrapper using mbedTLS.
 *
 * This file lives in backends/common/ because it is platform-independent:
 * all I/O is delegated to the oveRTOS socket API (ove_socket_send/recv),
 * which each backend implements using its native stack.
 */

#include "ove/ove.h"
#include "ove/net_tls.h"
#include "ove_backend_common.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#ifdef CONFIG_OVE_ZERO_HEAP
#include "mbedtls/memory_buffer_alloc.h"
#endif

#include <string.h>

#ifdef CONFIG_OVE_ZERO_HEAP
#ifndef CONFIG_OVE_NET_TLS_HEAP_SIZE
#define CONFIG_OVE_NET_TLS_HEAP_SIZE  32768
#endif
static unsigned char s_mbedtls_heap[CONFIG_OVE_NET_TLS_HEAP_SIZE];
static int s_mbedtls_heap_initialized;
#endif

/* ---------- BIO callbacks for mbedTLS ---------- */

/* BIO error codes — removed from mbedTLS 3.x (were in net_sockets.h) */
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED  (-0x004E)
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED  (-0x004C)
#endif

/*
 * mbedTLS calls these for I/O instead of raw sockets.
 * The context pointer is the ove_socket_t.
 */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	ove_socket_t sock = (ove_socket_t)ctx;
	size_t sent = 0;
	int ret = ove_socket_send(sock, buf, len, &sent);
	if (ret != OVE_OK) return MBEDTLS_ERR_NET_SEND_FAILED;
	return (int)sent;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	ove_socket_t sock = (ove_socket_t)ctx;
	size_t received = 0;
	int ret = ove_socket_recv(sock, buf, len, &received, OVE_WAIT_FOREVER);
	if (ret == OVE_ERR_NET_CLOSED) return 0;
	if (ret != OVE_OK) return MBEDTLS_ERR_NET_RECV_FAILED;
	return (int)received;
}

/* ---------- TLS session ---------- */

int ove_tls_init(ove_tls_t *tls, ove_tls_storage_t *storage)
{
	if (!tls || !storage) return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = (struct ove_tls *)storage;
	memset(t, 0, sizeof(*t));

#ifdef CONFIG_OVE_ZERO_HEAP
	/* Initialize mbedTLS static buffer allocator (once) */
	if (!s_mbedtls_heap_initialized) {
		mbedtls_memory_buffer_alloc_init(s_mbedtls_heap,
						 sizeof(s_mbedtls_heap));
		s_mbedtls_heap_initialized = 1;
	}

	mbedtls_entropy_context  *entropy  = &t->_entropy;
	mbedtls_ctr_drbg_context *ctr_drbg = &t->_ctr_drbg;
	mbedtls_ssl_config       *conf     = &t->_conf;
	mbedtls_ssl_context      *ssl      = &t->_ssl;
	mbedtls_x509_crt         *cacert   = &t->_cacert;
	mbedtls_x509_crt         *clicert  = &t->_client_cert;
	mbedtls_pk_context       *clikey   = &t->_client_key;
#else
	mbedtls_entropy_context  *entropy  = OVE_BACKEND_MALLOC(sizeof(*entropy));
	mbedtls_ctr_drbg_context *ctr_drbg = OVE_BACKEND_MALLOC(sizeof(*ctr_drbg));
	mbedtls_ssl_config       *conf     = OVE_BACKEND_MALLOC(sizeof(*conf));
	mbedtls_ssl_context      *ssl      = OVE_BACKEND_MALLOC(sizeof(*ssl));
	mbedtls_x509_crt         *cacert   = OVE_BACKEND_MALLOC(sizeof(*cacert));
	mbedtls_x509_crt         *clicert  = OVE_BACKEND_MALLOC(sizeof(*clicert));
	mbedtls_pk_context       *clikey   = OVE_BACKEND_MALLOC(sizeof(*clikey));

	if (!entropy || !ctr_drbg || !conf || !ssl || !cacert ||
	    !clicert || !clikey) {
		OVE_BACKEND_FREE(entropy);
		OVE_BACKEND_FREE(ctr_drbg);
		OVE_BACKEND_FREE(conf);
		OVE_BACKEND_FREE(ssl);
		OVE_BACKEND_FREE(cacert);
		OVE_BACKEND_FREE(clicert);
		OVE_BACKEND_FREE(clikey);
		return OVE_ERR_NO_MEMORY;
	}
#endif

	mbedtls_entropy_init(entropy);
	mbedtls_ctr_drbg_init(ctr_drbg);
	mbedtls_ssl_config_init(conf);
	mbedtls_ssl_init(ssl);
	mbedtls_x509_crt_init(cacert);
	mbedtls_x509_crt_init(clicert);
	mbedtls_pk_init(clikey);

	int ret = mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func,
					entropy, NULL, 0);
	if (ret != 0) {
		mbedtls_ssl_free(ssl);
		mbedtls_ssl_config_free(conf);
		mbedtls_ctr_drbg_free(ctr_drbg);
		mbedtls_entropy_free(entropy);
		mbedtls_x509_crt_free(cacert);
		mbedtls_x509_crt_free(clicert);
		mbedtls_pk_free(clikey);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(entropy);
		OVE_BACKEND_FREE(ctr_drbg);
		OVE_BACKEND_FREE(conf);
		OVE_BACKEND_FREE(ssl);
		OVE_BACKEND_FREE(cacert);
		OVE_BACKEND_FREE(clicert);
		OVE_BACKEND_FREE(clikey);
#endif
		return OVE_ERR_NOT_SUPPORTED;
	}

	t->ssl         = ssl;
	t->ssl_ctx     = NULL; /* set during handshake */
	t->conf        = conf;
	t->entropy     = entropy;
	t->ctr_drbg    = ctr_drbg;
	t->cacert      = cacert;
	t->client_cert = clicert;
	t->client_key  = clikey;
	t->sock        = NULL;

	*tls = t;
	return OVE_OK;
}

void ove_tls_deinit(ove_tls_t tls)
{
	if (!tls) return;
	struct ove_tls *t = tls;

	if (t->ssl) {
		mbedtls_ssl_free(t->ssl);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(t->ssl);
#endif
	}
	if (t->conf) {
		mbedtls_ssl_config_free(t->conf);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(t->conf);
#endif
	}
	if (t->cacert) {
		mbedtls_x509_crt_free(t->cacert);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(t->cacert);
#endif
	}
	if (t->client_cert) {
		mbedtls_x509_crt_free(t->client_cert);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(t->client_cert);
#endif
	}
	if (t->client_key) {
		mbedtls_pk_free(t->client_key);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(t->client_key);
#endif
	}
	if (t->ctr_drbg) {
		mbedtls_ctr_drbg_free(t->ctr_drbg);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(t->ctr_drbg);
#endif
	}
	if (t->entropy) {
		mbedtls_entropy_free(t->entropy);
#ifndef CONFIG_OVE_ZERO_HEAP
		OVE_BACKEND_FREE(t->entropy);
#endif
	}

	memset(t, 0, sizeof(*t));
}

int ove_tls_handshake(ove_tls_t tls, ove_socket_t sock,
		      const ove_tls_config_t *cfg)
{
	if (!tls || !sock) return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = tls;

	mbedtls_ssl_config *conf = t->conf;
	mbedtls_ssl_context *ssl = t->ssl;

	int ret = mbedtls_ssl_config_defaults(conf,
		MBEDTLS_SSL_IS_CLIENT,
		MBEDTLS_SSL_TRANSPORT_STREAM,
		MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0) return OVE_ERR_NOT_SUPPORTED;

	mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, t->ctr_drbg);

	/* Certificate verification */
	if (cfg && cfg->ca_cert && cfg->ca_cert_len > 0) {
		ret = mbedtls_x509_crt_parse(t->cacert, cfg->ca_cert,
					     cfg->ca_cert_len);
		if (ret != 0) return OVE_ERR_NOT_SUPPORTED;
		mbedtls_ssl_conf_ca_chain(conf, t->cacert, NULL);
		mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	} else {
		mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
	}

	/* Client certificate for mutual TLS (mTLS) */
	if (cfg && cfg->client_cert && cfg->client_cert_len > 0 &&
	    cfg->client_key && cfg->client_key_len > 0) {
		ret = mbedtls_x509_crt_parse(t->client_cert,
					     cfg->client_cert,
					     cfg->client_cert_len);
		if (ret != 0) return OVE_ERR_NOT_SUPPORTED;

		ret = mbedtls_pk_parse_key(t->client_key,
					   cfg->client_key,
					   cfg->client_key_len,
					   NULL, 0,
					   mbedtls_ctr_drbg_random,
					   t->ctr_drbg);
		if (ret != 0) return OVE_ERR_NOT_SUPPORTED;

		ret = mbedtls_ssl_conf_own_cert(conf, t->client_cert,
						t->client_key);
		if (ret != 0) return OVE_ERR_NOT_SUPPORTED;
	}

	ret = mbedtls_ssl_setup(ssl, conf);
	if (ret != 0) return OVE_ERR_NOT_SUPPORTED;

	/* SNI hostname */
	if (cfg && cfg->hostname) {
		mbedtls_ssl_set_hostname(ssl, cfg->hostname);
	}

	/* Wire BIO callbacks to our socket */
	t->sock = sock;
	mbedtls_ssl_set_bio(ssl, sock, bio_send, bio_recv, NULL);

	/* Perform handshake */
	while ((ret = mbedtls_ssl_handshake(ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
		    ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			return OVE_ERR_NOT_SUPPORTED;
	}

	return OVE_OK;
}

int ove_tls_send(ove_tls_t tls, const void *data, size_t len,
		 size_t *sent)
{
	if (!tls || !data) return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = tls;

	int ret = mbedtls_ssl_write(t->ssl, data, len);
	if (ret < 0) return OVE_ERR_NOT_SUPPORTED;
	if (sent) *sent = (size_t)ret;
	return OVE_OK;
}

int ove_tls_recv(ove_tls_t tls, void *buf, size_t len,
		 size_t *received)
{
	if (!tls || !buf) return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = tls;

	int ret = mbedtls_ssl_read(t->ssl, buf, len);
	if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
		return OVE_ERR_NET_CLOSED;
	if (ret < 0) return OVE_ERR_NOT_SUPPORTED;
	if (received) *received = (size_t)ret;
	return OVE_OK;
}

void ove_tls_close(ove_tls_t tls)
{
	if (!tls) return;
	struct ove_tls *t = tls;
	mbedtls_ssl_close_notify(t->ssl);
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_tls_create(ove_tls_t *tls)
{
	if (!tls) return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t) return OVE_ERR_NO_MEMORY;
	ove_tls_storage_t *storage = (ove_tls_storage_t *)t;
	int ret = ove_tls_init(tls, storage);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(t);
		return ret;
	}
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_tls_destroy(ove_tls_t tls)
{
	if (!tls) return;
	ove_tls_deinit(tls);
	OVE_BACKEND_FREE(tls);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */
