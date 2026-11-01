/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OVE_TEST_MQTT_LIFECYCLE_H
#define OVE_TEST_MQTT_LIFECYCLE_H

#include "ove/net_mqtt.h"
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

struct mqtt_broker_report {
	unsigned int accepted;
	unsigned int published;
	unsigned int disconnected;
	unsigned int closed;
};

static int mqtt_read_exact(mbedtls_ssl_context *ssl, uint8_t *buf, size_t size)
{
	while (size) {
		int count = mbedtls_ssl_read(ssl, buf, size);
		if (count <= 0)
			return -1;
		buf += count;
		size -= (size_t)count;
	}
	return 0;
}

static int mqtt_read_packet(mbedtls_ssl_context *ssl, uint8_t *packet, size_t capacity)
{
	if (mqtt_read_exact(ssl, packet, 1) < 0)
		return -1;
	size_t offset = 1, remaining = 0, multiplier = 1;
	uint8_t byte;
	do {
		if (offset > 4 || mqtt_read_exact(ssl, &byte, 1) < 0)
			return -1;
		remaining += (byte & 0x7f) * multiplier;
		multiplier *= 128;
		++offset;
	} while (byte & 0x80);
	if (offset + remaining > capacity)
		return -1;
	return mqtt_read_exact(ssl, packet + offset, remaining);
}

static void mqtt_broker_run(int listener, int report_fd, unsigned int connections)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	s_rng_state ^= 0x4d51545400000000u;
#endif
	if (psa_crypto_init() != PSA_SUCCESS)
		_exit(2);
	mbedtls_ssl_config conf;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_ssl_config_init(&conf);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&cert);
	mbedtls_pk_init(&key);
	if (mbedtls_ctr_drbg_seed(&drbg, drbg_entropy, NULL, NULL, 0) != 0 ||
	    mbedtls_x509_crt_parse(&cert, s_srv_cert.data, s_srv_cert.len) != 0 ||
	    mbedtls_pk_parse_key(&key, s_srv_key.data, s_srv_key.len, NULL, 0,
				 mbedtls_ctr_drbg_random, &drbg) != 0 ||
	    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		_exit(2);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
	if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0)
		_exit(2);
	struct mqtt_broker_report report = {0};
	for (unsigned int i = 0; i < connections; ++i) {
		struct pollfd ready = {.fd = listener, .events = POLLIN};
		if (poll(&ready, 1, 5000) <= 0)
			_exit(3);
		int fd = accept(listener, NULL, NULL);
		if (fd < 0)
			_exit(3);
		mbedtls_ssl_context ssl;
		mbedtls_ssl_init(&ssl);
		if (mbedtls_ssl_setup(&ssl, &conf) != 0)
			_exit(3);
		mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
		int rc;
		do {
			rc = mbedtls_ssl_handshake(&ssl);
		} while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
		uint8_t packet[512];
		if (rc != 0 || mqtt_read_packet(&ssl, packet, sizeof(packet)) < 0 ||
		    packet[0] != 0x10)
			_exit(3);
		uint8_t connack[] = {0x20, 2, 0, i == 0 ? 5 : 0};
		if (mbedtls_ssl_write(&ssl, connack, sizeof(connack)) != sizeof(connack))
			_exit(3);
		if (i == 0) {
			/* Refusal must close the client's first transport before retry. */
			if (mbedtls_ssl_read(&ssl, packet, sizeof(packet)) > 0)
				_exit(3);
		} else {
			++report.accepted;
			for (unsigned int pub = 0; pub < 2; ++pub) {
				if (mqtt_read_packet(&ssl, packet, sizeof(packet)) < 0 ||
				    (packet[0] & 0xf0) != 0x30)
					_exit(3);
				++report.published;
			}
			if (mqtt_read_packet(&ssl, packet, sizeof(packet)) == 0) {
				if (packet[0] != 0xe0)
					_exit(3);
				++report.disconnected;
			} else {
				++report.closed;
			}
		}
		mbedtls_ssl_free(&ssl);
		close(fd);
	}
	mbedtls_pk_free(&key);
	mbedtls_x509_crt_free(&cert);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_ssl_config_free(&conf);
	close(listener);
	if (write(report_fd, &report, sizeof(report)) != sizeof(report))
		_exit(3);
	close(report_fd);
	_exit(0);
}

static pid_t mqtt_broker_start(uint16_t *port, int *report_fd, unsigned int connections)
{
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		return -1;
	struct sockaddr_in addr = {.sin_family = AF_INET,
				   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	socklen_t length = sizeof(addr);
	int pipefd[2];
	if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    getsockname(listener, (struct sockaddr *)&addr, &length) != 0 ||
	    listen(listener, 4) != 0 || pipe(pipefd) != 0) {
		close(listener);
		return -1;
	}
	pid_t child = fork();
	if (child == 0) {
		close(pipefd[0]);
		mqtt_broker_run(listener, pipefd[1], connections);
	}
	close(listener);
	close(pipefd[1]);
	if (child < 0) {
		close(pipefd[0]);
		return -1;
	}
	*port = ntohs(addr.sin_port);
	*report_fd = pipefd[0];
	return child;
}

static int mqtt_broker_join(pid_t child)
{
	int status = 0;
	uint64_t now = 0;
	(void)ove_time_get_us(&now);
	uint64_t deadline = now + 6000000;
	for (;;) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0)
			return 0;
		(void)ove_time_get_us(&now);
		if (now >= deadline) {
			(void)kill(child, SIGKILL);
			(void)waitpid(child, &status, 0);
			return 0;
		}
		usleep(1000);
	}
}

static int mqtt_fd_count(void)
{
	DIR *directory = opendir("/proc/self/fd");
	if (!directory)
		return -1;
	int count = 0;
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL)
		if (entry->d_name[0] != '.')
			++count;
	(void)closedir(directory);
	return count;
}

#ifdef CONFIG_OVE_ZERO_HEAP
static size_t mqtt_pool_capacity(void)
{
	static void *blocks[4096];
	size_t count = 0, capacity = 0;
	for (size_t size = 4096; size >= 16; size /= 2) {
		void *block;
		while (count < sizeof(blocks) / sizeof(blocks[0]) &&
		       (block = mbedtls_calloc(1, size)) != NULL) {
			blocks[count++] = block;
			capacity += size;
		}
	}
	while (count)
		mbedtls_free(blocks[--count]);
	return capacity;
}
#endif

static void test_mqtt_real_tls_reconnect_and_cleanup(void **state)
{
	(void)state;
	static ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t client = NULL;
	assert_int_equal(ove_mqtt_client_init(&client, &storage), OVE_OK);
	unsigned int extra = 0;
#ifndef CONFIG_OVE_ZERO_HEAP
	extra = 1;
#endif
	int fd_before = mqtt_fd_count();
	uint16_t port = 0;
	int report_fd = -1;
	pid_t server = mqtt_broker_start(&port, &report_fd, 9 + extra);
	assert_true(server > 0);
	ove_mqtt_config_t config = {
		.host = "localhost",
		.port = port,
		.client_id = "lifecycle",
		.use_tls = 1,
		.tls_ca_cert = s_ca.data,
		.tls_ca_cert_len = s_ca.len,
	};
	int refused = ove_mqtt_connect(client, &config);
	int valid = refused == OVE_ERR_NOT_SUPPORTED;
#ifdef CONFIG_OVE_ZERO_HEAP
	size_t baseline = 0;
	int recovered = 1;
#endif
	for (unsigned int i = 0; i < 8 && valid; ++i) {
		int connected = ove_mqtt_connect(client, &config);
		int first = connected == OVE_OK
				    ? ove_mqtt_publish(client, "t", "a", 1, OVE_MQTT_QOS0)
				    : connected;
		int busy = first == OVE_OK ? ove_mqtt_connect(client, &config) : first;
		int second = busy == OVE_ERR_BUSY
				     ? ove_mqtt_publish(client, "t", "b", 1, OVE_MQTT_QOS0)
				     : busy;
		valid = connected == OVE_OK && first == OVE_OK && busy == OVE_ERR_BUSY &&
			second == OVE_OK;
		if (i == 7)
			ove_mqtt_client_deinit(client);
		else
			ove_mqtt_disconnect(client);
#ifdef CONFIG_OVE_ZERO_HEAP
		size_t capacity = mqtt_pool_capacity();
		if (i == 0)
			baseline = capacity;
		else
			recovered &= capacity == baseline;
#endif
	}
	ove_mqtt_client_deinit(client);
#ifndef CONFIG_OVE_ZERO_HEAP
	if (valid) {
		ove_mqtt_client_t owned = NULL;
		int created = ove_mqtt_client_create(&owned);
		int connected = created == OVE_OK ? ove_mqtt_connect(owned, &config) : created;
		int first = connected == OVE_OK
				    ? ove_mqtt_publish(owned, "t", "a", 1, OVE_MQTT_QOS0)
				    : connected;
		int second = first == OVE_OK ? ove_mqtt_publish(owned, "t", "b", 1, OVE_MQTT_QOS0)
					     : first;
		valid = created == OVE_OK && connected == OVE_OK && first == OVE_OK &&
			second == OVE_OK;
		ove_mqtt_client_destroy(owned);
	}
#endif
	int reaped = mqtt_broker_join(server);
	struct mqtt_broker_report report = {0};
	ssize_t size = read(report_fd, &report, sizeof(report));
	close(report_fd);
	int fd_after = mqtt_fd_count();
	assert_true(valid && reaped);
	assert_int_equal(size, sizeof(report));
	assert_int_equal(report.accepted, 8 + extra);
	assert_int_equal(report.published, 16 + 2 * extra);
	assert_int_equal(report.disconnected, 7);
	assert_int_equal(report.closed, 1 + extra);
	assert_true(fd_before >= 0);
	assert_int_equal(fd_after, fd_before);
#ifdef CONFIG_OVE_ZERO_HEAP
	assert_true(recovered && baseline > 0);
#endif
}

#endif
