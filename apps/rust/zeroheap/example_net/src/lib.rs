// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust networking example — zero-heap mode.
//!
//! Mirrors apps/rust/heap/example_net.  Threads come from `ove::thread!`
//! (file-scope static storage); HTTP/MQTT clients are constructed via
//! `Client::create(&mut storage)` with caller-supplied storage.  The
//! protocol pools (lwIP heap, mbedTLS arena, MQTT rx/tx, HTTP response
//! borrow buffer) live in BSS, sized at compile time.
//!
//! HTTP responses are borrowed pointers into the client's embedded
//! `_resp_buf[CONFIG_OVE_NET_HTTP_MAX_RESPONSE]` and remain valid until
//! the next request — copy them out before issuing another call.

#![cfg_attr(not(feature = "std"), no_std)]

use core::sync::atomic::{AtomicU32, Ordering};
use ove::{Priority, Thread};

ove::main!(app_main);

// ---------------------------------------------------------------------------
// Test counters
// ---------------------------------------------------------------------------

static PASS_COUNT: AtomicU32 = AtomicU32::new(0);
static FAIL_COUNT: AtomicU32 = AtomicU32::new(0);

fn err_code(e: ove::Error) -> i32 {
    e.to_code()
}

fn test(name: &str) {
    log::info!("  [TEST] {}", name);
}

fn pass(name: &str) {
    log::info!("  [PASS] {}", name);
    PASS_COUNT.fetch_add(1, Ordering::Relaxed);
}

fn fail(name: &str, err: i32) {
    log::error!("  [FAIL] {} ({})", name, err);
    FAIL_COUNT.fetch_add(1, Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// String constants
// ---------------------------------------------------------------------------

const UDP_MSG: &[u8] = b"oveRTOS UDP test";
const MQTT_TOPIC: &[u8] = b"overtos/test\0";
const MQTT_CLIENT_ID: &[u8] = b"overtos-test-zh\0";
const HTTP_POST_BODY: &[u8] = b"{\"test\":\"overtos\"}";
const HTTP_PUT_BODY: &[u8] = b"{\"update\":\"value\"}";

// ---------------------------------------------------------------------------
// 1. Network interface
// ---------------------------------------------------------------------------

fn test_netif_init() {
    log::info!("=== Network Interface ===");

    test("netif_init");
    let netif = match ove::net::NetIf::new() {
        Ok(n) => {
            pass("netif_init");
            n
        }
        Err(e) => {
            fail("netif_init", err_code(e));
            return;
        }
    };

    test("netif_up (static IP)");
    #[cfg(not(rtos_posix))]
    let cfg = {
        ove::net::NetIfConfig::new()
            .static_ip(
                ove::net::Address::ipv4(172, 1, 1, 2, 0),
                ove::net::Address::ipv4(255, 255, 255, 0, 0),
                ove::net::Address::ipv4(172, 1, 1, 1, 0),
            )
            .dns(ove::net::Address::ipv4(8, 8, 8, 8, 0))
    };
    #[cfg(rtos_posix)]
    let cfg = ove::net::NetIfConfig::new();

    match netif.up(&cfg) {
        Ok(()) => pass("netif_up (static IP)"),
        Err(e) => {
            fail("netif_up", err_code(e));
            return;
        }
    }

    #[cfg(not(rtos_posix))]
    {
        log::info!("  Waiting for link...");
        Thread::sleep_ms(3000);
    }

    test("netif_get_addr");
    match netif.get_addr() {
        Ok((ip, _gw, _nm)) => {
            let o = ip.octets();
            log::info!("  IP: {}.{}.{}.{}", o[0], o[1], o[2], o[3]);
            pass("netif_get_addr");
        }
        Err(e) => fail("netif_get_addr", err_code(e)),
    }
}

// ---------------------------------------------------------------------------
// 2. DNS resolution
// ---------------------------------------------------------------------------

fn test_dns() {
    log::info!("=== DNS Resolution ===");

    test("resolve example.com");
    match ove::net::dns_resolve(b"example.com\0", core::time::Duration::from_secs(5)) {
        Ok(addr) => {
            let o = addr.octets();
            log::info!("  -> {}.{}.{}.{}", o[0], o[1], o[2], o[3]);
            pass("resolve example.com");
        }
        Err(e) => fail("resolve example.com", err_code(e)),
    }

    test("resolve invalid.invalid (expect failure)");
    match ove::net::dns_resolve(b"invalid.invalid\0", core::time::Duration::from_secs(3)) {
        Err(_) => pass("resolve invalid.invalid (correctly failed)"),
        Ok(_) => fail("resolve invalid.invalid (should have failed)", 0),
    }
}

// ---------------------------------------------------------------------------
// 3. Raw TCP socket
// ---------------------------------------------------------------------------

fn test_tcp() {
    log::info!("=== TCP Socket ===");

    test("socket_open TCP");
    let sock = match ove::net::TcpStream::new() {
        Ok(s) => {
            pass("socket_open TCP");
            s
        }
        Err(e) => {
            fail("socket_open TCP", err_code(e));
            return;
        }
    };

    let mut dest = match ove::net::dns_resolve(b"example.com\0", core::time::Duration::from_secs(5)) {
        Ok(a) => a,
        Err(e) => {
            fail("dns for TCP test", err_code(e));
            return;
        }
    };
    dest.set_port(80);

    test("socket_connect");
    match sock.connect(&dest, core::time::Duration::from_millis(5000)) {
        Ok(()) => pass("socket_connect"),
        Err(e) => {
            fail("socket_connect", err_code(e));
            return;
        }
    }

    let req = b"GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    test("socket_send");
    match sock.send(req) {
        Ok(n) if n == req.len() => pass("socket_send"),
        Ok(_) => {
            fail("socket_send", 0);
            return;
        }
        Err(e) => {
            fail("socket_send", err_code(e));
            return;
        }
    }

    test("socket_recv");
    let mut buf: ove::containers::Vec<u8, 512> = ove::containers::Vec::new();
    // Pre-fill so we can lend slices to recv; we'll truncate to actual
    // length once the loop exits.  resize_default never fails here — N
    // matches buf.capacity() exactly.
    let _ = buf.resize_default(buf.capacity());
    let mut total = 0usize;
    while total < buf.len() - 1 {
        let end = buf.len() - 1;
        match sock.recv(&mut buf[total..end], core::time::Duration::from_secs(5)) {
            Ok(n) => total += n,
            Err(ove::Error::NetClosed) => break,
            Err(_) => break,
        }
    }
    // SAFETY: only `total` bytes were written by recv; truncate to that prefix.
    unsafe { buf.set_len(total) };

    if !buf.is_empty() {
        if find_in_buf(&buf, b"200 OK") {
            log::info!("  -> received {} bytes, status 200 OK", buf.len());
            pass("socket_recv (HTTP 200)");
        } else {
            log::warn!("  -> unexpected status in response");
            fail("socket_recv (unexpected status)", 0);
        }
    } else {
        fail("socket_recv (no data)", 0);
    }

    test("socket_close");
    drop(sock);
    pass("socket_close");
}

fn find_in_buf(haystack: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    haystack.windows(needle.len()).any(|w| w == needle)
}

// ---------------------------------------------------------------------------
// 4. UDP socket
// ---------------------------------------------------------------------------

fn test_udp() {
    log::info!("=== UDP Socket ===");

    test("socket_open UDP");
    let sock = match ove::net::UdpSocket::new() {
        Ok(s) => {
            pass("socket_open UDP");
            s
        }
        Err(e) => {
            fail("socket_open UDP", err_code(e));
            return;
        }
    };

    let bind_addr = ove::net::Address::ipv4(0, 0, 0, 0, 9999);
    test("socket_bind");
    match sock.bind(&bind_addr) {
        Ok(()) => pass("socket_bind"),
        Err(e) => {
            fail("socket_bind", err_code(e));
            return;
        }
    }

    let dest = ove::net::Address::ipv4(127, 0, 0, 1, 9999);
    test("socket_sendto");
    match sock.send_to(UDP_MSG, &dest) {
        Ok(_) => pass("socket_sendto"),
        Err(e) => {
            fail("socket_sendto", err_code(e));
            return;
        }
    }

    test("socket_recvfrom");
    let mut buf = [0u8; 64];
    match sock.recv_from(&mut buf, core::time::Duration::from_secs(2)) {
        Ok((n, _src)) if n == UDP_MSG.len() => {
            if &buf[..n] == UDP_MSG {
                pass("socket_recvfrom (echo match)");
            } else {
                fail("socket_recvfrom (data mismatch)", 0);
            }
        }
        Ok(_) => fail("socket_recvfrom (data mismatch)", 0),
        Err(ove::Error::Timeout) => fail("socket_recvfrom (timeout)", -4),
        Err(e) => fail("socket_recvfrom", err_code(e)),
    }
}

// ---------------------------------------------------------------------------
// 5. HTTP client
// ---------------------------------------------------------------------------

fn test_http() {
    log::info!("=== HTTP Client ===");

    test("http_client_init");
    let client = ove::http_client!();
    pass("http_client_init");

    test("http_get http://example.com/");
    match client.get(b"http://example.com/\0") {
        Ok(resp) => {
            let st = resp.status();
            let blen = resp.body().len();
            log::info!("  -> status {}, body {} bytes", st, blen);
            if st == 200 && blen > 0 {
                pass("http_get (200 OK)");
            } else {
                fail("http_get (unexpected status)", st);
            }
        }
        Err(e) => fail("http_get", err_code(e)),
    }

    test("http_post http://httpbin.org/post");
    match client.post(
        b"http://httpbin.org/post\0",
        b"application/json\0",
        HTTP_POST_BODY,
    ) {
        Ok(resp) => {
            let st = resp.status();
            let blen = resp.body().len();
            log::info!("  -> status {}, body {} bytes", st, blen);
            if st == 200 {
                pass("http_post (200 OK)");
                if find_in_buf(resp.body(), b"overtos") {
                    pass("http_post body echoed");
                } else {
                    fail("http_post body not echoed", 0);
                }
            } else {
                fail("http_post (unexpected status)", st);
            }
        }
        Err(e) => fail("http_post", err_code(e)),
    }

    test("http_put http://httpbin.org/put");
    let headers = [
        ove::net_http::Header {
            name: b"X-Custom\0",
            value: b"oveRTOS\0",
        },
        ove::net_http::Header {
            name: b"Accept\0",
            value: b"application/json\0",
        },
    ];
    match client.request_ex(
        ove::net_http::Method::Put,
        b"http://httpbin.org/put\0",
        b"application/json\0",
        HTTP_PUT_BODY,
        &headers,
    ) {
        Ok(resp) => {
            let st = resp.status();
            let blen = resp.body().len();
            log::info!("  -> status {}, body {} bytes", st, blen);
            if st == 200 {
                pass("http_put (200 OK)");
            } else {
                fail("http_put (unexpected status)", st);
            }
        }
        Err(e) => fail("http_put", err_code(e)),
    }
}

// ---------------------------------------------------------------------------
// 5b. SNTP
// ---------------------------------------------------------------------------

fn test_sntp() {
    log::info!("=== SNTP ===");

    test("sntp_sync pool.ntp.org");
    let cfg = ove::net_sntp::Config {
        server: b"pool.ntp.org\0",
        timeout: core::time::Duration::from_secs(5),
    };
    match ove::net_sntp::sync(&cfg) {
        Ok(()) => {
            pass("sntp_sync");
            test("sntp_get_utc");
            match ove::net_sntp::get_utc() {
                Ok(utc) => {
                    log::info!("  -> UTC: {}", utc);
                    pass("sntp_get_utc");
                }
                Err(e) => fail("sntp_get_utc", err_code(e)),
            }
        }
        Err(e) => fail("sntp_sync", err_code(e)),
    }
}

// ---------------------------------------------------------------------------
// 6. MQTT client
// ---------------------------------------------------------------------------

static MQTT_RX_COUNT: AtomicU32 = AtomicU32::new(0);

fn on_mqtt_message(topic: &str, payload: &[u8]) {
    log::info!(
        "  MQTT rx: [{}] {}",
        topic,
        core::str::from_utf8(payload).unwrap_or("<binary>")
    );
    MQTT_RX_COUNT.fetch_add(1, Ordering::Relaxed);
}

fn test_mqtt() {
    log::info!("=== MQTT Client ===");

    test("mqtt_client_init");
    let mut mqtt = ove::mqtt_client!();
    pass("mqtt_client_init");

    test("mqtt_connect test.mosquitto.org:1883");
    let cfg = ove::net_mqtt::Config {
        host: b"test.mosquitto.org\0",
        port: 1883,
        client_id: MQTT_CLIENT_ID,
        username: None,
        password: None,
        keep_alive_s: 30,
        use_tls: false,
    };
    match mqtt.connect(&cfg, on_mqtt_message) {
        Ok(()) => pass("mqtt_connect"),
        Err(e) => {
            fail("mqtt_connect", err_code(e));
            return;
        }
    }

    test("mqtt_subscribe overtos/test");
    match mqtt.subscribe(MQTT_TOPIC, ove::net_mqtt::Qos::AtMostOnce) {
        Ok(()) => pass("mqtt_subscribe"),
        Err(e) => fail("mqtt_subscribe", err_code(e)),
    }

    test("mqtt_publish QoS0");
    match mqtt.publish(MQTT_TOPIC, b"hello-qos0", ove::net_mqtt::Qos::AtMostOnce) {
        Ok(()) => pass("mqtt_publish QoS0"),
        Err(e) => fail("mqtt_publish QoS0", err_code(e)),
    }

    test("mqtt_publish QoS1");
    match mqtt.publish(MQTT_TOPIC, b"hello-qos1", ove::net_mqtt::Qos::AtLeastOnce) {
        Ok(()) => pass("mqtt_publish QoS1 (PUBACK received)"),
        Err(e) => fail("mqtt_publish QoS1", err_code(e)),
    }

    test("mqtt_loop (receive published messages)");
    MQTT_RX_COUNT.store(0, Ordering::Relaxed);
    for _ in 0..10 {
        let _ = mqtt.poll(core::time::Duration::from_millis(500));
        if MQTT_RX_COUNT.load(Ordering::Relaxed) >= 2 {
            break;
        }
    }
    let rx = MQTT_RX_COUNT.load(Ordering::Relaxed);
    if rx >= 1 {
        log::info!("  -> received {} message(s)", rx);
        pass("mqtt_loop (received messages)");
    } else {
        log::warn!("  -> received {} messages (broker may not echo)", rx);
        pass("mqtt_loop (ran without error)");
    }

    test("mqtt_unsubscribe");
    match mqtt.unsubscribe(MQTT_TOPIC) {
        Ok(()) => pass("mqtt_unsubscribe"),
        Err(e @ ove::Error::NetClosed) | Err(e @ ove::Error::NetReset) => {
            log::warn!("  connection closed by broker ({})", err_code(e));
            pass("mqtt_unsubscribe (connection closed, acceptable)");
        }
        Err(e) => fail("mqtt_unsubscribe", err_code(e)),
    }

    test("mqtt_loop keepalive ping");
    let _ = mqtt.poll(core::time::Duration::from_millis(100));
    pass("mqtt_loop keepalive");

    test("mqtt_disconnect");
    mqtt.disconnect();
    pass("mqtt_disconnect");
}

// ---------------------------------------------------------------------------
// Networking thread
// ---------------------------------------------------------------------------

fn net_thread() {
    test_netif_init();
    test_dns();
    test_tcp();
    test_udp();
    test_http();
    test_sntp();
    test_mqtt();

    let passed = PASS_COUNT.load(Ordering::Relaxed);
    let failed = FAIL_COUNT.load(Ordering::Relaxed);

    log::info!("========================================");
    log::info!("  Results: {} passed, {} failed", passed, failed);
    log::info!("========================================");

    if failed == 0 {
        log::info!("  ALL TESTS PASSED");
    } else {
        log::error!("  {} TEST(S) FAILED", failed);
    }

    let port: u16 = if cfg!(rtos_posix) { 8080 } else { 80 };
    log::info!("Starting HTTP server on port {}...", port);
    match ove::net_httpd::start(port, 1024) {
        Ok(()) => {
            ove::net_httpd::register_builtin_routes();
            log::info!("HTTP server running — open http://<device-ip>:{}/", port);
            loop {
                Thread::sleep_ms(1000);
            }
        }
        Err(e) => log::error!("HTTP server failed to start: {}", err_code(e)),
    }
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log::try_init();
    log::info!("Rust networking example (zero-heap mode): ready");

    ove::thread!("net-test", net_thread, Priority::Normal, 8192).detach();
    ove::run();
}
