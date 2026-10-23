/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * TLS client tests on the host: the HTTP and MQTT clients and the shared TLS
 * layer, talking to an in-process mbedTLS server on 127.0.0.1 that uses the
 * test PKI in pki/ (CA, server "localhost", client "ove-client").
 *
 * The server is capped at TLS 1.2.  mbedTLS is built without
 * MBEDTLS_THREADING_C, and TLS 1.3 runs its key exchange through PSA, whose
 * key store is process-global; keeping the server on TLS 1.2 leaves PSA to
 * the client thread alone.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include "ove/ove.h"
#include "ove/net.h"
#include "ove/net_http.h"
#include "ove/net_mqtt.h"
#include "ove/net_sntp.h"
#include "ove/hal/hal_entropy.h"

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/oid.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Board services the TLS layer needs ────────────────────────────── */

int ove_hal_entropy_fill(void *buf, size_t len)
{
	return getrandom(buf, len, 0) == (ssize_t)len ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

/* Log lines go to the console only; there is no shell here. */
void ove_shell_log_tee(const char *data, size_t len)
{
	(void)data;
	(void)len;
}

/* A "synced" clock fixed inside the test certificates' validity
 * (2026-10-23 to 2126), so the host's own clock cannot fail a handshake. */
int ove_sntp_get_utc(uint32_t *utc_s)
{
	*utc_s = 1893456000u; /* 2030-01-01T00:00:00Z */
	return OVE_OK;
}

/* ── Test PKI (PEM, length including the NUL as mbedTLS wants) ─────── */

struct pem {
	unsigned char *data;
	size_t len;
};

static struct pem load_pem(const char *name)
{
	char path[512];
	(void)snprintf(path, sizeof(path), "%s/%s", OVE_TEST_TLS_PKI_DIR, name);
	FILE *f = fopen(path, "rb");
	assert_non_null(f);
	assert_int_equal(fseek(f, 0, SEEK_END), 0);
	long n = ftell(f);
	assert_true(n > 0);
	assert_int_equal(fseek(f, 0, SEEK_SET), 0);
	struct pem p = {.data = malloc((size_t)n + 1), .len = (size_t)n + 1};
	assert_non_null(p.data);
	assert_int_equal(fread(p.data, 1, (size_t)n, f), (size_t)n);
	p.data[n] = '\0';
	(void)fclose(f);
	return p;
}

static struct pem s_ca, s_srv_cert, s_srv_key, s_cli_cert, s_cli_key, s_other_key;

/* ── In-process TLS server ─────────────────────────────────────────── */

enum srv_mode { SRV_HTTP, SRV_MQTT };

struct srv {
	enum srv_mode mode;
	int require_client_cert;
	int lfd;
	uint16_t port;
	pthread_t tid;
	/* Results, read after the join. */
	int handshake_rc;
	char peer_cn[64];
	char publish_topic[32];
	size_t publish_payload_len;
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	ssize_t n = send(*(int *)ctx, buf, len, MSG_NOSIGNAL);
	return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	struct pollfd pfd = {.fd = *(int *)ctx, .events = POLLIN};
	if (poll(&pfd, 1, 5000) <= 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	ssize_t n = recv(*(int *)ctx, buf, len, 0);
	return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}

static int drbg_entropy(void *ctx, unsigned char *buf, size_t len)
{
	(void)ctx;
	return ove_hal_entropy_fill(buf, len) == OVE_OK ? 0 : -1;
}

static int read_exact(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		int n = mbedtls_ssl_read(ssl, buf + got, len - got);
		if (n <= 0)
			return -1;
		got += (size_t)n;
	}
	return 0;
}

/* Reads one MQTT control packet; returns the length of its fixed header
 * (type byte + remaining length) and sets *total to the whole packet's. */
static int read_mqtt_packet(mbedtls_ssl_context *ssl, unsigned char *buf, size_t size,
			    size_t *total)
{
	size_t hdr = 1, remaining = 0, shift = 0;
	if (read_exact(ssl, buf, 1) != 0)
		return -1;
	do {
		if (hdr > 4 || read_exact(ssl, buf + hdr, 1) != 0)
			return -1;
		remaining |= (size_t)(buf[hdr] & 0x7F) << shift;
		shift += 7;
	} while (buf[hdr++] & 0x80);
	if (hdr + remaining > size || read_exact(ssl, buf + hdr, remaining) != 0)
		return -1;
	*total = hdr + remaining;
	return (int)hdr;
}

static void serve(struct srv *s, mbedtls_ssl_context *ssl)
{
	if (s->mode == SRV_HTTP) {
		char req[1024];
		size_t got = 0;
		while (got < sizeof(req) - 1) {
			int n = mbedtls_ssl_read(ssl, (unsigned char *)req + got,
						 sizeof(req) - 1 - got);
			if (n <= 0)
				return;
			got += (size_t)n;
			req[got] = '\0';
			if (strstr(req, "\r\n\r\n"))
				break;
		}
		const char *who = s->peer_cn[0] ? s->peer_cn : "none";
		char resp[256];
		int len = snprintf(resp, sizeof(resp),
				   "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n"
				   "Connection: close\r\n\r\n%s",
				   strlen(who), who);
		(void)mbedtls_ssl_write(ssl, (const unsigned char *)resp, (size_t)len);
	} else {
		static unsigned char pkt[8192]; /* one server at a time */
		size_t total = 0;
		if (read_mqtt_packet(ssl, pkt, sizeof(pkt), &total) < 0 || pkt[0] != 0x10)
			return;
		static const unsigned char connack[] = {0x20, 0x02, 0x00, 0x00};
		if (mbedtls_ssl_write(ssl, connack, sizeof(connack)) != sizeof(connack))
			return;
		int hdr = read_mqtt_packet(ssl, pkt, sizeof(pkt), &total);
		if (hdr < 0 || (pkt[0] & 0xF0) != 0x30 || total < (size_t)hdr + 2)
			return;
		size_t tlen = ((size_t)pkt[hdr] << 8) | pkt[hdr + 1];
		if (tlen < sizeof(s->publish_topic) && (size_t)hdr + 2 + tlen <= total) {
			memcpy(s->publish_topic, pkt + hdr + 2, tlen);
			/* QoS 0: no packet identifier, the rest is payload. */
			s->publish_payload_len = total - (size_t)hdr - 2 - tlen;
		}
	}
}

static void *srv_main(void *arg)
{
	struct srv *s = arg;
	struct pollfd pfd = {.fd = s->lfd, .events = POLLIN};
	s->handshake_rc = -1;
	int ready = poll(&pfd, 1, 10000);
	int fd = ready > 0 ? accept(s->lfd, NULL, NULL) : -1;
	/* One connection per test: closing now also refuses a client that
	 * comes too late, instead of leaving it waiting for a handshake. */
	close(s->lfd);
	s->lfd = -1;
	if (fd < 0)
		return NULL;

	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt ca, cert;
	mbedtls_pk_context key;
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&ca);
	mbedtls_x509_crt_init(&cert);
	mbedtls_pk_init(&key);

	if (mbedtls_ctr_drbg_seed(&drbg, drbg_entropy, NULL, NULL, 0) == 0 &&
	    mbedtls_x509_crt_parse(&ca, s_ca.data, s_ca.len) == 0 &&
	    mbedtls_x509_crt_parse(&cert, s_srv_cert.data, s_srv_cert.len) == 0 &&
	    mbedtls_pk_parse_key(&key, s_srv_key.data, s_srv_key.len, NULL, 0,
				 mbedtls_ctr_drbg_random, &drbg) == 0 &&
	    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) == 0) {
		mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
		mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
		mbedtls_ssl_conf_authmode(&conf, s->require_client_cert
							 ? MBEDTLS_SSL_VERIFY_REQUIRED
							 : MBEDTLS_SSL_VERIFY_NONE);
		if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) == 0 &&
		    mbedtls_ssl_setup(&ssl, &conf) == 0) {
			mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
			int rc;
			do {
				rc = mbedtls_ssl_handshake(&ssl);
			} while (rc == MBEDTLS_ERR_SSL_WANT_READ ||
				 rc == MBEDTLS_ERR_SSL_WANT_WRITE);
			s->handshake_rc = rc;
			if (rc == 0) {
				const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&ssl);
				if (peer) {
					const mbedtls_x509_name *n = &peer->subject;
					for (; n; n = n->next) {
						if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) ==
							    0 &&
						    n->val.len < sizeof(s->peer_cn)) {
							memcpy(s->peer_cn, n->val.p, n->val.len);
							break;
						}
					}
				}
				serve(s, &ssl);
				(void)mbedtls_ssl_close_notify(&ssl);
			}
		}
	}

	mbedtls_pk_free(&key);
	mbedtls_x509_crt_free(&cert);
	mbedtls_x509_crt_free(&ca);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ssl_free(&ssl);
	close(fd);
	return NULL;
}

static void srv_start(struct srv *s, enum srv_mode mode, int require_client_cert)
{
	memset(s, 0, sizeof(*s));
	s->mode = mode;
	s->require_client_cert = require_client_cert;
	s->lfd = socket(AF_INET, SOCK_STREAM, 0);
	assert_true(s->lfd >= 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	assert_int_equal(bind(s->lfd, (struct sockaddr *)&a, sizeof(a)), 0);
	assert_int_equal(listen(s->lfd, 1), 0);
	socklen_t alen = sizeof(a);
	assert_int_equal(getsockname(s->lfd, (struct sockaddr *)&a, &alen), 0);
	s->port = ntohs(a.sin_port);
	assert_int_equal(pthread_create(&s->tid, NULL, srv_main, s), 0);
}

static void srv_join(struct srv *s)
{
	pthread_join(s->tid, NULL);
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static void test_https_get_verifies_server(void **state)
{
	(void)state;
	struct srv s;
	srv_start(&s, SRV_HTTP, 0);
	ove_http_set_tls(s_ca.data, s_ca.len, 0);

	ove_http_client_storage_t st;
	ove_http_client_t c = NULL;
	assert_int_equal(ove_http_client_init(&c, &st), OVE_OK);
	char url[64];
	(void)snprintf(url, sizeof(url), "https://localhost:%u/", s.port);
	ove_http_response_t resp = {0};
	int rc = ove_http_get(c, url, &resp);
	int status = resp.status;
	char body[16] = "";
	if (rc == OVE_OK && resp.body)
		(void)snprintf(body, sizeof(body), "%s", resp.body);
	ove_http_response_free(&resp);
	ove_http_client_deinit(c);
	srv_join(&s);

	assert_int_equal(rc, OVE_OK);
	assert_int_equal(status, 200);
	assert_int_equal(s.handshake_rc, 0);
	assert_string_equal(body, "none");
}

/* GET https://localhost:<port>/ with the process-wide TLS policy; the
 * server answers with the client certificate's CN, or "none". */
static int https_get(uint16_t port, int *status, char *body, size_t body_size)
{
	ove_http_client_storage_t st;
	ove_http_client_t c = NULL;
	assert_int_equal(ove_http_client_init(&c, &st), OVE_OK);
	char url[64];
	(void)snprintf(url, sizeof(url), "https://localhost:%u/", port);
	ove_http_response_t resp = {0};
	int rc = ove_http_get(c, url, &resp);
	*status = resp.status;
	body[0] = '\0';
	if (rc == OVE_OK && resp.body)
		(void)snprintf(body, body_size, "%s", resp.body);
	ove_http_response_free(&resp);
	ove_http_client_deinit(c);
	return rc;
}

static void test_https_presents_client_cert(void **state)
{
	(void)state;
	struct srv s;
	srv_start(&s, SRV_HTTP, 1);
	ove_http_set_tls(s_ca.data, s_ca.len, 0);
	assert_int_equal(ove_http_set_client_cert(s_cli_cert.data, s_cli_cert.len, s_cli_key.data,
						  s_cli_key.len),
			 OVE_OK);

	int status = 0;
	char body[32];
	int rc = https_get(s.port, &status, body, sizeof(body));
	assert_int_equal(ove_http_set_client_cert(NULL, 0, NULL, 0), OVE_OK);
	srv_join(&s);

	assert_int_equal(rc, OVE_OK);
	assert_int_equal(status, 200);
	assert_int_equal(s.handshake_rc, 0);
	assert_string_equal(s.peer_cn, "ove-client");
	assert_string_equal(body, "ove-client");
}

static void test_https_without_client_cert_is_refused(void **state)
{
	(void)state;
	struct srv s;
	srv_start(&s, SRV_HTTP, 1);
	ove_http_set_tls(s_ca.data, s_ca.len, 0);

	int status = 0;
	char body[32];
	int rc = https_get(s.port, &status, body, sizeof(body));
	srv_join(&s);

	assert_int_not_equal(rc, OVE_OK);
	assert_int_not_equal(s.handshake_rc, 0);
}

static void test_http_set_client_cert_needs_both(void **state)
{
	(void)state;
	assert_int_equal(ove_http_set_client_cert(s_cli_cert.data, s_cli_cert.len, NULL, 0),
			 OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_http_set_client_cert(NULL, 0, s_cli_key.data, s_cli_key.len),
			 OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_http_set_client_cert(s_cli_cert.data, 0, s_cli_key.data,
						  s_cli_key.len),
			 OVE_ERR_INVALID_PARAM);
}

/* Caught on the device before the handshake, not by the server. */
static void test_https_mismatched_client_key(void **state)
{
	(void)state;
	struct srv s;
	srv_start(&s, SRV_HTTP, 1);
	ove_http_set_tls(s_ca.data, s_ca.len, 0);
	assert_int_equal(ove_http_set_client_cert(s_cli_cert.data, s_cli_cert.len, s_other_key.data,
						  s_other_key.len),
			 OVE_OK);

	int status = 0;
	char body[32];
	int rc = https_get(s.port, &status, body, sizeof(body));
	assert_int_equal(ove_http_set_client_cert(NULL, 0, NULL, 0), OVE_OK);
	srv_join(&s);

	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
}

/* mbedTLS reads a buffer as PEM only when its length counts the NUL. */
static void test_https_pem_length_without_nul(void **state)
{
	(void)state;
	struct srv s;
	srv_start(&s, SRV_HTTP, 1);
	ove_http_set_tls(s_ca.data, s_ca.len, 0);
	assert_int_equal(ove_http_set_client_cert(s_cli_cert.data, s_cli_cert.len - 1,
						  s_cli_key.data, s_cli_key.len),
			 OVE_OK);

	int status = 0;
	char body[32];
	int rc = https_get(s.port, &status, body, sizeof(body));
	assert_int_equal(ove_http_set_client_cert(NULL, 0, NULL, 0), OVE_OK);
	srv_join(&s);

	assert_int_equal(rc, OVE_ERR_NOT_SUPPORTED);
}

/* The session outlives ove_mqtt_connect(): every later send must still find
 * its TLS state (ASan reports it if that state lived on connect's stack). */
static void test_mqtt_tls_connect_publish(void **state)
{
	(void)state;
	struct srv s;
	srv_start(&s, SRV_MQTT, 0);

	ove_mqtt_client_storage_t st;
	ove_mqtt_client_t c = NULL;
	assert_int_equal(ove_mqtt_client_init(&c, &st), OVE_OK);
	ove_mqtt_config_t cfg = {
		.host = "localhost",
		.port = s.port,
		.client_id = "ove-test",
		.use_tls = 1,
		.tls_ca_cert = s_ca.data,
		.tls_ca_cert_len = s_ca.len,
	};
	int rc_connect = ove_mqtt_connect(c, &cfg);
	int rc_publish = rc_connect == OVE_OK ? ove_mqtt_publish(c, "ove/t", "x", 1, OVE_MQTT_QOS0)
					      : rc_connect;
	ove_mqtt_disconnect(c);
	ove_mqtt_client_deinit(c);
	srv_join(&s);

	assert_int_equal(rc_connect, OVE_OK);
	assert_int_equal(rc_publish, OVE_OK);
	assert_int_equal(s.handshake_rc, 0);
	assert_string_equal(s.publish_topic, "ove/t");
}

/* A packet larger than one TLS record (4 KB, ove_mbedtls_config.h) must
 * reach the broker whole. */
static void test_mqtt_tls_publish_spans_records(void **state)
{
	(void)state;
	struct srv s;
	srv_start(&s, SRV_MQTT, 0);

	ove_mqtt_client_storage_t st;
	ove_mqtt_client_t c = NULL;
	assert_int_equal(ove_mqtt_client_init(&c, &st), OVE_OK);
	ove_mqtt_config_t cfg = {
		.host = "localhost",
		.port = s.port,
		.client_id = "ove-test",
		.use_tls = 1,
		.tls_ca_cert = s_ca.data,
		.tls_ca_cert_len = s_ca.len,
	};
	static char payload[6000];
	memset(payload, 'p', sizeof(payload));
	int rc_connect = ove_mqtt_connect(c, &cfg);
	int rc_publish = rc_connect == OVE_OK ? ove_mqtt_publish(c, "ove/big", payload,
								 sizeof(payload), OVE_MQTT_QOS0)
					      : rc_connect;
	ove_mqtt_disconnect(c);
	ove_mqtt_client_deinit(c);
	srv_join(&s);

	assert_int_equal(rc_connect, OVE_OK);
	assert_int_equal(rc_publish, OVE_OK);
	assert_string_equal(s.publish_topic, "ove/big");
	assert_int_equal(s.publish_payload_len, sizeof(payload));
}

int main(void)
{
	/* A handshake that never completes must not hang the test run. */
	alarm(120);

	s_ca = load_pem("ca.pem");
	s_srv_cert = load_pem("srv.pem");
	s_srv_key = load_pem("srv.key");
	s_cli_cert = load_pem("cli.pem");
	s_cli_key = load_pem("cli.key");
	s_other_key = load_pem("other.key");

	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_https_get_verifies_server),
		cmocka_unit_test(test_mqtt_tls_connect_publish),
		cmocka_unit_test(test_mqtt_tls_publish_spans_records),
		cmocka_unit_test(test_https_presents_client_cert),
		cmocka_unit_test(test_https_without_client_cert_is_refused),
		cmocka_unit_test(test_http_set_client_cert_needs_both),
		cmocka_unit_test(test_https_mismatched_client_key),
		cmocka_unit_test(test_https_pem_length_without_nul),
	};
	int failed = cmocka_run_group_tests_name("net_tls", tests, NULL, NULL);

	free(s_ca.data);
	free(s_srv_cert.data);
	free(s_srv_key.data);
	free(s_cli_cert.data);
	free(s_cli_key.data);
	free(s_other_key.data);
	return failed;
}
