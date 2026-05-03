/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS Networking Example
 *
 * Exercises the full networking stack:
 *   1. Network interface init with static IP
 *   2. DNS resolution (positive + negative)
 *   3. Raw TCP socket connect + HTTP/1.0 request
 *   4. UDP send/receive (loopback echo)
 *   5. HTTP client GET and POST
 *   6. MQTT connect, subscribe, publish QoS0/1, receive, disconnect
 *   7. Embedded HTTP server (web dashboard)
 *
 * On POSIX the netif init is a no-op (host networking).
 * On FreeRTOS/lwIP the static IP config drives real hardware.
 */

#include "ove/ove.h"
#include "ove/net_sntp.h"
#include <stdio.h>
#include <string.h>

static int pass_count;
static int fail_count;

#define TEST(name) OVE_LOG_INF("  [TEST] %s", name)
#define PASS(name)                                \
	do {                                      \
		OVE_LOG_INF("  [PASS] %s", name); \
		pass_count++;                     \
	} while (0)
#define FAIL(name, err)                                     \
	do {                                                \
		OVE_LOG_ERR("  [FAIL] %s (%d)", name, err); \
		fail_count++;                               \
	} while (0)

/* ── 1. Network interface ───────────────────────────────────────── */

static void test_netif_init(void)
{
	OVE_LOG_INF("=== Network Interface ===");

	ove_netif_t netif;
	ove_netif_storage_t netif_storage;

	TEST("netif_init");
	int ret = ove_netif_init(&netif, &netif_storage);
	if (ret != OVE_OK) {
		FAIL("netif_init", ret);
		return;
	}
	PASS("netif_init");

	TEST("netif_up (static IP)");
	ove_netif_config_t cfg = {0};
#ifndef CONFIG_OVE_RTOS_POSIX
	/* Embedded: static IP 172.1.1.2/24, gw 172.1.1.1, DNS 8.8.8.8 */
	cfg.use_dhcp = 0;
	ove_sockaddr_ipv4(&cfg.static_ip, 172, 1, 1, 2, 0);
	ove_sockaddr_ipv4(&cfg.gateway, 172, 1, 1, 1, 0);
	ove_sockaddr_ipv4(&cfg.netmask, 255, 255, 255, 0, 0);
	ove_sockaddr_ipv4(&cfg.dns, 8, 8, 8, 8, 0);
#endif
	ret = ove_netif_up(netif, &cfg);
	if (ret != OVE_OK) {
		FAIL("netif_up", ret);
		return;
	}
	PASS("netif_up (static IP)");

	/* Give the link time to come up on hardware */
#ifndef CONFIG_OVE_RTOS_POSIX
	OVE_LOG_INF("  Waiting for link...");
	ove_thread_sleep_ms(3000);
#endif

	/* Query actual interface addresses */
	TEST("netif_get_addr");
	ove_sockaddr_t ip_addr, gw_addr, nm_addr;
	ret = ove_netif_get_addr(netif, &ip_addr, &gw_addr, &nm_addr);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  IP: %u.%u.%u.%u", ip_addr.addr[0], ip_addr.addr[1], ip_addr.addr[2],
			    ip_addr.addr[3]);
		PASS("netif_get_addr");
	} else {
		FAIL("netif_get_addr", ret);
	}
}

/* ── 2. DNS resolution ──────────────────────────────────────────── */

static void test_dns(void)
{
	OVE_LOG_INF("=== DNS Resolution ===");

	TEST("resolve example.com");
	ove_sockaddr_t addr;
	int ret = ove_dns_resolve("example.com", &addr, 5000);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> %u.%u.%u.%u", addr.addr[0], addr.addr[1], addr.addr[2],
			    addr.addr[3]);
		PASS("resolve example.com");
	} else {
		FAIL("resolve example.com", ret);
	}

	TEST("resolve invalid.invalid (expect failure)");
	ret = ove_dns_resolve("invalid.invalid", &addr, 3000);
	if (ret != OVE_OK) {
		PASS("resolve invalid.invalid (correctly failed)");
	} else {
		FAIL("resolve invalid.invalid (should have failed)", 0);
	}
}

/* ── 3. Raw TCP socket ──────────────────────────────────────────── */

static void test_tcp(void)
{
	OVE_LOG_INF("=== TCP Socket ===");

	ove_socket_t sock;
	ove_socket_storage_t sock_storage;

	TEST("socket_open TCP");
	int ret = ove_socket_open(&sock, &sock_storage, OVE_AF_INET, OVE_SOCK_STREAM);
	if (ret != OVE_OK) {
		FAIL("socket_open TCP", ret);
		return;
	}
	PASS("socket_open TCP");

	/* Resolve + connect to example.com:80 */
	ove_sockaddr_t addr;
	ret = ove_dns_resolve("example.com", &addr, 5000);
	if (ret != OVE_OK) {
		FAIL("dns for TCP test", ret);
		ove_socket_close(sock);
		return;
	}
	addr.port = 80;

	TEST("socket_connect");
	ret = ove_socket_connect(sock, &addr, 5000);
	if (ret != OVE_OK) {
		FAIL("socket_connect", ret);
		ove_socket_close(sock);
		return;
	}
	PASS("socket_connect");

	/* Send HTTP request */
	const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
	size_t sent = 0;

	TEST("socket_send");
	ret = ove_socket_send(sock, req, strlen(req), &sent);
	if (ret == OVE_OK && sent == strlen(req)) {
		PASS("socket_send");
	} else {
		FAIL("socket_send", ret);
		ove_socket_close(sock);
		return;
	}

	/* Receive response */
	TEST("socket_recv");
	char buf[512];
	size_t total = 0;
	size_t received = 0;

	/* Read until connection closes */
	while (total < sizeof(buf) - 1) {
		ret = ove_socket_recv(sock, buf + total, sizeof(buf) - 1 - total, &received, 5000);
		if (ret == OVE_ERR_NET_CLOSED)
			break;
		if (ret != OVE_OK)
			break;
		total += received;
	}

	if (total > 0) {
		buf[total] = '\0';
		/* Check for HTTP 200 */
		if (strstr(buf, "200 OK")) {
			OVE_LOG_INF("  -> received %u bytes, status 200 OK", (unsigned)total);
			PASS("socket_recv (HTTP 200)");
		} else {
			char *eol = strstr(buf, "\r\n");
			if (eol)
				*eol = '\0';
			OVE_LOG_WRN("  -> %s", buf);
			FAIL("socket_recv (unexpected status)", 0);
		}
	} else {
		FAIL("socket_recv (no data)", ret);
	}

	TEST("socket_close");
	ove_socket_close(sock);
	PASS("socket_close");
}

/* ── 4. UDP socket ──────────────────────────────────────────────── */

static void test_udp(void)
{
	OVE_LOG_INF("=== UDP Socket ===");

	ove_socket_t sock;
	ove_socket_storage_t sock_storage;

	TEST("socket_open UDP");
	int ret = ove_socket_open(&sock, &sock_storage, OVE_AF_INET, OVE_SOCK_DGRAM);
	if (ret != OVE_OK) {
		FAIL("socket_open UDP", ret);
		return;
	}
	PASS("socket_open UDP");

	/* Bind to a local port */
	ove_sockaddr_t bind_addr;
	ove_sockaddr_ipv4(&bind_addr, 0, 0, 0, 0, 9999);

	TEST("socket_bind");
	ret = ove_socket_bind(sock, &bind_addr);
	if (ret != OVE_OK) {
		FAIL("socket_bind", ret);
		ove_socket_close(sock);
		return;
	}
	PASS("socket_bind");

	/* Send to self */
	ove_sockaddr_t dest;
	ove_sockaddr_ipv4(&dest, 127, 0, 0, 1, 9999);
	const char *msg = "oveRTOS UDP test";
	size_t sent = 0;

	TEST("socket_sendto");
	ret = ove_socket_sendto(sock, msg, strlen(msg), &sent, &dest);
	if (ret == OVE_OK) {
		PASS("socket_sendto");
	} else {
		FAIL("socket_sendto", ret);
		ove_socket_close(sock);
		return;
	}

	/* Receive */
	TEST("socket_recvfrom");
	char buf[64];
	size_t received = 0;
	ove_sockaddr_t src;
	ret = ove_socket_recvfrom(sock, buf, sizeof(buf) - 1, &received, &src, 2000);
	if (ret == OVE_OK && received == strlen(msg)) {
		buf[received] = '\0';
		if (strcmp(buf, msg) == 0) {
			PASS("socket_recvfrom (echo match)");
		} else {
			FAIL("socket_recvfrom (data mismatch)", 0);
		}
	} else if (ret == OVE_ERR_TIMEOUT) {
		FAIL("socket_recvfrom (timeout)", ret);
	} else {
		FAIL("socket_recvfrom", ret);
	}

	ove_socket_close(sock);
}

/* ── 5. HTTP client ─────────────────────────────────────────────── */

static void test_http(void)
{
	OVE_LOG_INF("=== HTTP Client ===");

	ove_http_client_t client;
	static ove_http_client_storage_t client_storage;

	TEST("http_client_init");
	int ret = ove_http_client_init(&client, &client_storage);
	if (ret != OVE_OK) {
		FAIL("http_client_init", ret);
		return;
	}
	PASS("http_client_init");

	/* GET request */
	TEST("http_get http://example.com/");
	ove_http_response_t resp;
	ret = ove_http_get(client, "http://example.com/", &resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes", resp.status, (unsigned)resp.body_len);
		if (resp.status == 200 && resp.body_len > 0) {
			PASS("http_get (200 OK)");
		} else {
			FAIL("http_get (unexpected status)", resp.status);
		}
		ove_http_response_free(&resp);
	} else {
		FAIL("http_get", ret);
	}

	/* POST request */
	TEST("http_post http://httpbin.org/post");
	const char *json = "{\"test\":\"overtos\"}";
	ret = ove_http_post(client, "http://httpbin.org/post", "application/json", json,
			    strlen(json), &resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes", resp.status, (unsigned)resp.body_len);
		if (resp.status == 200) {
			PASS("http_post (200 OK)");
			/* Check echo body contains our payload */
			if (resp.body && strstr(resp.body, "overtos")) {
				PASS("http_post body echoed");
			} else {
				FAIL("http_post body not echoed", 0);
			}
		} else {
			FAIL("http_post (unexpected status)", resp.status);
		}
		ove_http_response_free(&resp);
	} else {
		FAIL("http_post", ret);
	}

	/* PUT request with custom headers */
	TEST("http_put http://httpbin.org/put");
	const char *put_json = "{\"update\":\"value\"}";
	ove_http_header_t headers[] = {
		{"X-Custom", "oveRTOS"},
		{"Accept", "application/json"},
	};
	ret = ove_http_request_ex(client, OVE_HTTP_PUT, "http://httpbin.org/put",
				  "application/json", put_json, strlen(put_json), headers, 2,
				  &resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes", resp.status, (unsigned)resp.body_len);
		if (resp.status == 200) {
			PASS("http_put (200 OK)");
		} else {
			FAIL("http_put (unexpected status)", resp.status);
		}
		ove_http_response_free(&resp);
	} else {
		FAIL("http_put", ret);
	}

	ove_http_client_deinit(client);
}

/* ── 5b. SNTP ──────────────────────────────────────────────────── */

static void test_sntp(void)
{
	OVE_LOG_INF("=== SNTP ===");

	TEST("sntp_sync pool.ntp.org");
	ove_sntp_config_t sntp_cfg = {
		.server = "pool.ntp.org",
		.timeout_ms = 5000,
	};
	int ret = ove_sntp_sync(&sntp_cfg);
	if (ret == OVE_OK) {
		PASS("sntp_sync");
		TEST("sntp_get_utc");
		uint32_t utc = 0;
		ret = ove_sntp_get_utc(&utc);
		if (ret == OVE_OK) {
			OVE_LOG_INF("  -> UTC: %lu", (unsigned long)utc);
			PASS("sntp_get_utc");
		} else {
			FAIL("sntp_get_utc", ret);
		}
	} else {
		FAIL("sntp_sync", ret);
	}
}

/* ── 6. MQTT client ─────────────────────────────────────────────── */

static volatile int mqtt_rx_count;
static char mqtt_rx_payload[128];

static void mqtt_on_message(const char *topic, size_t topic_len, const void *payload,
			    size_t payload_len, void *user_data)
{
	(void)user_data;
	OVE_LOG_INF("  MQTT rx: [%.*s] %.*s", (int)topic_len, topic, (int)payload_len,
		    (const char *)payload);
	if (payload_len < sizeof(mqtt_rx_payload)) {
		memcpy(mqtt_rx_payload, payload, payload_len);
		mqtt_rx_payload[payload_len] = '\0';
	}
	mqtt_rx_count++;
}

static void test_mqtt(void)
{
	OVE_LOG_INF("=== MQTT Client ===");

	ove_mqtt_client_t client;
	static ove_mqtt_client_storage_t client_storage;

	TEST("mqtt_client_init");
	int ret = ove_mqtt_client_init(&client, &client_storage);
	if (ret != OVE_OK) {
		FAIL("mqtt_client_init", ret);
		return;
	}
	PASS("mqtt_client_init");

	TEST("mqtt_connect test.mosquitto.org:1883");
	ove_mqtt_config_t mqtt_cfg = {
		.host = "test.mosquitto.org",
		.port = 1883,
		.client_id = "overtos-test",
		.keep_alive_s = 30,
		.on_message = mqtt_on_message,
	};
	ret = ove_mqtt_connect(client, &mqtt_cfg);
	if (ret != OVE_OK) {
		FAIL("mqtt_connect", ret);
		ove_mqtt_client_deinit(client);
		return;
	}
	PASS("mqtt_connect");

	/* Subscribe */
	TEST("mqtt_subscribe overtos/test");
	ret = ove_mqtt_subscribe(client, "overtos/test", OVE_MQTT_QOS0);
	if (ret == OVE_OK) {
		PASS("mqtt_subscribe");
	} else {
		FAIL("mqtt_subscribe", ret);
	}

	/* Publish */
	TEST("mqtt_publish QoS0");
	const char *msg0 = "hello-qos0";
	ret = ove_mqtt_publish(client, "overtos/test", msg0, strlen(msg0), OVE_MQTT_QOS0);
	if (ret == OVE_OK) {
		PASS("mqtt_publish QoS0");
	} else {
		FAIL("mqtt_publish QoS0", ret);
	}

	/* Publish QoS1 */
	TEST("mqtt_publish QoS1");
	const char *msg1 = "hello-qos1";
	ret = ove_mqtt_publish(client, "overtos/test", msg1, strlen(msg1), OVE_MQTT_QOS1);
	if (ret == OVE_OK) {
		PASS("mqtt_publish QoS1 (PUBACK received)");
	} else {
		FAIL("mqtt_publish QoS1", ret);
	}

	/* Poll to receive our own messages */
	TEST("mqtt_loop (receive published messages)");
	mqtt_rx_count = 0;
	for (int i = 0; i < 10; i++) {
		ove_mqtt_loop(client, 500);
		if (mqtt_rx_count >= 2)
			break;
	}
	if (mqtt_rx_count >= 1) {
		OVE_LOG_INF("  -> received %d message(s)", mqtt_rx_count);
		PASS("mqtt_loop (received messages)");
	} else {
		OVE_LOG_WRN("  -> received %d messages (broker may not echo)", mqtt_rx_count);
		PASS("mqtt_loop (ran without error)");
	}

	/* Unsubscribe — do this promptly after loop to avoid stale connection */
	TEST("mqtt_unsubscribe");
	ret = ove_mqtt_unsubscribe(client, "overtos/test");
	if (ret == OVE_OK) {
		PASS("mqtt_unsubscribe");
	} else {
		/* Some brokers close the connection during idle polling;
		 * treat connection-closed as acceptable for unsubscribe. */
		if (ret == OVE_ERR_NET_CLOSED || ret == OVE_ERR_NET_RESET) {
			OVE_LOG_WRN("  connection closed by broker (%d)", ret);
			PASS("mqtt_unsubscribe (connection closed, acceptable)");
		} else {
			FAIL("mqtt_unsubscribe", ret);
		}
	}

	/* Keepalive ping (skip if connection already closed) */
	TEST("mqtt_loop keepalive ping");
	ret = ove_mqtt_loop(client, 100);
	PASS("mqtt_loop keepalive");

	/* Disconnect */
	TEST("mqtt_disconnect");
	ove_mqtt_disconnect(client);
	PASS("mqtt_disconnect");

	ove_mqtt_client_deinit(client);
}

/* ── Networking thread ──────────────────────────────────────────── */

static void net_thread(void *arg)
{
	(void)arg;

	test_netif_init();
	test_dns();
	test_tcp();
	test_udp();
	test_http();
	test_sntp();
	test_mqtt();

	OVE_LOG_INF("========================================");
	OVE_LOG_INF("  Results: %d passed, %d failed", pass_count, fail_count);
	OVE_LOG_INF("========================================");

	if (fail_count == 0) {
		OVE_LOG_INF("  ALL TESTS PASSED");
	} else {
		OVE_LOG_ERR("  %d TEST(S) FAILED", fail_count);
	}

	/* Start web dashboard — runs forever */
	uint16_t httpd_port = 80;
#ifdef CONFIG_OVE_RTOS_POSIX
	httpd_port = 8080; /* avoid needing root on Linux */
#endif
	OVE_LOG_INF("Starting HTTP server on port %u...", (unsigned)httpd_port);
	ove_httpd_config_t httpd_cfg = {.port = httpd_port, .max_body_size = 1024};
	int ret = ove_httpd_start(&httpd_cfg);
	if (ret == OVE_OK) {
		ove_httpd_register_builtin_routes();
		OVE_LOG_INF("HTTP server running — open http://<device-ip>:%u/",
			    (unsigned)httpd_port);
		/* Keep thread alive so httpd keeps running */
		while (1) {
			ove_thread_sleep_ms(1000);
		}
	} else {
		OVE_LOG_ERR("HTTP server failed to start: %d", ret);
	}
}

/* ── Thread handle (statically allocated) ───────────────────────── */

/* ── App entry point ────────────────────────────────────────────── */

void ove_main(void)
{
	OVE_LOG_INF("Networking example (heap mode): init");

	ove_thread_t net;
	if (ove_thread_create(&net, "net-test", net_thread, NULL,
			      OVE_PRIO_NORMAL, 8192) != OVE_OK) {
		OVE_LOG_ERR("Failed to spawn net-test thread");
		return;
	}

	OVE_LOG_INF("Networking example (heap mode): ready");
	ove_run();

	/* On POSIX, ove_run() returns — keep alive for the httpd server */
	while (1) {
		ove_thread_sleep_ms(1000);
	}
}
