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
#include "ove/log.h"
#include "ove/net_tls.h"
#include "ove/net_sntp.h"
#include "ove/hal/hal_entropy.h"
#include "ove_backend_common.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/aes.h"
#include "mbedtls/threading.h"
#include "mbedtls/asn1.h"
#include "mbedtls/bignum.h"
#include "mbedtls/cipher.h"
#include "mbedtls/dhm.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/pem.h"
#include "psa/crypto.h"
#ifdef CONFIG_OVE_ZERO_HEAP
#include "mbedtls/memory_buffer_alloc.h"
#endif

#include <string.h>

#ifdef CONFIG_OVE_ZERO_HEAP
#ifndef CONFIG_OVE_NET_TLS_HEAP_SIZE
#define CONFIG_OVE_NET_TLS_HEAP_SIZE 49152
#endif
static unsigned char s_mbedtls_heap[CONFIG_OVE_NET_TLS_HEAP_SIZE];
#endif

/* ---------- One lock for mbedTLS's shared state ---------- */

/*
 * Sessions on different threads share the PSA key store and random
 * generator, the lazily built AES and cipher-suite tables and, in zero-heap
 * builds, the memory pool.  mbedTLS guards its shared state with mutexes
 * (MBEDTLS_THREADING_ALT, ove_mbedtls_config.h); all of them are this one
 * recursive mutex, since mbedTLS takes some inside others (the key store's
 * around an allocation, the random generator's around reseeding).  It is
 * held only inside mbedTLS, never across socket I/O, so an idle session
 * blocks no other; the longest hold is an RSA private-key operation.
 */
static ove_mutex_storage_t s_lock_storage;
static ove_mutex_t s_lock;
static int s_tls_ready;

static void tls_mutex_init(mbedtls_threading_mutex_t *m)
{
	(void)m;
}

static void tls_mutex_free(mbedtls_threading_mutex_t *m)
{
	(void)m;
}

static int tls_mutex_lock(mbedtls_threading_mutex_t *m)
{
	(void)m;
	return ove_recursive_mutex_lock(s_lock, OVE_WAIT_FOREVER) == OVE_OK
		       ? 0
		       : MBEDTLS_ERR_THREADING_MUTEX_ERROR;
}

static int tls_mutex_unlock(mbedtls_threading_mutex_t *m)
{
	(void)m;
	ove_recursive_mutex_unlock(s_lock);
	return 0;
}

/*
 * Before any thread can start a session: until mbedtls_threading_set_alt(),
 * every mbedTLS lock fails; the pool must exist before the first
 * allocation; and the tables mbedTLS builds on first use are built here, as
 * nothing guards their construction.  A constructor, so no session can race
 * it; ove_tls_init() runs it too, for a session set up by another
 * constructor (OVE_TLS_DEFINE_STATIC) that the linker ordered first.  Both
 * happen before any thread starts, so the flag needs no lock.
 */
__attribute__((used, constructor)) static void tls_global_init(void)
{
	if (s_tls_ready)
		return;
	if (ove_recursive_mutex_init(&s_lock, &s_lock_storage) != OVE_OK)
		return; /* ove_tls_init() reports it */
	mbedtls_threading_set_alt(tls_mutex_init, tls_mutex_free, tls_mutex_lock, tls_mutex_unlock);
#ifdef CONFIG_OVE_ZERO_HEAP
	mbedtls_memory_buffer_alloc_init(s_mbedtls_heap, sizeof(s_mbedtls_heap));
#endif
	mbedtls_aes_context aes;
	static const unsigned char zero_key[16];
	mbedtls_aes_init(&aes);
	(void)mbedtls_aes_setkey_enc(&aes, zero_key, 128); /* AES tables, AES-NI probe */
	mbedtls_aes_free(&aes);
	(void)mbedtls_ecp_grp_id_list();
	(void)mbedtls_ssl_list_ciphersuites();
	s_tls_ready = 1;
}

/* ---------- BIO callbacks for mbedTLS ---------- */

/* BIO error codes — removed from mbedTLS 3.x (were in net_sockets.h) */
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED (-0x004E)
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED (-0x004C)
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
	if (ret != OVE_OK)
		return MBEDTLS_ERR_NET_SEND_FAILED;
	return (int)sent;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	ove_socket_t sock = (ove_socket_t)ctx;
	size_t received = 0;
	int ret = ove_socket_recv(sock, buf, len, &received, OVE_WAIT_FOREVER);
	if (ret == OVE_ERR_NET_CLOSED)
		return 0;
	if (ret != OVE_OK)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return (int)received;
}

/* ---------- TLS session ---------- */

/* mbedTLS's entropy source (MBEDTLS_ENTROPY_HARDWARE_ALT): the board's. The
 * prototype lives in a private mbedTLS header. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	(void)data;
	if (ove_hal_entropy_fill(output, len) != OVE_OK) {
		*olen = 0;
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	}
	*olen = len;
	return 0;
}

/* Certificate validity is checked against UTC from SNTP (mbedtls_time, see
 * config/mbedtls/ove_mbedtls_config.h). Before a sync this reports 1970, so
 * every certificate looks not-yet-valid: TLS fails closed. */
time_t ove_mbedtls_time(time_t *timer)
{
	uint32_t utc = 0;
	/* Before a sync this fails and utc stays 0, which fails TLS closed (above). */
	int rc = ove_sntp_get_utc(&utc);
	(void)rc;
	if (timer)
		*timer = (time_t)utc;
	return (time_t)utc;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	uint64_t us = 0;
	ove_time_get_us(&us);
	return (mbedtls_ms_time_t)(us / 1000u);
}

/* Whether a negative mbedTLS code means it ran out of memory: the heap, or
 * the zero-heap pool.  A code is a module's high-level error (bits 7-14)
 * plus the low-level one beneath it (bits 0-6), so a certificate parse that
 * failed allocating in ASN.1 or bignum reports X.509 and the cause.  PSA
 * statuses (-128 to -255) are not built that way and pass through some
 * paths as they are. */
static int tls_out_of_memory(int ret)
{
	if (ret <= -0x80 && ret >= -0xFF)
		return ret == PSA_ERROR_INSUFFICIENT_MEMORY;
	unsigned int high = (unsigned int)-ret & 0xFF80u;
	unsigned int low = (unsigned int)-ret & 0x007Fu;
	return high == (unsigned int)-MBEDTLS_ERR_SSL_ALLOC_FAILED ||
	       high == (unsigned int)-MBEDTLS_ERR_X509_ALLOC_FAILED ||
	       high == (unsigned int)-MBEDTLS_ERR_PK_ALLOC_FAILED ||
	       high == (unsigned int)-MBEDTLS_ERR_PEM_ALLOC_FAILED ||
	       high == (unsigned int)-MBEDTLS_ERR_ECP_ALLOC_FAILED ||
	       high == (unsigned int)-MBEDTLS_ERR_DHM_ALLOC_FAILED ||
	       high == (unsigned int)-MBEDTLS_ERR_CIPHER_ALLOC_FAILED ||
	       high == (unsigned int)-MBEDTLS_ERR_MD_ALLOC_FAILED ||
	       low == (unsigned int)-MBEDTLS_ERR_MPI_ALLOC_FAILED ||
	       low == (unsigned int)-MBEDTLS_ERR_ASN1_ALLOC_FAILED;
}

/* An mbedTLS failure: OVE_ERR_NO_MEMORY when it ran out of memory, otherwise
 * OVE_ERR_NOT_SUPPORTED; the log keeps the code that says which it was. */
static int tls_failed(const char *what, int ret)
{
	if (ret > 0) { /* a PEM bundle in which this many certificates failed */
		OVE_LOG_ERR("TLS: %s failed for %d certificate(s)", what, ret);
		return OVE_ERR_NOT_SUPPORTED;
	}
	OVE_LOG_ERR("TLS: %s failed: -0x%04x", what, (unsigned int)-ret);
	return tls_out_of_memory(ret) ? OVE_ERR_NO_MEMORY : OVE_ERR_NOT_SUPPORTED;
}

int ove_tls_init(ove_tls_t *tls, ove_tls_storage_t *storage)
{
	if (!tls || !storage)
		return OVE_ERR_INVALID_PARAM;
	tls_global_init();
	if (!s_tls_ready) {
		OVE_LOG_ERR("TLS: could not create the lock that guards mbedTLS");
		return OVE_ERR_NOT_SUPPORTED;
	}
	struct ove_tls *t = (struct ove_tls *)storage;
	memset(t, 0, sizeof(*t));

#ifdef CONFIG_OVE_ZERO_HEAP
	mbedtls_entropy_context *entropy = &t->_entropy;
	mbedtls_ctr_drbg_context *ctr_drbg = &t->_ctr_drbg;
	mbedtls_ssl_config *conf = &t->_conf;
	mbedtls_ssl_context *ssl = &t->_ssl;
	mbedtls_x509_crt *cacert = &t->_cacert;
	mbedtls_x509_crt *clicert = &t->_client_cert;
	mbedtls_pk_context *clikey = &t->_client_key;
#else
	mbedtls_entropy_context *entropy = OVE_BACKEND_MALLOC(sizeof(*entropy));
	mbedtls_ctr_drbg_context *ctr_drbg = OVE_BACKEND_MALLOC(sizeof(*ctr_drbg));
	mbedtls_ssl_config *conf = OVE_BACKEND_MALLOC(sizeof(*conf));
	mbedtls_ssl_context *ssl = OVE_BACKEND_MALLOC(sizeof(*ssl));
	mbedtls_x509_crt *cacert = OVE_BACKEND_MALLOC(sizeof(*cacert));
	mbedtls_x509_crt *clicert = OVE_BACKEND_MALLOC(sizeof(*clicert));
	mbedtls_pk_context *clikey = OVE_BACKEND_MALLOC(sizeof(*clikey));

	if (!entropy || !ctr_drbg || !conf || !ssl || !cacert || !clicert || !clikey) {
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

	int ret = mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func, entropy, NULL, 0);
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
		return tls_failed("random generator seed", ret);
	}

	t->ssl = ssl;
	t->ssl_ctx = NULL; /* set during handshake */
	t->conf = conf;
	t->entropy = entropy;
	t->ctr_drbg = ctr_drbg;
	t->cacert = cacert;
	t->client_cert = clicert;
	t->client_key = clikey;
	t->sock = NULL;

	*tls = t;
	return OVE_OK;
}

void ove_tls_deinit(ove_tls_t tls)
{
	if (!tls)
		return;
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

int ove_tls_handshake(ove_tls_t tls, ove_socket_t sock, const ove_tls_config_t *cfg)
{
	if (!tls || !sock)
		return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = tls;

	int have_cert = cfg && cfg->client_cert && cfg->client_cert_len > 0;
	int have_key = cfg && cfg->client_key && cfg->client_key_len > 0;
	if (have_cert != have_key) {
		OVE_LOG_ERR("TLS: client certificate and key must be given together");
		return OVE_ERR_INVALID_PARAM;
	}
	/* Verifying a certificate says nothing about whose it is without the
	 * name to check it against; mbedTLS (since 3.6.3) fails such a
	 * handshake, so refuse it here, with a clear error, before any I/O. */
	if (cfg && cfg->ca_cert && cfg->ca_cert_len > 0 && !cfg->hostname) {
		OVE_LOG_ERR("TLS: a CA certificate needs a hostname to verify against");
		return OVE_ERR_INVALID_PARAM;
	}

	mbedtls_ssl_config *conf = t->conf;
	mbedtls_ssl_context *ssl = t->ssl;

	int ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
					      MBEDTLS_SSL_TRANSPORT_STREAM,
					      MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0)
		return tls_failed("configuration", ret);

	mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, t->ctr_drbg);

	/* Certificate verification: require a CA cert, or an explicit
	 * allow_insecure opt-in. Refusing silent verify-none eliminates
	 * the default MITM exposure. */
	if (cfg && cfg->ca_cert && cfg->ca_cert_len > 0) {
		ret = mbedtls_x509_crt_parse(t->cacert, cfg->ca_cert, cfg->ca_cert_len);
		if (ret != 0)
			return tls_failed("CA certificate parse", ret);
		mbedtls_ssl_conf_ca_chain(conf, t->cacert, NULL);
		mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	} else if (cfg && cfg->allow_insecure) {
		OVE_LOG_WRN("TLS: allow_insecure set, peer certificate not verified");
		mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
	} else {
		OVE_LOG_ERR("TLS: refusing handshake — no CA cert and allow_insecure not set");
		return OVE_ERR_INVALID_PARAM;
	}

	/* Client certificate for mutual TLS (mTLS) */
	if (have_cert) {
		ret = mbedtls_x509_crt_parse(t->client_cert, cfg->client_cert,
					     cfg->client_cert_len);
		if (ret != 0)
			return tls_failed("client certificate parse", ret);

		ret = mbedtls_pk_parse_key(t->client_key, cfg->client_key, cfg->client_key_len,
					   NULL, 0, mbedtls_ctr_drbg_random, t->ctr_drbg);
		if (ret != 0)
			return tls_failed("client key parse", ret);

		/* Otherwise only the server notices, and over TLS 1.3 only after
		 * the handshake, on the first read. */
		const mbedtls_x509_crt *crt = t->client_cert;
		ret = mbedtls_pk_check_pair(&crt->pk, t->client_key, mbedtls_ctr_drbg_random,
					    t->ctr_drbg);
		if (ret < 0 && tls_out_of_memory(ret)) {
			OVE_LOG_ERR("TLS: out of memory checking the client key");
			return OVE_ERR_NO_MEMORY;
		}
		if (ret != 0) {
			OVE_LOG_ERR("TLS: client key does not match the certificate (-0x%04x)",
				    (unsigned int)-ret);
			return OVE_ERR_INVALID_PARAM;
		}

		ret = mbedtls_ssl_conf_own_cert(conf, t->client_cert, t->client_key);
		if (ret != 0)
			return tls_failed("client certificate setup", ret);
	}

	ret = mbedtls_ssl_setup(ssl, conf);
	if (ret != 0)
		return tls_failed("session setup", ret);

	/* SNI, and the name the server's certificate is checked against */
	if (cfg && cfg->hostname) {
		ret = mbedtls_ssl_set_hostname(ssl, cfg->hostname);
		if (ret != 0)
			return tls_failed("hostname", ret);
	}

	/* Wire BIO callbacks to our socket */
	t->sock = sock;
	mbedtls_ssl_set_bio(ssl, sock, bio_send, bio_recv, NULL);

	/* Perform handshake */
	while ((ret = mbedtls_ssl_handshake(ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			return tls_failed("handshake", ret);
	}

	return OVE_OK;
}

int ove_tls_send(ove_tls_t tls, const void *data, size_t len, size_t *sent)
{
	if (!tls || !data)
		return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = tls;

	int ret = mbedtls_ssl_write(t->ssl, data, len);
	if (ret < 0)
		return OVE_ERR_NOT_SUPPORTED;
	if (sent)
		*sent = (size_t)ret;
	return OVE_OK;
}

int ove_tls_recv(ove_tls_t tls, void *buf, size_t len, size_t *received)
{
	if (!tls || !buf)
		return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = tls;

	int ret = mbedtls_ssl_read(t->ssl, buf, len);
	if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
		return OVE_ERR_NET_CLOSED;
	if (ret < 0)
		return OVE_ERR_NOT_SUPPORTED;
	if (received)
		*received = (size_t)ret;
	return OVE_OK;
}

void ove_tls_close(ove_tls_t tls)
{
	if (!tls)
		return;
	struct ove_tls *t = tls;
	mbedtls_ssl_close_notify(t->ssl);
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_tls_create(ove_tls_t *tls)
{
	if (!tls)
		return OVE_ERR_INVALID_PARAM;
	struct ove_tls *t = OVE_BACKEND_MALLOC(sizeof(*t));
	if (!t)
		return OVE_ERR_NO_MEMORY;
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
	if (!tls)
		return;
	ove_tls_deinit(tls);
	OVE_BACKEND_FREE(tls);
}
#endif /* !CONFIG_OVE_ZERO_HEAP */
