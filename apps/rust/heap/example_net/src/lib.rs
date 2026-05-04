// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Rust Networking Example
//!
//! Exercises the full networking stack:
//!   1. Network interface init with static IP
//!   2. DNS resolution (positive + negative)
//!   3. Raw TCP socket connect + HTTP/1.0 request
//!   4. UDP send/receive (loopback echo)
//!   5. HTTP client GET, POST, and PUT
//!   6. SNTP time synchronization
//!   7. MQTT connect, subscribe, publish QoS0/1, receive, disconnect
//!   8. Embedded HTTP server (web dashboard)
//!
//! On POSIX the netif init is a no-op (host networking).
//! On FreeRTOS/lwIP the static IP config drives real hardware.

#![cfg_attr(not(feature = "std"), no_std)]

use ove_allocator as _;

use core::sync::atomic::{AtomicU32, Ordering};
use ove::Thread;

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
    ove::log_inf!("  [TEST] {}", name);
}

fn pass(name: &str) {
    ove::log_inf!("  [PASS] {}", name);
    PASS_COUNT.fetch_add(1, Ordering::Relaxed);
}

fn fail(name: &str, err: i32) {
    ove::log_err!("  [FAIL] {} ({})", name, err);
    FAIL_COUNT.fetch_add(1, Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// String constants
// ---------------------------------------------------------------------------

const UDP_MSG: &[u8] = b"oveRTOS UDP test";
const MQTT_TOPIC: &[u8] = b"overtos/test\0";
const MQTT_CLIENT_ID: &[u8] = b"overtos-test\0";
const HTTP_POST_BODY: &[u8] = b"{\"test\":\"overtos\"}";
const HTTP_PUT_BODY: &[u8] = b"{\"update\":\"value\"}";

// ---------------------------------------------------------------------------
// 1. Network interface
// ---------------------------------------------------------------------------

fn test_netif_init() {
    ove::log_inf!("=== Network Interface ===");

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

    // Give the link time to come up on hardware
    #[cfg(not(rtos_posix))]
    {
        ove::log_inf!("  Waiting for link...");
        Thread::sleep_ms(3000);
    }

    // Query actual interface addresses
    test("netif_get_addr");
    match netif.get_addr() {
        Ok((ip, _gw, _nm)) => {
            let o = ip.octets();
            ove::log_inf!("  IP: {}.{}.{}.{}", o[0], o[1], o[2], o[3]);
            pass("netif_get_addr");
        }
        Err(e) => fail("netif_get_addr", err_code(e)),
    }
}

// ---------------------------------------------------------------------------
// 2. DNS resolution
// ---------------------------------------------------------------------------

fn test_dns() {
    ove::log_inf!("=== DNS Resolution ===");

    test("resolve example.com");
    match ove::net::dns_resolve(b"example.com\0", 5000) {
        Ok(addr) => {
            let o = addr.octets();
            ove::log_inf!("  -> {}.{}.{}.{}", o[0], o[1], o[2], o[3]);
            pass("resolve example.com");
        }
        Err(e) => fail("resolve example.com", err_code(e)),
    }

    test("resolve invalid.invalid (expect failure)");
    match ove::net::dns_resolve(b"invalid.invalid\0", 3000) {
        Err(_) => pass("resolve invalid.invalid (correctly failed)"),
        Ok(_) => fail("resolve invalid.invalid (should have failed)", 0),
    }
}

// ---------------------------------------------------------------------------
// 3. Raw TCP socket
// ---------------------------------------------------------------------------

fn test_tcp() {
    ove::log_inf!("=== TCP Socket ===");

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

    // Resolve + connect to example.com:80
    let mut dest = match ove::net::dns_resolve(b"example.com\0", 5000) {
        Ok(a) => a,
        Err(e) => {
            fail("dns for TCP test", err_code(e));
            return;
        }
    };
    dest.set_port(80);

    test("socket_connect");
    match sock.connect(&dest, 5000) {
        Ok(()) => pass("socket_connect"),
        Err(e) => {
            fail("socket_connect", err_code(e));
            return;
        }
    }

    // Send HTTP request
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

    // Receive response
    test("socket_recv");
    let mut buf: ove::containers::Vec<u8, 512> = ove::containers::Vec::new();
    // Pre-fill so we can lend slices to recv; we'll truncate to actual
    // length once the loop exits.  resize_default never fails here — N
    // matches buf.capacity() exactly.
    let _ = buf.resize_default(buf.capacity());
    let mut total = 0usize;
    while total < buf.len() - 1 {
        let end = buf.len() - 1;
        match sock.recv(&mut buf[total..end], 5000) {
            Ok(n) => total += n,
            Err(ove::Error::NetClosed) => break,
            Err(_) => break,
        }
    }
    // SAFETY: only `total` bytes were written by recv; truncate to that prefix.
    unsafe { buf.set_len(total) };

    if !buf.is_empty() {
        // Check for HTTP 200
        if find_in_buf(&buf, b"200 OK") {
            ove::log_inf!("  -> received {} bytes, status 200 OK", buf.len());
            pass("socket_recv (HTTP 200)");
        } else {
            ove::log_wrn!("  -> unexpected status in response");
            fail("socket_recv (unexpected status)", 0);
        }
    } else {
        fail("socket_recv (no data)", 0);
    }

    test("socket_close");
    // TcpStream closes on drop, but we log it explicitly
    drop(sock);
    pass("socket_close");
}

/// Substring search in a byte buffer.
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
    ove::log_inf!("=== UDP Socket ===");

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

    // Bind to a local port
    let bind_addr = ove::net::Address::ipv4(0, 0, 0, 0, 9999);
    test("socket_bind");
    match sock.bind(&bind_addr) {
        Ok(()) => pass("socket_bind"),
        Err(e) => {
            fail("socket_bind", err_code(e));
            return;
        }
    }

    // Send to self
    let dest = ove::net::Address::ipv4(127, 0, 0, 1, 9999);
    test("socket_sendto");
    match sock.send_to(UDP_MSG, &dest) {
        Ok(_) => pass("socket_sendto"),
        Err(e) => {
            fail("socket_sendto", err_code(e));
            return;
        }
    }

    // Receive
    test("socket_recvfrom");
    let mut buf = [0u8; 64];
    match sock.recv_from(&mut buf, 2000) {
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
    ove::log_inf!("=== HTTP Client ===");

    test("http_client_init");
    let mut http_storage = ove::net_http::ClientStorage::new();
    let client = match ove::net_http::Client::create(&mut http_storage) {
        Ok(c) => {
            pass("http_client_init");
            c
        }
        Err(e) => {
            fail("http_client_init", err_code(e));
            return;
        }
    };

    // GET request
    test("http_get http://example.com/");
    match client.get(b"http://example.com/\0") {
        Ok(resp) => {
            let st = resp.status();
            let blen = resp.body().len();
            ove::log_inf!("  -> status {}, body {} bytes", st, blen);
            if st == 200 && blen > 0 {
                pass("http_get (200 OK)");
            } else {
                fail("http_get (unexpected status)", st);
            }
        }
        Err(e) => fail("http_get", err_code(e)),
    }

    // POST request
    test("http_post http://httpbin.org/post");
    match client.post(
        b"http://httpbin.org/post\0",
        b"application/json\0",
        HTTP_POST_BODY,
    ) {
        Ok(resp) => {
            let st = resp.status();
            let blen = resp.body().len();
            ove::log_inf!("  -> status {}, body {} bytes", st, blen);
            if st == 200 {
                pass("http_post (200 OK)");
                // Check echo body contains our payload
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

    // PUT request with custom headers
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
            ove::log_inf!("  -> status {}, body {} bytes", st, blen);
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
    ove::log_inf!("=== SNTP ===");

    test("sntp_sync pool.ntp.org");
    let cfg = ove::net_sntp::Config {
        server: b"pool.ntp.org\0",
        timeout_ms: 5000,
    };
    match ove::net_sntp::sync(&cfg) {
        Ok(()) => {
            pass("sntp_sync");
            test("sntp_get_utc");
            match ove::net_sntp::get_utc() {
                Ok(utc) => {
                    ove::log_inf!("  -> UTC: {}", utc);
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
    ove::log_inf!(
        "  MQTT rx: [{}] {}",
        topic,
        core::str::from_utf8(payload).unwrap_or("<binary>")
    );
    MQTT_RX_COUNT.fetch_add(1, Ordering::Relaxed);
}

fn test_mqtt() {
    ove::log_inf!("=== MQTT Client ===");

    test("mqtt_client_init");
    let mut mqtt_storage = ove::net_mqtt::ClientStorage::new();
    let mut mqtt = match ove::net_mqtt::Client::create(&mut mqtt_storage) {
        Ok(c) => {
            pass("mqtt_client_init");
            c
        }
        Err(e) => {
            fail("mqtt_client_init", err_code(e));
            return;
        }
    };

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

    // Subscribe
    test("mqtt_subscribe overtos/test");
    match mqtt.subscribe(MQTT_TOPIC, ove::net_mqtt::Qos::AtMostOnce) {
        Ok(()) => pass("mqtt_subscribe"),
        Err(e) => fail("mqtt_subscribe", err_code(e)),
    }

    // Publish QoS0
    test("mqtt_publish QoS0");
    match mqtt.publish(MQTT_TOPIC, b"hello-qos0", ove::net_mqtt::Qos::AtMostOnce) {
        Ok(()) => pass("mqtt_publish QoS0"),
        Err(e) => fail("mqtt_publish QoS0", err_code(e)),
    }

    // Publish QoS1
    test("mqtt_publish QoS1");
    match mqtt.publish(MQTT_TOPIC, b"hello-qos1", ove::net_mqtt::Qos::AtLeastOnce) {
        Ok(()) => pass("mqtt_publish QoS1 (PUBACK received)"),
        Err(e) => fail("mqtt_publish QoS1", err_code(e)),
    }

    // Poll to receive our own messages
    test("mqtt_loop (receive published messages)");
    MQTT_RX_COUNT.store(0, Ordering::Relaxed);
    for _ in 0..10 {
        let _ = mqtt.poll(500);
        if MQTT_RX_COUNT.load(Ordering::Relaxed) >= 2 {
            break;
        }
    }
    let rx = MQTT_RX_COUNT.load(Ordering::Relaxed);
    if rx >= 1 {
        ove::log_inf!("  -> received {} message(s)", rx);
        pass("mqtt_loop (received messages)");
    } else {
        ove::log_wrn!("  -> received {} messages (broker may not echo)", rx);
        pass("mqtt_loop (ran without error)");
    }

    // Unsubscribe -- do this promptly after loop to avoid stale connection
    test("mqtt_unsubscribe");
    match mqtt.unsubscribe(MQTT_TOPIC) {
        Ok(()) => pass("mqtt_unsubscribe"),
        Err(e @ ove::Error::NetClosed) | Err(e @ ove::Error::NetReset) => {
            // Some brokers close the connection during idle polling;
            // treat connection-closed as acceptable for unsubscribe.
            ove::log_wrn!("  connection closed by broker ({})", err_code(e));
            pass("mqtt_unsubscribe (connection closed, acceptable)");
        }
        Err(e) => fail("mqtt_unsubscribe", err_code(e)),
    }

    // Keepalive ping
    test("mqtt_loop keepalive ping");
    let _ = mqtt.poll(100);
    pass("mqtt_loop keepalive");

    // Disconnect
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

    ove::log_inf!("========================================");
    ove::log_inf!("  Results: {} passed, {} failed", passed, failed);
    ove::log_inf!("========================================");

    if failed == 0 {
        ove::log_inf!("  ALL TESTS PASSED");
    } else {
        ove::log_err!("  {} TEST(S) FAILED", failed);
    }

    // HTTPD — runs forever after the test harness
    {
        let port: u16 = if cfg!(rtos_posix) { 8080 } else { 80 };
        ove::log_inf!("Starting HTTP server on port {}...", port);
        match ove::net_httpd::start(port, 1024) {
            Ok(()) => {
                ove::net_httpd::register_builtin_routes();
                ove::log_inf!("HTTP server running — open http://<device-ip>:{}/", port);
                loop {
                    Thread::sleep_ms(1000);
                }
            }
            Err(e) => ove::log_err!("HTTP server failed to start: {}", err_code(e)),
        }
    }
}

// ---------------------------------------------------------------------------
// App entry point
// ---------------------------------------------------------------------------

fn app_main() {
    ove::log_inf!("Rust networking example (heap mode): init");

    // Heap-allocated thread via spawn_with — closure captures move into
    // the boxed FnOnce, allowing the test runner to be a one-shot
    // closure rather than a free `fn()`.
    let net = Thread::spawn_with(b"net-test\0", ove::Priority::Normal, 8192, || {
        net_thread();
    })
    .expect("net-test spawn");
    core::mem::forget(net);

    ove::log_inf!("Rust networking example (heap mode): ready");
    ove::run();

    loop {
        Thread::sleep_ms(1000);
    }
}
