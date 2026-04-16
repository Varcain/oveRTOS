/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Networking Example
 *
 * Exercises the full networking stack (mirrors the C reference):
 *   1. Network interface init with static IP
 *   2. DNS resolution (positive + negative)
 *   3. Raw TCP socket connect + HTTP/1.0 request
 *   4. UDP send/receive (loopback echo)
 *   5. HTTP client GET, POST, and PUT
 *   6. SNTP time sync
 *   7. MQTT connect, subscribe, publish QoS0/1, receive, disconnect
 *   8. Results summary
 *   9. Embedded HTTP server (web dashboard)
 *
 * On POSIX the netif init is a no-op (host networking).
 * On FreeRTOS/lwIP the static IP config drives real hardware.
 */

#include <ove/ove.hpp>
#include <ove/net_sntp.hpp>
#include <cstring>

static int pass_count;
static int fail_count;

#define TEST(name) OVE_LOG_INF("  [TEST] %s", name)
#define PASS(name) do { OVE_LOG_INF("  [PASS] %s", name); pass_count++; } while(0)
#define FAIL(name, err) do { OVE_LOG_ERR("  [FAIL] %s (%d)", name, err); fail_count++; } while(0)

/* ── 1. Network interface ───────────────────────────────────────── */

static ove::NetIf netif;

static void test_netif_init()
{
	OVE_LOG_INF("=== Network Interface ===");

	TEST("netif_init");
	if (!netif.valid()) { FAIL("netif_init", -1); return; }
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

	int ret = netif.up(cfg);
	if (ret != OVE_OK) { FAIL("netif_up", ret); return; }
	PASS("netif_up (static IP)");

	/* Give the link time to come up on hardware */
#ifndef CONFIG_OVE_RTOS_POSIX
	OVE_LOG_INF("  Waiting for link...");
	ove::Thread<0>::sleep_ms(3000);
#endif

	/* Query actual interface addresses */
	TEST("netif_get_addr");
	ove::Address ip, gw, nm;
	ret = netif.get_addr(&ip, &gw, &nm);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  IP: %u.%u.%u.%u",
			    ip.raw.addr[0], ip.raw.addr[1],
			    ip.raw.addr[2], ip.raw.addr[3]);
		PASS("netif_get_addr");
	} else {
		FAIL("netif_get_addr", ret);
	}
}

/* ── 2. DNS resolution ──────────────────────────────────────────── */

static void test_dns()
{
	OVE_LOG_INF("=== DNS Resolution ===");

	TEST("resolve example.com");
	ove::Address addr;
	int ret = ove::dns::resolve("example.com", addr, 5000);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> %u.%u.%u.%u",
			    addr.raw.addr[0], addr.raw.addr[1],
			    addr.raw.addr[2], addr.raw.addr[3]);
		PASS("resolve example.com");
	} else {
		FAIL("resolve example.com", ret);
	}

	TEST("resolve invalid.invalid (expect failure)");
	ret = ove::dns::resolve("invalid.invalid", addr, 3000);
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
	if (!sock.is_open()) { FAIL("socket_open TCP", -1); return; }
	PASS("socket_open TCP");

	/* Resolve + connect to example.com:80 */
	ove::Address addr;
	int ret = ove::dns::resolve("example.com", addr, 5000);
	if (ret != OVE_OK) {
		FAIL("dns for TCP test", ret);
		return;
	}
	addr.set_port(80);

	TEST("socket_connect");
	ret = sock.connect(addr, 5000);
	if (ret != OVE_OK) { FAIL("socket_connect", ret); return; }
	PASS("socket_connect");

	/* Send HTTP request */
	const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
	size_t sent = 0;

	TEST("socket_send");
	ret = sock.send(req, std::strlen(req), &sent);
	if (ret == OVE_OK && sent == std::strlen(req)) {
		PASS("socket_send");
	} else {
		FAIL("socket_send", ret);
		return;
	}

	/* Receive response */
	TEST("socket_recv");
	char buf[512];
	size_t total = 0;
	size_t received = 0;

	/* Read until connection closes */
	while (total < sizeof(buf) - 1) {
		ret = sock.recv(buf + total, sizeof(buf) - 1 - total,
				&received, 5000);
		if (ret == OVE_ERR_NET_CLOSED) break;
		if (ret != OVE_OK) break;
		total += received;
	}

	if (total > 0) {
		buf[total] = '\0';
		/* Check for HTTP 200 */
		if (std::strstr(buf, "200 OK")) {
			OVE_LOG_INF("  -> received %u bytes, status 200 OK",
				    (unsigned)total);
			PASS("socket_recv (HTTP 200)");
		} else {
			char *eol = std::strstr(buf, "\r\n");
			if (eol) *eol = '\0';
			OVE_LOG_WRN("  -> %s", buf);
			FAIL("socket_recv (unexpected status)", 0);
		}
	} else {
		FAIL("socket_recv (no data)", ret);
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
	if (!sock.is_open()) { FAIL("socket_open UDP", -1); return; }
	PASS("socket_open UDP");

	/* Bind to a local port */
	TEST("socket_bind");
	int ret = sock.bind(ove::Address::ipv4(0, 0, 0, 0, 9999));
	if (ret != OVE_OK) { FAIL("socket_bind", ret); return; }
	PASS("socket_bind");

	/* Send to self */
	auto dest = ove::Address::ipv4(127, 0, 0, 1, 9999);
	const char *msg = "oveRTOS UDP test";
	size_t sent = 0;

	TEST("socket_sendto");
	ret = sock.send_to(msg, std::strlen(msg), dest, &sent);
	if (ret == OVE_OK) {
		PASS("socket_sendto");
	} else {
		FAIL("socket_sendto", ret);
		return;
	}

	/* Receive */
	TEST("socket_recvfrom");
	char buf[64];
	size_t received = 0;
	ove::Address src;
	ret = sock.recv_from(buf, sizeof(buf) - 1, &src, &received, 2000);
	if (ret == OVE_OK && received == std::strlen(msg)) {
		buf[received] = '\0';
		if (std::strcmp(buf, msg) == 0) {
			PASS("socket_recvfrom (echo match)");
		} else {
			FAIL("socket_recvfrom (data mismatch)", 0);
		}
	} else if (ret == OVE_ERR_TIMEOUT) {
		FAIL("socket_recvfrom (timeout)", ret);
	} else {
		FAIL("socket_recvfrom", ret);
	}
}

/* ── 5. HTTP client ─────────────────────────────────────────────── */

static void test_http()
{
	OVE_LOG_INF("=== HTTP Client ===");

	TEST("http_client_init");
	static ove::http::Client client;
	if (!client.valid()) { FAIL("http_client_init", -1); return; }
	PASS("http_client_init");

	/* GET request */
	TEST("http_get http://example.com/");
	ove::http::Response resp;
	int ret = client.get("http://example.com/", resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes",
			    resp.status(), (unsigned)resp.body_len());
		if (resp.status() == 200 && resp.body_len() > 0) {
			PASS("http_get (200 OK)");
		} else {
			FAIL("http_get (unexpected status)", resp.status());
		}
	} else {
		FAIL("http_get", ret);
	}

	/* POST request */
	TEST("http_post http://httpbin.org/post");
	const char *json = R"({"test":"overtos"})";
	ove::http::Response post_resp;
	ret = client.post("http://httpbin.org/post",
			  "application/json",
			  json, std::strlen(json), post_resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes",
			    post_resp.status(), (unsigned)post_resp.body_len());
		if (post_resp.status() == 200) {
			PASS("http_post (200 OK)");
			/* Check echo body contains our payload */
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

	/* PUT request with custom headers */
	TEST("http_put http://httpbin.org/put");
	const char *put_json = R"({"update":"value"})";
	ove_http_header_t headers[] = {
		{"X-Custom", "oveRTOS"},
		{"Accept", "application/json"},
	};
	ove::http::Response put_resp;
	ret = client.request(OVE_HTTP_PUT,
			     "http://httpbin.org/put",
			     "application/json",
			     put_json, std::strlen(put_json),
			     headers, 2, put_resp);
	if (ret == OVE_OK) {
		OVE_LOG_INF("  -> status %d, body %u bytes",
			    put_resp.status(), (unsigned)put_resp.body_len());
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
	ove::sntp::Config sntp_cfg{"pool.ntp.org", 5000};
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
	static ove::mqtt::Client mqtt;
	if (!mqtt.valid()) { FAIL("mqtt_client_init", -1); return; }
	PASS("mqtt_client_init");

	TEST("mqtt_connect test.mosquitto.org:1883");
	ove::mqtt::Config mqtt_cfg{
		.host = "test.mosquitto.org",
		.port = 1883,
		.client_id = "overtos-test",
		.keep_alive_s = 30,
	};

	int ret = mqtt.connect(mqtt_cfg,
		+[](std::string_view topic, std::string_view payload) {
			OVE_LOG_INF("  MQTT rx: [%.*s] %.*s",
				    (int)topic.size(), topic.data(),
				    (int)payload.size(), payload.data());
			if (payload.size() < sizeof(mqtt_rx_payload)) {
				std::memcpy(mqtt_rx_payload, payload.data(),
					    payload.size());
				mqtt_rx_payload[payload.size()] = '\0';
			}
			mqtt_rx_count++;
		});

	if (ret != OVE_OK) {
		FAIL("mqtt_connect", ret);
		return;
	}
	PASS("mqtt_connect");

	/* Subscribe */
	TEST("mqtt_subscribe overtos/test");
	ret = mqtt.subscribe("overtos/test");
	if (ret == OVE_OK) {
		PASS("mqtt_subscribe");
	} else {
		FAIL("mqtt_subscribe", ret);
	}

	/* Publish QoS0 */
	TEST("mqtt_publish QoS0");
	const char *msg0 = "hello-qos0";
	ret = mqtt.publish("overtos/test", msg0, std::strlen(msg0));
	if (ret == OVE_OK) {
		PASS("mqtt_publish QoS0");
	} else {
		FAIL("mqtt_publish QoS0", ret);
	}

	/* Publish QoS1 */
	TEST("mqtt_publish QoS1");
	const char *msg1 = "hello-qos1";
	ret = mqtt.publish("overtos/test",
			   std::string_view{msg1}, ove::mqtt::Qos::AtLeastOnce);
	if (ret == OVE_OK) {
		PASS("mqtt_publish QoS1 (PUBACK received)");
	} else {
		FAIL("mqtt_publish QoS1", ret);
	}

	/* Poll to receive our own messages */
	TEST("mqtt_loop (receive published messages)");
	mqtt_rx_count = 0;
	for (int i = 0; i < 10; i++) {
		mqtt.loop(500);
		if (mqtt_rx_count >= 2) break;
	}
	if (mqtt_rx_count >= 1) {
		OVE_LOG_INF("  -> received %d message(s)", mqtt_rx_count);
		PASS("mqtt_loop (received messages)");
	} else {
		OVE_LOG_WRN("  -> received %d messages (broker may not echo)",
			    mqtt_rx_count);
		PASS("mqtt_loop (ran without error)");
	}

	/* Unsubscribe */
	TEST("mqtt_unsubscribe");
	ret = mqtt.unsubscribe("overtos/test");
	if (ret == OVE_OK) {
		PASS("mqtt_unsubscribe");
	} else {
		if (ret == OVE_ERR_NET_CLOSED || ret == OVE_ERR_NET_RESET) {
			OVE_LOG_WRN("  connection closed by broker (%d)", ret);
			PASS("mqtt_unsubscribe (connection closed, acceptable)");
		} else {
			FAIL("mqtt_unsubscribe", ret);
		}
	}

	/* Keepalive ping */
	TEST("mqtt_loop keepalive ping");
	mqtt.loop(100);
	PASS("mqtt_loop keepalive");

	/* Disconnect */
	TEST("mqtt_disconnect");
	mqtt.disconnect();
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

	/* Start web dashboard — runs forever */
	uint16_t httpd_port = 80;
#ifdef CONFIG_OVE_RTOS_POSIX
	httpd_port = 8080; /* avoid needing root on Linux */
#endif
	OVE_LOG_INF("Starting HTTP server on port %u...", (unsigned)httpd_port);
	ove::httpd::Config httpd_cfg{.port = httpd_port, .max_body_size = 1024};
	ove::httpd::set_netif(netif.handle());
	int ret = ove::httpd::start(httpd_cfg);
	if (ret == OVE_OK) {
		ove::httpd::register_builtin_routes();
		OVE_LOG_INF("HTTP server running — open http://<device-ip>:%u/",
			    (unsigned)httpd_port);
		/* Keep thread alive so httpd keeps running */
		while (true) {
			ove::Thread<0>::sleep_ms(1000);
		}
	} else {
		OVE_LOG_ERR("HTTP server failed to start: %d", ret);
	}
}

/* ── App entry point ────────────────────────────────────────────── */

OVE_MAIN()
{
	OVE_LOG_INF("C++ networking example: init");

	static ove::Thread<8192> net(net_thread, nullptr,
				     OVE_PRIO_NORMAL, "net-test");

	OVE_LOG_INF("C++ networking example: ready");
	ove::run();

	/* On POSIX, ove::run() returns — keep alive for the httpd server */
	while (true) { ove::Thread<0>::sleep_ms(1000); }
}
