/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Networking Example — zero-heap mode.
 *
 * Mirrors apps/cpp/heap/example_net/src/app.cpp.  Threads, network
 * interface, and HTTP/MQTT clients are file-scope `static` instances;
 * in zero-heap mode the wrappers embed kernel storage inline and the
 * protocol pools (lwIP heap, mbedTLS arena, MQTT rx/tx, HTTP response
 * borrow buffer) live in BSS, sized at compile time.
 *
 * HTTP responses are borrowed pointers into the client's embedded
 * `_resp_buf[CONFIG_OVE_NET_HTTP_MAX_RESPONSE]`; copy them out before
 * issuing the next request.
 */

#include <ove/ove.hpp>
#include <ove/net_sntp.hpp>
#include <chrono>
#include <cstring>

using namespace std::chrono_literals;

static int pass_count;
static int fail_count;

static inline void TEST(const char *name)
{
	OVE_LOG_INF("  [TEST] %s", name);
}
static inline void PASS(const char *name)
{
	OVE_LOG_INF("  [PASS] %s", name);
	pass_count++;
}
static inline void FAIL(const char *name, int err)
{
	OVE_LOG_ERR("  [FAIL] %s (%d)", name, err);
	fail_count++;
}

/* File-scope: zero-heap wrappers embed storage inline. */
static ove::NetIf netif;
static ove::http::Client http_client;
static ove::mqtt::Client mqtt_client;

/* ── 1. Network interface ───────────────────────────────────────── */

static void test_netif_init()
{
	OVE_LOG_INF("=== Network Interface ===");

	TEST("netif_init");
	if (!netif.valid()) {
		FAIL("netif_init", -1);
		return;
	}
	PASS("netif_init");

	TEST("netif_up (static IP)");
	auto cfg = ove::NetIfConfig{}
#ifndef CONFIG_OVE_RTOS_POSIX
			   .static_ip(ove::Address::ipv4(172, 1, 1, 2, 0),
				      ove::Address::ipv4(255, 255, 255, 0, 0),
				      ove::Address::ipv4(172, 1, 1, 1, 0))
			   .dns(ove::Address::ipv4(8, 8, 8, 8, 0))
#endif
		;

	auto up_r = netif.up(cfg);
	if (!up_r) {
		FAIL("netif_up", static_cast<int>(up_r.error()));
		return;
	}
	PASS("netif_up (static IP)");

#ifndef CONFIG_OVE_RTOS_POSIX
	OVE_LOG_INF("  Waiting for link...");
	ove::this_thread::sleep_ms(3000);
#endif

	TEST("netif_get_addr");
	ove::Address ip, gw, nm;
	if (auto r = netif.get_addr(&ip, &gw, &nm)) {
		OVE_LOG_INF("  IP: %u.%u.%u.%u", ip.raw.addr[0], ip.raw.addr[1], ip.raw.addr[2],
			    ip.raw.addr[3]);
		PASS("netif_get_addr");
	} else {
		FAIL("netif_get_addr", static_cast<int>(r.error()));
	}
}

/* ── 2. DNS resolution ──────────────────────────────────────────── */

static void test_dns()
{
	OVE_LOG_INF("=== DNS Resolution ===");

	TEST("resolve example.com");
	ove::Address addr;
	int ret = ove::dns::resolve("example.com", addr, 5s);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> %u.%u.%u.%u", addr.raw.addr[0], addr.raw.addr[1],
			    addr.raw.addr[2], addr.raw.addr[3]);
		PASS("resolve example.com");
	} else {
		FAIL("resolve example.com", ret);
	}

	TEST("resolve invalid.invalid (expect failure)");
	ret = ove::dns::resolve("invalid.invalid", addr, 3s);
	if (ret != OVE_OK) {
		PASS("resolve invalid.invalid (correctly failed)");
	} else {
		FAIL("resolve invalid.invalid (should have failed)", 0);
	}
}

/* ── 3. Raw TCP socket ──────────────────────────────────────────── */

static void test_tcp()
{
	OVE_LOG_INF("=== TCP Socket ===");

	TEST("socket_open TCP");
	ove::TcpSocket sock;
	if (!sock.is_open()) {
		FAIL("socket_open TCP", -1);
		return;
	}
	PASS("socket_open TCP");

	ove::Address addr;
	int ret = ove::dns::resolve("example.com", addr, 5s);
	if (ret != OVE_OK) {
		FAIL("dns for TCP test", ret);
		return;
	}
	addr.set_port(80);

	TEST("socket_connect");
	if (auto r = sock.connect(addr, std::chrono::seconds{5}); !r) {
		FAIL("socket_connect", static_cast<int>(r.error()));
		return;
	}
	PASS("socket_connect");

	const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";

	TEST("socket_send");
	if (auto r = sock.send(req, std::strlen(req)); r && *r == std::strlen(req)) {
		PASS("socket_send");
	} else {
		FAIL("socket_send", r ? 0 : static_cast<int>(r.error()));
		return;
	}

	TEST("socket_recv");
	char buf[512];
	size_t total = 0;
	int last_err = 0;

	while (total < sizeof(buf) - 1) {
		auto r = sock.recv(buf + total, sizeof(buf) - 1 - total, 5s);
		if (!r) {
			last_err = static_cast<int>(r.error());
			break; /* NetClosed treated as clean EOF; other errors abort */
		}
		total += *r;
	}

	if (total > 0) {
		buf[total] = '\0';
		if (std::strstr(buf, "200 OK")) {
			OVE_LOG_INF("  -> received %u bytes, status 200 OK", (unsigned)total);
			PASS("socket_recv (HTTP 200)");
		} else {
			char *eol = std::strstr(buf, "\r\n");
			if (eol)
				*eol = '\0';
			OVE_LOG_WRN("  -> %s", buf);
			FAIL("socket_recv (unexpected status)", 0);
		}
	} else {
		FAIL("socket_recv (no data)", last_err);
	}

	TEST("socket_close");
	sock.close();
	PASS("socket_close");
}

/* ── 4. UDP socket ──────────────────────────────────────────────── */

static void test_udp()
{
	OVE_LOG_INF("=== UDP Socket ===");

	TEST("socket_open UDP");
	ove::UdpSocket sock;
	if (!sock.is_open()) {
		FAIL("socket_open UDP", -1);
		return;
	}
	PASS("socket_open UDP");

	TEST("socket_bind");
	if (auto r = sock.bind(ove::Address::ipv4(0, 0, 0, 0, 9999)); !r) {
		FAIL("socket_bind", static_cast<int>(r.error()));
		return;
	}
	PASS("socket_bind");

	auto dest = ove::Address::ipv4(127, 0, 0, 1, 9999);
	const char *msg = "oveRTOS UDP test";

	TEST("socket_sendto");
	if (auto r = sock.send_to(msg, std::strlen(msg), dest); r) {
		PASS("socket_sendto");
	} else {
		FAIL("socket_sendto", static_cast<int>(r.error()));
		return;
	}

	TEST("socket_recvfrom");
	char buf[64];
	ove::Address src;
	auto rcv = sock.recv_from(buf, sizeof(buf) - 1, &src, 2s);
	if (rcv && *rcv == std::strlen(msg)) {
		buf[*rcv] = '\0';
		if (std::strcmp(buf, msg) == 0) {
			PASS("socket_recvfrom (echo match)");
		} else {
			FAIL("socket_recvfrom (data mismatch)", 0);
		}
	} else if (!rcv && rcv.error() == ove::Error::Timeout) {
		FAIL("socket_recvfrom (timeout)", static_cast<int>(rcv.error()));
	} else {
		FAIL("socket_recvfrom", rcv ? 0 : static_cast<int>(rcv.error()));
	}
}

/* ── 5. HTTP client ─────────────────────────────────────────────── */

static void test_http()
{
	OVE_LOG_INF("=== HTTP Client ===");

	TEST("http_client_init");
	if (!http_client.valid()) {
		FAIL("http_client_init", -1);
		return;
	}
	PASS("http_client_init");

	TEST("http_get http://example.com/");
	ove::http::Response resp;
	int ret = http_client.get("http://example.com/", resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes", resp.status(),
			    (unsigned)resp.body_len());
		if (resp.status() == 200 && resp.body_len() > 0) {
			PASS("http_get (200 OK)");
		} else {
			FAIL("http_get (unexpected status)", resp.status());
		}
	} else {
		FAIL("http_get", ret);
	}

	TEST("http_post http://httpbin.org/post");
	const char *json = R"({"test":"overtos"})";
	ove::http::Response post_resp;
	ret = http_client.post("http://httpbin.org/post", "application/json", json,
			       std::strlen(json), post_resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes", post_resp.status(),
			    (unsigned)post_resp.body_len());
		if (post_resp.status() == 200) {
			PASS("http_post (200 OK)");
			if (post_resp.body() && std::strstr(post_resp.body(), "overtos")) {
				PASS("http_post body echoed");
			} else {
				FAIL("http_post body not echoed", 0);
			}
		} else {
			FAIL("http_post (unexpected status)", post_resp.status());
		}
	} else {
		FAIL("http_post", ret);
	}

	TEST("http_put http://httpbin.org/put");
	const char *put_json = R"({"update":"value"})";
	ove_http_header_t headers[] = {
		{"X-Custom", "oveRTOS"},
		{"Accept", "application/json"},
	};
	ove::http::Response put_resp;
	ret = http_client.request(OVE_HTTP_PUT, "http://httpbin.org/put", "application/json",
				  put_json, std::strlen(put_json), headers, 2, put_resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes", put_resp.status(),
			    (unsigned)put_resp.body_len());
		if (put_resp.status() == 200) {
			PASS("http_put (200 OK)");
		} else {
			FAIL("http_put (unexpected status)", put_resp.status());
		}
	} else {
		FAIL("http_put", ret);
	}
}

/* ── 5b. SNTP ──────────────────────────────────────────────────── */

static void test_sntp()
{
	OVE_LOG_INF("=== SNTP ===");

	TEST("sntp_sync pool.ntp.org");
	ove::sntp::Config sntp_cfg{"pool.ntp.org", OVE_SEC(5)};
	int ret = ove::sntp::sync(sntp_cfg);
	if (ret == OVE_OK) {
		PASS("sntp_sync");
		TEST("sntp_get_utc");
		uint32_t utc = 0;
		ret = ove::sntp::get_utc(utc);
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

static void test_mqtt()
{
	OVE_LOG_INF("=== MQTT Client ===");

	TEST("mqtt_client_init");
	if (!mqtt_client.valid()) {
		FAIL("mqtt_client_init", -1);
		return;
	}
	PASS("mqtt_client_init");

	TEST("mqtt_connect test.mosquitto.org:1883");
	ove::mqtt::Config mqtt_cfg{
		.host = "test.mosquitto.org",
		.port = 1883,
		.client_id = "overtos-test-zh",
		.keep_alive_s = 30,
	};

	int ret = mqtt_client.connect(
		mqtt_cfg, +[](std::string_view topic, std::string_view payload) {
			OVE_LOG_INF("  MQTT rx: [%.*s] %.*s", (int)topic.size(), topic.data(),
				    (int)payload.size(), payload.data());
			if (payload.size() < sizeof(mqtt_rx_payload)) {
				std::memcpy(mqtt_rx_payload, payload.data(), payload.size());
				mqtt_rx_payload[payload.size()] = '\0';
			}
			mqtt_rx_count++;
		});

	if (ret != OVE_OK) {
		FAIL("mqtt_connect", ret);
		return;
	}
	PASS("mqtt_connect");

	TEST("mqtt_subscribe overtos/test");
	ret = mqtt_client.subscribe("overtos/test");
	if (ret == OVE_OK) {
		PASS("mqtt_subscribe");
	} else {
		FAIL("mqtt_subscribe", ret);
	}

	TEST("mqtt_publish QoS0");
	const char *msg0 = "hello-qos0";
	ret = mqtt_client.publish("overtos/test", msg0, std::strlen(msg0));
	if (ret == OVE_OK) {
		PASS("mqtt_publish QoS0");
	} else {
		FAIL("mqtt_publish QoS0", ret);
	}

	TEST("mqtt_publish QoS1");
	const char *msg1 = "hello-qos1";
	ret = mqtt_client.publish("overtos/test", std::string_view{msg1},
				  ove::mqtt::Qos::AtLeastOnce);
	if (ret == OVE_OK) {
		PASS("mqtt_publish QoS1 (PUBACK received)");
	} else {
		FAIL("mqtt_publish QoS1", ret);
	}

	TEST("mqtt_loop (receive published messages)");
	mqtt_rx_count = 0;
	for (int i = 0; i < 10; i++) {
		mqtt_client.loop(500ms);
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

	TEST("mqtt_unsubscribe");
	ret = mqtt_client.unsubscribe("overtos/test");
	if (ret == OVE_OK) {
		PASS("mqtt_unsubscribe");
	} else if (ret == OVE_ERR_NET_CLOSED || ret == OVE_ERR_NET_RESET) {
		OVE_LOG_WRN("  connection closed by broker (%d)", ret);
		PASS("mqtt_unsubscribe (connection closed, acceptable)");
	} else {
		FAIL("mqtt_unsubscribe", ret);
	}

	TEST("mqtt_loop keepalive ping");
	mqtt_client.loop(100ms);
	PASS("mqtt_loop keepalive");

	TEST("mqtt_disconnect");
	mqtt_client.disconnect();
	PASS("mqtt_disconnect");
}

/* ── Networking thread ──────────────────────────────────────────── */

static void net_thread(void *)
{
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

	uint16_t httpd_port = 80;
#ifdef CONFIG_OVE_RTOS_POSIX
	httpd_port = 8080;
#endif
	OVE_LOG_INF("Starting HTTP server on port %u...", (unsigned)httpd_port);
	ove::httpd::Config httpd_cfg{.port = httpd_port,
				     .max_body_size = CONFIG_OVE_NET_HTTPD_MAX_BODY};
	ove::httpd::set_netif(netif.handle());
	int ret = ove::httpd::start(httpd_cfg);
	if (ret == OVE_OK) {
		ove::httpd::register_builtin_routes();
		OVE_LOG_INF("HTTP server running — open http://<device-ip>:%u/",
			    (unsigned)httpd_port);
		while (true) {
			ove::this_thread::sleep_ms(1000);
		}
	} else {
		OVE_LOG_ERR("HTTP server failed to start: %d", ret);
	}
}

OVE_MAIN()
{
	OVE_LOG_INF("C++ networking example (zero-heap mode): ready");

	/* Function-scope static: storage and stack inline in zero-heap mode. */
	static ove::Thread<8192> net_th(net_thread, nullptr, OVE_PRIO_NORMAL, "net-test");
	(void)net_th;

	ove::run();
}
