/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Zero-heap TLS on the host: the shared TLS layer built as zero-heap
 * firmware builds it (cmake/OveMbedtls.cmake), mbedTLS allocating from the
 * CONFIG_OVE_NET_TLS_HEAP_SIZE pool.  Running out of that pool must fail
 * the handshake with OVE_ERR_NO_MEMORY wherever it happens, and sessions on
 * several threads must share it safely.
 *
 * The pool is the process's only mbedTLS allocator, so the server runs in
 * a forked child, whose copy of the pool is its own, and TLS 1.3 like a
 * real server.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include "ove/ove.h"
#include "ove/net.h"
#include "ove/net_tls.h"
#include "ove/net_sntp.h"
#include "ove/hal/hal_entropy.h"

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "psa/crypto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Board services the TLS layer needs ────────────────────────────── */

/* Deterministic (splitmix64), so every run makes the same keys and
 * signatures and so the same allocations: how much an ECDSA operation
 * needs varies with its numbers, and the sweep must not. */
static uint64_t s_rng_state = 0x6f76655f746c7321u;

int ove_hal_entropy_fill(void *buf, size_t len)
{
	unsigned char *p = buf;
	while (len > 0) {
		/* atomic: the concurrency test calls it from several threads */
		uint64_t z =
			__atomic_add_fetch(&s_rng_state, 0x9e3779b97f4a7c15u, __ATOMIC_RELAXED);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
		z ^= z >> 31;
		size_t n = len < sizeof(z) ? len : sizeof(z);
		memcpy(p, &z, n);
		p += n;
		len -= n;
	}
	return OVE_OK;
}

/* The last line logged: the mbedTLS code behind a failed handshake. */
static char s_last_log[160];
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

void ove_shell_log_tee(const char *data, size_t len)
{
	if (len >= sizeof(s_last_log))
		len = sizeof(s_last_log) - 1;
	(void)pthread_mutex_lock(&s_log_lock);
	memcpy(s_last_log, data, len);
	s_last_log[len] = '\0';
	(void)pthread_mutex_unlock(&s_log_lock);
}

/* Inside the test certificates' validity, as in test_net_tls.c. */
int ove_sntp_get_utc(uint32_t *utc_s)
{
	*utc_s = 1893456000u; /* 2030-01-01T00:00:00Z */
	return OVE_OK;
}

/* ── Test PKI (static: zero-heap builds refuse malloc) ─────────────── */

struct pem {
	unsigned char data[2048];
	size_t len; /* including the NUL, as mbedTLS wants */
};

static void load_pem(struct pem *p, const char *name)
{
	char path[512];
	(void)snprintf(path, sizeof(path), "%s/%s", OVE_TEST_TLS_PKI_DIR, name);
	FILE *f = fopen(path, "rb");
	assert_non_null(f);
	size_t n = fread(p->data, 1, sizeof(p->data) - 1, f);
	assert_true(n > 0 && feof(f));
	p->data[n] = '\0';
	p->len = n + 1;
	(void)fclose(f);
}

static struct pem s_ca, s_srv_cert, s_srv_key, s_cli_cert, s_cli_key;

/* ── Forked TLS server ─────────────────────────────────────────────── */

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

static void srv_serve(mbedtls_ssl_config *conf, int fd)
{
	mbedtls_ssl_context ssl;
	mbedtls_ssl_init(&ssl);
	if (mbedtls_ssl_setup(&ssl, conf) == 0) {
		mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
		int rc;
		do {
			rc = mbedtls_ssl_handshake(&ssl);
		} while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
		if (rc == 0)
			(void)mbedtls_ssl_close_notify(&ssl);
	}
	mbedtls_ssl_free(&ssl);
}

/* Handshakes every client that connects, requiring its certificate, until
 * none comes for 10 s.  Runs in the child, so it never returns. */
static void srv_run(int lfd, bool fork_each)
{
	/* The pool, a copy of the client's from the fork on, stays as it is:
	 * swapping in another would strand what mbedTLS already allocated from
	 * it (PSA's state, if the client has run a session), which mbedTLS
	 * frees when it reseeds PSA in a forked process. */
	s_rng_state ^= 0x5345525645520000u; /* not the client's stream */
	if (psa_crypto_init() != PSA_SUCCESS)
		_exit(2);

	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt ca, cert;
	mbedtls_pk_context key;
	mbedtls_ssl_config_init(&conf);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&ca);
	mbedtls_x509_crt_init(&cert);
	mbedtls_pk_init(&key);
	if (mbedtls_ctr_drbg_seed(&drbg, drbg_entropy, NULL, NULL, 0) != 0 ||
	    mbedtls_x509_crt_parse(&ca, s_ca.data, s_ca.len) != 0 ||
	    mbedtls_x509_crt_parse(&cert, s_srv_cert.data, s_srv_cert.len) != 0 ||
	    mbedtls_pk_parse_key(&key, s_srv_key.data, s_srv_key.len, NULL, 0,
				 mbedtls_ctr_drbg_random, &drbg) != 0 ||
	    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		_exit(2);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0)
		_exit(2);

	if (fork_each)
		(void)signal(SIGCHLD, SIG_IGN);
	for (;;) {
		struct pollfd pfd = {.fd = lfd, .events = POLLIN};
		if (poll(&pfd, 1, 10000) <= 0)
			_exit(0);
		int fd = accept(lfd, NULL, NULL);
		if (fd < 0)
			_exit(2);
		if (fork_each) {
			pid_t c = fork();
			if (c == 0) {
				srv_serve(&conf, fd);
				_exit(0);
			}
		} else {
			srv_serve(&conf, fd);
		}
		close(fd);
	}
}

/* @p fork_each: serve each connection in a process of its own, so clients
 * can handshake at the same time. */
static pid_t srv_start(uint16_t *port, bool fork_each)
{
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	assert_true(lfd >= 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	assert_int_equal(bind(lfd, (struct sockaddr *)&a, sizeof(a)), 0);
	assert_int_equal(listen(lfd, 4), 0);
	socklen_t alen = sizeof(a);
	assert_int_equal(getsockname(lfd, (struct sockaddr *)&a, &alen), 0);
	*port = ntohs(a.sin_port);
	pid_t pid = fork();
	assert_true(pid >= 0);
	if (pid == 0)
		srv_run(lfd, fork_each);
	close(lfd);
	return pid;
}

static void srv_stop(pid_t pid)
{
	(void)kill(pid, SIGTERM);
	(void)waitpid(pid, NULL, 0);
}

/* ── Tests ─────────────────────────────────────────────────────────── */

/* Set up by a constructor that the linker runs before ove_net_tls.c's, which
 * sets up mbedTLS's lock and pool: ove_tls_init must do that itself then. */
OVE_TLS_DEFINE_STATIC(s_static_tls);

static void test_static_session_before_tls_setup(void **state)
{
	(void)state;
	assert_non_null(s_static_tls);
}

/* One mutual-TLS handshake against the server; the session is torn down
 * before returning, so all it leaves in the pool is mbedTLS's global state. */
static int handshake(uint16_t port)
{
	ove_socket_storage_t sock_st;
	ove_socket_t sock = NULL;
	int rc = ove_socket_open(&sock, &sock_st, OVE_AF_INET, OVE_SOCK_STREAM);
	if (rc != OVE_OK)
		return rc; /* no assert: the concurrency test calls this off the main thread */
	ove_sockaddr_t addr = {.family = OVE_AF_INET, .port = port, .addr = {127, 0, 0, 1}};
	rc = ove_socket_connect(sock, &addr, OVE_MS(5000));
	if (rc != OVE_OK) {
		ove_socket_close(sock);
		return rc;
	}

	ove_tls_storage_t tls_st;
	ove_tls_t tls = NULL;
	rc = ove_tls_init(&tls, &tls_st);
	if (rc != OVE_OK) {
		ove_socket_close(sock);
		return rc;
	}
	ove_tls_config_t cfg = {
		.ca_cert = s_ca.data,
		.ca_cert_len = s_ca.len,
		.hostname = "localhost",
		.client_cert = s_cli_cert.data,
		.client_cert_len = s_cli_cert.len,
		.client_key = s_cli_key.data,
		.client_key_len = s_cli_key.len,
	};
	rc = ove_tls_handshake(tls, sock, &cfg);
	if (rc == OVE_OK)
		ove_tls_close(tls);
	ove_tls_deinit(tls);
	ove_socket_close(sock);
	return rc;
}

/* Takes everything in the pool except one contiguous run of @p budget
 * bytes, so a handshake has exactly that much to allocate from; the blocks
 * taken go in @p blocks, their count in @p n.  False when the pool has no
 * run that long. */
static bool pool_leave(size_t budget, void **blocks, size_t max_blocks, size_t *n)
{
	void *keep = mbedtls_calloc(1, budget);
	if (!keep)
		return false;
	*n = 0;
	for (size_t size = 4096; size >= 16; size /= 2) {
		void *b;
		while ((b = mbedtls_calloc(1, size)) != NULL) {
			assert_true(*n < max_blocks);
			blocks[(*n)++] = b;
		}
	}
	mbedtls_free(keep);
	return true;
}

static void pool_return(void **blocks, size_t n)
{
	while (n > 0)
		mbedtls_free(blocks[--n]);
}

/* Where mbedTLS 3.6 reports running out of memory as something else, which
 * the TLS layer cannot tell apart: an entropy source failure while seeding
 * the random generator, a key in no known format while parsing the client
 * key, a generic error in the TLS 1.3 key schedule (PSA's out-of-memory
 * status untranslated), a failed verification of the server's certificate,
 * a handshake failure when checking the server's handshake signature
 * (ssl_tls13_generic.c drops pk_verify_ext's code). */
static bool reported_otherwise(const char *log)
{
	return strstr(log, "random generator seed failed: -0x0034") ||
	       strstr(log, "client key parse failed: -0x3d00") ||
	       strstr(log, "handshake failed: -0x0001") ||
	       strstr(log, "handshake failed: -0x2700") || strstr(log, "handshake failed: -0x6e00");
}

/*
 * After one handshake has set up mbedTLS's global state, a session is
 * given 32 bytes of the pool, then 32 more each round, until its handshake
 * completes.  On the way it runs out at every stage that allocates
 * (seeding, parsing the CA, the client certificate and key, session setup,
 * key exchange, signing, verification), and each failure must be
 * OVE_ERR_NO_MEMORY or one of the above.  Completing at the end also shows
 * the failed sessions gave all their memory back.
 */
static void test_pool_exhaustion_is_no_memory(void **state)
{
	(void)state;
	uint16_t port;
	pid_t srv = srv_start(&port, false);

	assert_int_equal(handshake(port), OVE_OK);

	static void *blocks[4096];
	int out_of_memory = 0;
	int not_reported = 0;
	int completed = 0;
	for (size_t budget = 32; !completed && budget <= CONFIG_OVE_NET_TLS_HEAP_SIZE;
	     budget += 32) {
		size_t n = 0;
		assert_true(pool_leave(budget, blocks, sizeof(blocks) / sizeof(blocks[0]), &n));
		s_last_log[0] = '\0';
		int rc = handshake(port);
		pool_return(blocks, n);
		if (rc == OVE_OK) {
			completed = 1;
		} else if (rc == OVE_ERR_NO_MEMORY) {
			out_of_memory++;
		} else if (rc == OVE_ERR_NOT_SUPPORTED && reported_otherwise(s_last_log)) {
			not_reported++;
		} else {
			srv_stop(srv);
			fail_msg("%zu bytes: handshake returned %d, last log: %s", budget, rc,
				 s_last_log);
		}
	}
	srv_stop(srv);

	print_message("%d out of memory, %d reported by mbedTLS as other failures\n", out_of_memory,
		      not_reported);
	assert_true(completed);
	assert_true(out_of_memory > 0);
}

/* Four clients handshaking at once, every session allocating from the one
 * pool (and sharing the PSA key store).  Unguarded, two were enough to
 * corrupt the pool in every run: mbedTLS's allocator checks then call
 * exit(1). */
enum { CONC_THREADS = 4, CONC_ROUNDS = 5 };
static uint16_t s_conc_port;

static void *conc_client(void *arg)
{
	int *failed = arg;
	for (int i = 0; i < CONC_ROUNDS; i++)
		if (handshake(s_conc_port) != OVE_OK)
			(*failed)++;
	return NULL;
}

static void test_concurrent_sessions_share_the_pool(void **state)
{
	(void)state;
	pid_t srv = srv_start(&s_conc_port, true);
	pthread_t th[CONC_THREADS];
	int failed[CONC_THREADS] = {0};
	for (int i = 0; i < CONC_THREADS; i++)
		assert_int_equal(pthread_create(&th[i], NULL, conc_client, &failed[i]), 0);
	for (int i = 0; i < CONC_THREADS; i++)
		assert_int_equal(pthread_join(th[i], NULL), 0);
	srv_stop(srv);
	for (int i = 0; i < CONC_THREADS; i++)
		if (failed[i])
			fail_msg("client %d: %d of %d handshakes failed", i, failed[i],
				 CONC_ROUNDS);
}

int main(void)
{
	alarm(120);

	load_pem(&s_ca, "ca.pem");
	load_pem(&s_srv_cert, "srv.pem");
	load_pem(&s_srv_key, "srv.key");
	load_pem(&s_cli_cert, "cli.pem");
	load_pem(&s_cli_key, "cli.key");

	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_static_session_before_tls_setup),
		cmocka_unit_test(test_pool_exhaustion_is_no_memory),
		cmocka_unit_test(test_concurrent_sessions_share_the_pool),
	};
	return cmocka_run_group_tests_name("net_tls_zh", tests, NULL, NULL);
}
