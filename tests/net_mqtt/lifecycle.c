/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "framework/ove_test.h"
#include <stdlib.h>
#include <string.h>

void *ove_test_mqtt_malloc(size_t size);
void ove_test_mqtt_free(void *ptr);

enum failure_stage {
	FAIL_NONE,
	FAIL_DNS,
	FAIL_OPEN,
	FAIL_CONNECT,
	FAIL_TLS_INIT,
	FAIL_HANDSHAKE,
	FAIL_SEND,
	FAIL_RECV,
	FAIL_SHORT,
	FAIL_REFUSED,
	FAIL_ZERO_SEND,
	FAIL_PARTIAL_SEND
};

struct allocation {
	unsigned char *raw;
	void *data;
	size_t size;
};

struct owned_resource {
	void *storage;
	uintptr_t id;
	int active;
};

static struct {
	struct allocation blocks[64];
	struct owned_resource sockets[64];
	struct owned_resource sessions[64];
	unsigned int malloc_calls;
	unsigned int fail_malloc;
	unsigned int live_blocks;
	unsigned int io_calls;
	unsigned int send_calls;
	unsigned int recv_calls;
	unsigned int live_sockets;
	unsigned int live_sessions;
	unsigned int bad_cleanup;
	unsigned int corrupt;
	unsigned int tls_reinit;
	unsigned int socket_reinit;
	unsigned int tls_close_calls;
	unsigned int tls_deinit_calls;
	unsigned int socket_close_calls;
	unsigned int callback_count;
	enum failure_stage failure;
	uint8_t sent[CONFIG_OVE_NET_MQTT_TX_BUF + 64];
	size_t sent_size;
	uint8_t incoming[64];
	size_t incoming_size;
} probe;

static char credential[65537];

void *ove_test_mqtt_malloc(size_t size)
{
	++probe.malloc_calls;
#ifdef CONFIG_OVE_ZERO_HEAP
	(void)size;
	return NULL;
#else
	if (probe.fail_malloc == probe.malloc_calls)
		return NULL;
	for (size_t i = 0; i < 64; ++i) {
		if (probe.blocks[i].raw)
			continue;
		unsigned char *raw = malloc(size + 32);
		if (!raw)
			return NULL;
		memset(raw, 0xa5, 16);
		memset(raw + 16 + size, 0xa5, 16);
		probe.blocks[i] = (struct allocation){raw, raw + 16, size};
		++probe.live_blocks;
		return raw + 16;
	}
	return NULL;
#endif
}

void ove_test_mqtt_free(void *ptr)
{
	if (!ptr)
		return;
	for (size_t i = 0; i < 64; ++i) {
		struct allocation *b = &probe.blocks[i];
		if (b->data != ptr || !b->raw)
			continue;
		for (size_t j = 0; j < 16; ++j)
			if (b->raw[j] != 0xa5 || b->raw[16 + b->size + j] != 0xa5)
				++probe.corrupt;
		free(b->raw);
		*b = (struct allocation){0};
		--probe.live_blocks;
		return;
	}
	++probe.bad_cleanup;
}

static uintptr_t resource_open(struct owned_resource *table, void *storage)
{
	uintptr_t id = 1;
	for (size_t i = 0; i < 64; ++i) {
		if (table[i].id >= id)
			id = table[i].id + 1;
	}
	for (size_t i = 0; i < 64; ++i) {
		if (!table[i].active) {
			table[i] = (struct owned_resource){storage, id, 1};
			return id;
		}
	}
	return 0;
}

static int resource_close(struct owned_resource *table, void *storage, uintptr_t id)
{
	for (size_t i = 0; i < 64; ++i) {
		if (table[i].active && table[i].storage == storage && table[i].id == id) {
			table[i].active = 0;
			return 1;
		}
	}
	++probe.bad_cleanup;
	return 0;
}

int ove_dns_resolve(const char *host, ove_sockaddr_t *addr, uint64_t timeout)
{
	(void)host;
	(void)timeout;
	++probe.io_calls;
	memset(addr, 0, sizeof(*addr));
	return probe.failure == FAIL_DNS ? OVE_ERR_NET_RESET : OVE_OK;
}

int ove_socket_open(ove_socket_t *sock, ove_socket_storage_t *storage, ove_af_t af,
		    ove_sock_type_t type)
{
	(void)af;
	(void)type;
	++probe.io_calls;
	if (probe.failure == FAIL_OPEN)
		return OVE_ERR_NET_RESET;
	for (size_t i = 0; i < 64; ++i)
		if (probe.sockets[i].active && probe.sockets[i].storage == storage)
			++probe.socket_reinit;
	storage->fd = (int)resource_open(probe.sockets, storage);
	*sock = storage;
	++probe.live_sockets;
	return OVE_OK;
}

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr, uint64_t timeout)
{
	(void)sock;
	(void)addr;
	(void)timeout;
	++probe.io_calls;
	return probe.failure == FAIL_CONNECT ? OVE_ERR_NET_RESET : OVE_OK;
}

void ove_socket_close(ove_socket_t sock)
{
	++probe.socket_close_calls;
	if (!sock) {
		++probe.bad_cleanup;
		return;
	}
	if (resource_close(probe.sockets, sock, (uintptr_t)sock->fd))
		--probe.live_sockets;
	sock->fd = -1;
}

static int scripted_send(const void *data, size_t size, size_t *sent)
{
	++probe.io_calls;
	++probe.send_calls;
	*sent = 0;
	if (probe.failure == FAIL_SEND ||
	    (probe.failure == FAIL_PARTIAL_SEND && probe.send_calls > 1))
		return OVE_ERR_NET_RESET;
	if (probe.failure == FAIL_ZERO_SEND)
		return OVE_OK;
	if (probe.failure == FAIL_PARTIAL_SEND && size > 2)
		size = 2;
	if (probe.sent_size + size <= sizeof(probe.sent))
		memcpy(probe.sent + probe.sent_size, data, size);
	else
		++probe.corrupt;
	probe.sent_size += size;
	*sent = size;
	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t size, size_t *sent)
{
	if (!sock || sock->fd < 0)
		++probe.bad_cleanup;
	return scripted_send(data, size, sent);
}

static int scripted_recv(void *data, size_t size, size_t *received)
{
	++probe.io_calls;
	++probe.recv_calls;
	*received = 0;
	if (probe.failure == FAIL_RECV)
		return OVE_ERR_NET_RESET;
	if (probe.recv_calls > 1) {
		if (!probe.incoming_size)
			return OVE_ERR_TIMEOUT;
		*received = probe.incoming_size < size ? probe.incoming_size : size;
		memcpy(data, probe.incoming, *received);
		probe.incoming_size = 0;
		return OVE_OK;
	}
	uint8_t connack[4] = {0x20, 0x02, 0, probe.failure == FAIL_REFUSED ? 5 : 0};
	size_t count = probe.failure == FAIL_SHORT ? 3 : 4;
	*received = count < size ? count : size;
	memcpy(data, connack, *received);
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *data, size_t size, size_t *received, uint64_t timeout)
{
	(void)timeout;
	if (!sock || sock->fd < 0)
		++probe.bad_cleanup;
	return scripted_recv(data, size, received);
}

#ifdef CONFIG_OVE_NET_TLS
int ove_tls_init(ove_tls_t *tls, ove_tls_storage_t *storage)
{
	++probe.io_calls;
	if (probe.failure == FAIL_TLS_INIT)
		return OVE_ERR_NO_MEMORY;
	for (size_t i = 0; i < 64; ++i)
		if (probe.sessions[i].active && probe.sessions[i].storage == storage)
			++probe.tls_reinit;
	memset(storage, 0, sizeof(*storage));
	storage->ssl = (void *)resource_open(probe.sessions, storage);
	*tls = storage;
	++probe.live_sessions;
	return OVE_OK;
}

int ove_tls_handshake(ove_tls_t tls, ove_socket_t sock, const ove_tls_config_t *cfg)
{
	(void)cfg;
	++probe.io_calls;
	tls->sock = sock;
	return probe.failure == FAIL_HANDSHAKE ? OVE_ERR_NET_RESET : OVE_OK;
}

void ove_tls_close(ove_tls_t tls)
{
	if (!tls || !tls->ssl)
		++probe.bad_cleanup;
	++probe.tls_close_calls;
}

void ove_tls_deinit(ove_tls_t tls)
{
	++probe.tls_deinit_calls;
	if (!tls) {
		++probe.bad_cleanup;
		return;
	}
	if (resource_close(probe.sessions, tls, (uintptr_t)tls->ssl))
		--probe.live_sessions;
	tls->ssl = NULL;
}

int ove_tls_send(ove_tls_t tls, const void *data, size_t size, size_t *sent)
{
	if (!tls || !tls->ssl)
		++probe.bad_cleanup;
	return scripted_send(data, size, sent);
}

int ove_tls_recv(ove_tls_t tls, void *data, size_t size, size_t *received)
{
	if (!tls || !tls->ssl)
		++probe.bad_cleanup;
	return scripted_recv(data, size, received);
}
#endif

static int setup(void **state)
{
	(void)state;
	memset(&probe, 0, sizeof(probe));
	memset(credential, 'x', sizeof(credential) - 1);
	credential[sizeof(credential) - 1] = 0;
	return 0;
}

static int teardown(void **state)
{
	(void)state;
	/* The dependency doubles own no asynchronous actors. Preserve leak/canary
	 * observations in assertions, then release any deliberately broken owner. */
	for (size_t i = 0; i < 64; ++i)
		if (probe.blocks[i].raw)
			free(probe.blocks[i].raw);
	return 0;
}

static ove_mqtt_config_t config(void)
{
	ove_mqtt_config_t cfg = {.host = "broker", .port = 1883, .client_id = "client"};
#ifdef CONFIG_OVE_NET_TLS
	cfg.use_tls = 1;
#endif
	return cfg;
}

static void assert_clean(void)
{
	assert_int_equal(probe.live_sockets, 0);
	assert_int_equal(probe.live_sessions, 0);
	assert_int_equal(probe.live_blocks, 0);
	assert_int_equal(probe.corrupt, 0);
	assert_int_equal(probe.bad_cleanup, 0);
}

static void test_long_credentials_preflight(void **state)
{
	(void)state;
	for (int field = 0; field < 3; ++field) {
		ove_mqtt_client_storage_t storage;
		ove_mqtt_client_t c = NULL;
		OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
		ove_mqtt_config_t cfg = config();
		if (field == 0)
			cfg.client_id = credential;
		if (field == 1)
			cfg.username = credential;
		if (field == 2)
			cfg.password = credential;
		int rc = ove_mqtt_connect(c, &cfg);
		unsigned int io = probe.io_calls;
		unsigned int allocations = probe.malloc_calls;
		ove_mqtt_client_deinit(c);
		assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
		assert_int_equal(io, 0);
		assert_int_equal(allocations, 0);
		assert_clean();
	}
}

static void test_connect_reuses_buffers(void **state)
{
	(void)state;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	for (unsigned int i = 0; i < 5; ++i) {
		probe.recv_calls = 0;
		OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
		ove_mqtt_disconnect(c);
	}
	unsigned int allocations = probe.malloc_calls;
	ove_mqtt_client_deinit(c);
#ifdef CONFIG_OVE_ZERO_HEAP
	assert_int_equal(allocations, 0);
#else
	assert_int_equal(allocations, 2);
#endif
	assert_clean();
}

static void test_live_deinit_closes_transport(void **state)
{
	(void)state;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	ove_mqtt_client_deinit(c);
	ove_mqtt_client_deinit(c);
	ove_mqtt_disconnect(c);
	assert_clean();
}

static void original_callback(const char *topic, size_t tlen, const void *payload, size_t plen,
			      void *data)
{
	if (tlen == 1 && topic[0] == 't' && plen == 1 && *(const char *)payload == 'v')
		++*(int *)data;
}

static void replacement_callback(const char *topic, size_t tlen, const void *payload, size_t plen,
				 void *data)
{
	(void)topic;
	(void)tlen;
	(void)payload;
	(void)plen;
	++*(int *)data;
}

static void test_connected_rejects_second_connect(void **state)
{
	(void)state;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	int original_calls = 0, replacement_calls = 0;
	ove_mqtt_config_t cfg = config();
	cfg.on_message = original_callback;
	cfg.user_data = &original_calls;
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	c->keep_alive_s = 99;
	c->pkt_id = 37;
	c->sub_count = 1;
	strcpy(c->sub_filters[0], "t");
	unsigned char snapshot[sizeof(storage)];
	memcpy(snapshot, &storage, sizeof(storage));
	unsigned int before = probe.io_calls;
	unsigned int allocated_before = probe.malloc_calls;
	unsigned int blocks_before = probe.live_blocks;
	cfg.client_id = "replacement";
	cfg.username = "new-user";
	cfg.password = "new-password";
	cfg.keep_alive_s = 11;
	cfg.on_message = replacement_callback;
	cfg.user_data = &replacement_calls;
	probe.recv_calls = 0;
	int rc = ove_mqtt_connect(c, &cfg);
	unsigned int after = probe.io_calls;
	int untouched = memcmp(snapshot, &storage, sizeof(storage)) == 0;
	int same_allocations = allocated_before == probe.malloc_calls &&
			       blocks_before == probe.live_blocks;
	int publish = ove_mqtt_publish(c, "t", "v", 1, OVE_MQTT_QOS0);
	const uint8_t packet[] = {0x30, 4, 0, 1, 't', 'v'};
	memcpy(probe.incoming, packet, sizeof(packet));
	probe.incoming_size = sizeof(packet);
	/* A rejected attempt must not restart the existing receive stream. */
	probe.recv_calls = 1;
	int loop = ove_mqtt_loop(c, OVE_MS(100));
	ove_mqtt_disconnect(c);
	ove_mqtt_client_deinit(c);
	assert_int_equal(rc, OVE_ERR_BUSY);
	assert_int_equal(after, before);
	assert_true(untouched && same_allocations);
	assert_int_equal(publish, OVE_OK);
	assert_int_equal(loop, OVE_OK);
	assert_int_equal(original_calls, 1);
	assert_int_equal(replacement_calls, 0);
	assert_int_equal(probe.tls_reinit, 0);
	assert_int_equal(probe.socket_reinit, 0);
	assert_clean();
}

static void test_disconnect_send_failure_still_releases(void **state)
{
	(void)state;
	for (int zero = 0; zero < 2; ++zero) {
		(void)setup(NULL);
		ove_mqtt_client_storage_t storage;
		ove_mqtt_client_t c = NULL;
		OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
		ove_mqtt_config_t cfg = config();
		OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
		uint8_t *rx = c->rx_buf, *tx = c->tx_buf;
		probe.failure = zero ? FAIL_ZERO_SEND : FAIL_SEND;
		ove_mqtt_disconnect(c);
		unsigned int live = probe.live_sockets + probe.live_sessions;
		int retained = rx == c->rx_buf && tx == c->tx_buf;
		unsigned int sends = probe.send_calls;
		unsigned int releases = probe.socket_close_calls + probe.tls_deinit_calls;
		ove_mqtt_disconnect(c);
		int once = sends == probe.send_calls &&
			   releases == probe.socket_close_calls + probe.tls_deinit_calls;
		probe.failure = FAIL_NONE;
		probe.recv_calls = 0;
		int retry = ove_mqtt_connect(c, &cfg);
		ove_mqtt_client_deinit(c);
		assert_int_equal(live, 0);
		assert_true(retained && once);
		assert_int_equal(retry, OVE_OK);
		assert_clean();
	}
}

static void test_frame_capacity(void **state)
{
	(void)state;
	if (CONFIG_OVE_NET_MQTT_TX_BUF != 512 && CONFIG_OVE_NET_MQTT_TX_BUF != 130)
		skip();
	size_t length = CONFIG_OVE_NET_MQTT_TX_BUF == 512 ? 497 : 115;
	credential[length] = 0;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	cfg.client_id = credential;
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	size_t sent = probe.sent_size;
	ove_mqtt_disconnect(c);
	credential[length] = 'x';
	credential[length + 1] = 0;
	unsigned int before = probe.io_calls;
	int rc = ove_mqtt_connect(c, &cfg);
	unsigned int after = probe.io_calls;
	ove_mqtt_client_deinit(c);
	assert_int_equal(sent, CONFIG_OVE_NET_MQTT_TX_BUF == 512 ? 512 : 129);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_int_equal(after, before);
	assert_clean();
}

static void test_optional_field_capacity(void **state)
{
	(void)state;
	if (CONFIG_OVE_NET_MQTT_TX_BUF != 512)
		skip();
	credential[170] = 0;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	cfg.client_id = cfg.username = cfg.password = credential;
	int rc = ove_mqtt_connect(c, &cfg);
	unsigned int effects = probe.io_calls + probe.malloc_calls;
	ove_mqtt_client_deinit(c);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_int_equal(effects, 0);
	assert_clean();
}

static void test_empty_optional_fields(void **state)
{
	(void)state;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	cfg.username = cfg.password = "";
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	unsigned int flags = probe.sent[9];
	size_t size = probe.sent_size;
	ove_mqtt_disconnect(c);
	ove_mqtt_client_deinit(c);
	assert_int_equal(flags, 0xc2);
	assert_int_equal(size, 24);
	assert_clean();
}

static void test_empty_optional_prefix_counts(void **state)
{
	(void)state;
	if (CONFIG_OVE_NET_MQTT_TX_BUF != 512)
		skip();
	credential[497] = 0;
	for (int field = 0; field < 2; ++field) {
		ove_mqtt_client_storage_t storage;
		ove_mqtt_client_t c = NULL;
		OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
		ove_mqtt_config_t cfg = config();
		cfg.client_id = credential;
		if (field == 0)
			cfg.username = "";
		else
			cfg.password = "";
		int rc = ove_mqtt_connect(c, &cfg);
		unsigned int effects = probe.io_calls + probe.malloc_calls;
		ove_mqtt_client_deinit(c);
		assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
		assert_int_equal(effects, 0);
		assert_clean();
	}
}

static void test_wire_length_boundary(void **state)
{
	(void)state;
	if (CONFIG_OVE_NET_MQTT_TX_BUF < 196625)
		skip();
	for (int field = 0; field < 4; ++field) {
		(void)setup(NULL);
		credential[65535] = 0;
		ove_mqtt_client_storage_t storage;
		ove_mqtt_client_t c = NULL;
		OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
		ove_mqtt_config_t cfg = config();
		if (field == 0)
			cfg.client_id = credential;
		if (field == 1)
			cfg.username = credential;
		if (field == 2) {
			cfg.username = "";
			cfg.password = credential;
		}
		if (field == 3)
			cfg.client_id = cfg.username = cfg.password = credential;
		int rc = ove_mqtt_connect(c, &cfg);
		size_t prefix = field == 1 ? 22 : field == 2 ? 24 : 14;
		int exact_prefix = probe.sent[prefix] == 0xff && probe.sent[prefix + 1] == 0xff;
		size_t total = probe.sent_size;
		ove_mqtt_disconnect(c);
		ove_mqtt_client_deinit(c);
		assert_int_equal(rc, OVE_OK);
		assert_true(exact_prefix);
		if (field == 3)
			assert_int_equal(total, 196625);
		assert_clean();
	}
}

static void test_small_rx_preflight(void **state)
{
	(void)state;
	if (CONFIG_OVE_NET_MQTT_RX_BUF >= 4)
		skip();
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	int rc = ove_mqtt_connect(c, &cfg);
	unsigned int effects = probe.io_calls + probe.malloc_calls;
	ove_mqtt_client_deinit(c);
	assert_int_equal(rc, OVE_ERR_INVALID_PARAM);
	assert_int_equal(effects, 0);
	assert_clean();
}

static void test_unavailable_tls_preflight(void **state)
{
	(void)state;
#ifdef CONFIG_OVE_NET_TLS
	skip();
#else
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	cfg.use_tls = 1;
	int rc = ove_mqtt_connect(c, &cfg);
	unsigned int effects = probe.io_calls + probe.malloc_calls;
	ove_mqtt_client_deinit(c);
	assert_int_equal(rc, OVE_ERR_NOT_SUPPORTED);
	assert_int_equal(effects, 0);
	assert_clean();
#endif
}

static void test_partial_buffer_allocation(void **state)
{
	(void)state;
#ifdef CONFIG_OVE_ZERO_HEAP
	skip();
#else
	for (unsigned int fail = 1; fail <= 2; ++fail) {
		(void)setup(NULL);
		ove_mqtt_client_storage_t storage;
		ove_mqtt_client_t c = NULL;
		OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
		ove_mqtt_config_t cfg = config();
		probe.fail_malloc = fail;
		int rc = ove_mqtt_connect(c, &cfg);
		unsigned int live = probe.live_blocks;
		int unpublished = c->rx_buf == NULL && c->tx_buf == NULL;
		unsigned int io = probe.io_calls;
		probe.fail_malloc = 0;
		int retry = ove_mqtt_connect(c, &cfg);
		ove_mqtt_disconnect(c);
		ove_mqtt_client_deinit(c);
		assert_int_equal(rc, OVE_ERR_NO_MEMORY);
		assert_int_equal(live, 0);
		assert_true(unpublished);
		assert_int_equal(io, 0);
		assert_int_equal(retry, OVE_OK);
		assert_clean();
	}
#endif
}

static void test_failure_stage_recovery(void **state)
{
	(void)state;
	for (enum failure_stage stage = FAIL_DNS; stage <= FAIL_PARTIAL_SEND; ++stage) {
#ifndef CONFIG_OVE_NET_TLS
		if (stage == FAIL_TLS_INIT || stage == FAIL_HANDSHAKE)
			continue;
#endif
		(void)setup(NULL);
		ove_mqtt_client_storage_t storage;
		ove_mqtt_client_t c = NULL;
		OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
		ove_mqtt_config_t cfg = config();
		probe.failure = stage;
		int rc = ove_mqtt_connect(c, &cfg);
		unsigned int sockets = probe.live_sockets, sessions = probe.live_sessions;
		int handles_cleared = c->sock == NULL && c->tls == NULL;
		uint8_t *rx = c->rx_buf, *tx = c->tx_buf;
		probe.failure = FAIL_NONE;
		probe.recv_calls = 0;
		int retry = ove_mqtt_connect(c, &cfg);
		int reused = rx == c->rx_buf && tx == c->tx_buf;
		ove_mqtt_disconnect(c);
		ove_mqtt_client_deinit(c);
		int expected = stage == FAIL_TLS_INIT	 ? OVE_ERR_NO_MEMORY
			       : stage == FAIL_ZERO_SEND ? OVE_ERR_NET_CLOSED
			       : stage == FAIL_SHORT || stage == FAIL_REFUSED
				       ? OVE_ERR_NOT_SUPPORTED
				       : OVE_ERR_NET_RESET;
		assert_int_equal(rc, expected);
		assert_int_equal(sockets + sessions, 0);
		assert_true(handles_cleared && reused);
		assert_int_equal(retry, OVE_OK);
		assert_clean();
	}
}

static void test_fresh_session_clears_filters(void **state)
{
	(void)state;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	c->sub_count = 1;
	strcpy(c->sub_filters[0], "old");
	ove_mqtt_disconnect(c);
	probe.recv_calls = 0;
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	unsigned int filters = c->sub_count;
	ove_mqtt_disconnect(c);
	ove_mqtt_client_deinit(c);
	assert_int_equal(filters, 0);
	assert_clean();
}

static void disconnected_callback(const char *topic, size_t tlen, const void *payload, size_t plen,
				  void *data)
{
	ove_mqtt_client_t c = data;
	int before = tlen == 1 && topic[0] == 't' && plen == 1 && *(const char *)payload == 'v';
	ove_mqtt_disconnect(c);
	int after = topic[0] == 't' && *(const char *)payload == 'v';
	probe.callback_count += before && after ? 1 : 100;
}

static void test_callback_disconnect_keeps_packet(void **state)
{
	(void)state;
	ove_mqtt_client_storage_t storage;
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_init(&c, &storage));
	ove_mqtt_config_t cfg = config();
	cfg.on_message = disconnected_callback;
	cfg.user_data = c;
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	const uint8_t packets[] = {0x30, 4, 0, 1, 't', 'v', 0x90, 3, 0, 1, 0};
	memcpy(probe.incoming, packets, sizeof(packets));
	probe.incoming_size = sizeof(packets);
	int rc = ove_mqtt_subscribe(c, "t", OVE_MQTT_QOS0);
	ove_mqtt_client_deinit(c);
	assert_int_equal(rc, OVE_ERR_NET_CLOSED);
	assert_int_equal(probe.callback_count, 1);
	assert_clean();
}

static void test_destroy_owns_live_transport(void **state)
{
	(void)state;
#ifdef CONFIG_OVE_ZERO_HEAP
	skip();
#else
	ove_mqtt_client_t c = NULL;
	OVE_TEST_ASSERT_OK(ove_mqtt_client_create(&c));
	ove_mqtt_config_t cfg = config();
	OVE_TEST_ASSERT_OK(ove_mqtt_connect(c, &cfg));
	ove_mqtt_client_destroy(c);
	ove_mqtt_client_destroy(NULL);
	assert_clean();
#endif
}

int main(int argc, char **argv)
{
	if (argc > 1)
		cmocka_set_test_filter(argv[1]);
	else if (CONFIG_OVE_NET_MQTT_RX_BUF < 4)
		cmocka_set_test_filter("test_small_rx_preflight");
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_long_credentials_preflight, setup, teardown),
		cmocka_unit_test_setup_teardown(test_connect_reuses_buffers, setup, teardown),
		cmocka_unit_test_setup_teardown(test_live_deinit_closes_transport, setup, teardown),
		cmocka_unit_test_setup_teardown(test_connected_rejects_second_connect, setup,
						teardown),
		cmocka_unit_test_setup_teardown(test_frame_capacity, setup, teardown),
		cmocka_unit_test_setup_teardown(test_optional_field_capacity, setup, teardown),
		cmocka_unit_test_setup_teardown(test_empty_optional_fields, setup, teardown),
		cmocka_unit_test_setup_teardown(test_empty_optional_prefix_counts, setup, teardown),
		cmocka_unit_test_setup_teardown(test_wire_length_boundary, setup, teardown),
		cmocka_unit_test_setup_teardown(test_small_rx_preflight, setup, teardown),
		cmocka_unit_test_setup_teardown(test_unavailable_tls_preflight, setup, teardown),
		cmocka_unit_test_setup_teardown(test_partial_buffer_allocation, setup, teardown),
		cmocka_unit_test_setup_teardown(test_failure_stage_recovery, setup, teardown),
		cmocka_unit_test_setup_teardown(test_fresh_session_clears_filters, setup, teardown),
		cmocka_unit_test_setup_teardown(test_callback_disconnect_keeps_packet, setup,
						teardown),
		cmocka_unit_test_setup_teardown(test_destroy_owns_live_transport, setup, teardown),
		cmocka_unit_test_setup_teardown(test_disconnect_send_failure_still_releases, setup,
						teardown),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
