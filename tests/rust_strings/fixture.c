/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/ove.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

int ove_hal_entropy_fill(void *data, size_t len)
{
	return getrandom(data, len, 0) == (ssize_t)len ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}
int ove_sntp_get_utc(uint32_t *utc)
{
	*utc = 1893456000u;
	return OVE_OK;
}
static int entropy(void *ctx, unsigned char *data, size_t len)
{
	(void)ctx;
	return ove_hal_entropy_fill(data, len) == OVE_OK ? 0 : -1;
}
static int bio_send(void *ctx, const unsigned char *data, size_t len)
{
	ssize_t n = send(*(int *)ctx, data, len, MSG_NOSIGNAL);
	return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}
static int bio_recv(void *ctx, unsigned char *data, size_t len)
{
	struct pollfd p = {.fd = *(int *)ctx, .events = POLLIN};
	if (poll(&p, 1, 5000) <= 0)
		return MBEDTLS_ERR_SSL_TIMEOUT;
	ssize_t n = recv(p.fd, data, len, 0);
	return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
}
static unsigned char *pem(const char *name, size_t *len)
{
	char path[512];
	(void)snprintf(path, sizeof(path), "%s/%s", OVE_TEST_TLS_PKI_DIR, name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long size = ftell(f);
	if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	static unsigned char cert_storage[8192], key_storage[8192];
	unsigned char *bytes = strcmp(name, "srv.pem") == 0 ? cert_storage : key_storage;
	if ((size_t)size >= sizeof(cert_storage)) {
		fclose(f);
		return NULL;
	}
	if (!bytes || fread(bytes, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	bytes[size] = 0;
	*len = (size_t)size + 1;
	return bytes;
}
static int listener, handshake_result, saw_localhost;
static pthread_t server_thread;
static int sni(void *ctx, mbedtls_ssl_context *ssl, const unsigned char *name, size_t len)
{
	(void)ctx;
	(void)ssl;
	saw_localhost = len == 9 && memcmp(name, "localhost", 9) == 0;
	return 0;
}
static void *serve(void *unused)
{
	(void)unused;
	struct pollfd p = {.fd = listener, .events = POLLIN};
	int fd = poll(&p, 1, 10000) > 0 ? accept(listener, NULL, NULL) : -1;
	close(listener);
	if (fd < 0)
		return NULL;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&cert);
	mbedtls_pk_init(&key);
	size_t cert_len = 0, key_len = 0;
	unsigned char *cert_bytes = pem("srv.pem", &cert_len),
		      *key_bytes = pem("srv.key", &key_len);
	if (cert_bytes && key_bytes && mbedtls_ctr_drbg_seed(&drbg, entropy, NULL, NULL, 0) == 0 &&
	    mbedtls_x509_crt_parse(&cert, cert_bytes, cert_len) == 0 &&
	    mbedtls_pk_parse_key(&key, key_bytes, key_len, NULL, 0, mbedtls_ctr_drbg_random,
				 &drbg) == 0 &&
	    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) == 0) {
		mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
		mbedtls_ssl_conf_sni(&conf, sni, NULL);
		if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) == 0 &&
		    mbedtls_ssl_setup(&ssl, &conf) == 0) {
			mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
			int rc;
			do {
				rc = mbedtls_ssl_handshake(&ssl);
			} while (rc == MBEDTLS_ERR_SSL_WANT_READ ||
				 rc == MBEDTLS_ERR_SSL_WANT_WRITE);
			handshake_result = rc;
			if (rc == 0) {
				unsigned char bytes[3];
				size_t got = 0;
				while (got < sizeof(bytes)) {
					int n = mbedtls_ssl_read(&ssl, bytes + got,
								 sizeof(bytes) - got);
					if (n <= 0)
						break;
					got += (size_t)n;
				}
				if (got == 3 && memcmp(bytes, "A\0B", 3) == 0) {
					int n = mbedtls_ssl_write(&ssl,
								  (const unsigned char *)"B\0A", 3);
					if (n != 3)
						handshake_result = -2;
				} else
					handshake_result = -3;
			}
		}
	}

	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_pk_free(&key);
	mbedtls_x509_crt_free(&cert);
	mbedtls_ctr_drbg_free(&drbg);
	close(fd);
	return NULL;
}
int ove_test_tls_start(void)
{
	handshake_result = -1;
	saw_localhost = 0;
	listener = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr = {.sin_family = AF_INET,
				   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	socklen_t len = sizeof(addr);
	if (listener < 0)
		return -1;
	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(listener, 1) != 0 ||
	    getsockname(listener, (struct sockaddr *)&addr, &len) != 0 ||
	    pthread_create(&server_thread, NULL, serve, NULL) != 0) {
		close(listener);
		return -1;
	}
	return ntohs(addr.sin_port);
}
int ove_test_tls_finish(void)
{
	pthread_join(server_thread, NULL);
	return handshake_result == 0 && saw_localhost ? 0 : -1;
}
