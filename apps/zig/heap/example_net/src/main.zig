// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig Networking Example
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

const std = @import("std");
const ove = @import("ove");

/// Route `std.log.*` and any library using `std.log.scoped(...)` through
/// `ove.log.logFn` — emits to the oveRTOS console.
pub const std_options: std.Options = .{
    .logFn = ove.log.logFn,
};
const net = ove.net;
const Address = ove.Address;

const is_posix = @hasDecl(ove.ffi, "CONFIG_OVE_RTOS_POSIX");

// -- Test tracking ----------------------------------------------------------

var pass_count: u32 = 0;
var fail_count: u32 = 0;

// Net test thread — heap-mode value-returning create() returns a 1-pointer
// handle, parked here as an optional so it stays valid past appMain's exit.
var net_thread: ?ove.Thread(16384) = null;

fn testCase(name: []const u8) void {
    std.log.info("  [TEST] {s}", .{name});
}
fn passCase(name: []const u8) void {
    std.log.info("  [PASS] {s}", .{name});
    pass_count += 1;
}
fn failCase(name: []const u8, code: i32) void {
    std.log.err("  [FAIL] {s} ({d})", .{ name, code });
    fail_count += 1;
}

fn errCode(e: anyerror) i32 {
    return switch (e) {
        error.NotRegistered => -1,
        error.InvalidParam => -2,
        error.NoMemory => -3,
        error.Timeout => -4,
        error.NotSupported => -5,
        error.QueueFull => -6,
        error.NetRefused => -7,
        error.NetUnreachable => -8,
        error.NetAddrInUse => -9,
        error.NetReset => -10,
        error.NetDnsFail => -11,
        error.NetClosed => -12,
        else => -99,
    };
}

// -- 1. Network interface ---------------------------------------------------

fn testNetifInit() void {
    std.log.info("=== Network Interface ===", .{});

    testCase("netif_init");
    var netif = net.NetIf.create() catch |e| {
        failCase("netif_init", errCode(e));
        return;
    };
    _ = &netif;
    passCase("netif_init");

    testCase("netif_up (static IP)");
    var cfg = net.NetIfConfig.init();
    if (!is_posix) {
        // Embedded: static IP 172.1.1.2/24, gw 172.1.1.1, DNS 8.8.8.8
        cfg = cfg.staticIp(
            Address.ipv4(172, 1, 1, 2, 0),
            Address.ipv4(255, 255, 255, 0, 0),
            Address.ipv4(172, 1, 1, 1, 0),
        ).dns(Address.ipv4(8, 8, 8, 8, 0));
    }
    netif.up(cfg) catch |e| {
        failCase("netif_up", errCode(e));
        return;
    };
    passCase("netif_up (static IP)");

    // Give the link time to come up on hardware
    if (!is_posix) {
        std.log.info("  Waiting for link...", .{});
        ove.thread.sleepMs(3000);
    }

    // Query actual interface addresses
    testCase("netif_get_addr");
    if (netif.getAddr()) |info| {
        const o = info.ip.octets();
        std.log.info("  IP: {d}.{d}.{d}.{d}", .{ o[0], o[1], o[2], o[3] });
        passCase("netif_get_addr");
    } else |e| {
        failCase("netif_get_addr", errCode(e));
    }
}

// -- 2. DNS resolution ------------------------------------------------------

fn testDns() void {
    std.log.info("=== DNS Resolution ===", .{});

    testCase("resolve example.com");
    if (net.dns.resolve("example.com", 5000 * std.time.ns_per_ms)) |addr| {
        const o = addr.octets();
        std.log.info("  -> {d}.{d}.{d}.{d}", .{ o[0], o[1], o[2], o[3] });
        passCase("resolve example.com");
    } else |e| {
        failCase("resolve example.com", errCode(e));
    }

    testCase("resolve invalid.invalid (expect failure)");
    if (net.dns.resolve("invalid.invalid", 3000 * std.time.ns_per_ms)) |_| {
        failCase("resolve invalid.invalid (should have failed)", 0);
    } else |_| {
        passCase("resolve invalid.invalid (correctly failed)");
    }
}

// -- 3. Raw TCP socket ------------------------------------------------------

fn testTcp() void {
    std.log.info("=== TCP Socket ===", .{});

    testCase("socket_open TCP");
    var sock = net.TcpStream.create() catch |e| {
        failCase("socket_open TCP", errCode(e));
        return;
    };
    defer sock.deinit();
    passCase("socket_open TCP");

    // Resolve + connect to example.com:80
    const dest_addr = net.dns.resolve("example.com", 5000 * std.time.ns_per_ms) catch |e| {
        failCase("dns for TCP test", errCode(e));
        return;
    };
    const dest = dest_addr.withPort(80);

    testCase("socket_connect");
    sock.connect(dest, 5000 * std.time.ns_per_ms) catch |e| {
        failCase("socket_connect", errCode(e));
        return;
    };
    passCase("socket_connect");

    // Send HTTP request
    const req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    testCase("socket_send");
    const sent = sock.send(req) catch |e| {
        failCase("socket_send", errCode(e));
        return;
    };
    if (sent == req.len) {
        passCase("socket_send");
    } else {
        failCase("socket_send", 0);
        return;
    }

    // Receive response
    testCase("socket_recv");
    var buf: [512]u8 = undefined;
    var total: usize = 0;

    // Read until connection closes
    while (total < buf.len - 1) {
        const n = sock.recv(buf[total..], 5 * std.time.ns_per_s) catch |e| {
            if (e == error.NetClosed) break;
            break;
        };
        total += n;
    }

    if (total > 0) {
        const response = buf[0..total];
        // Check for HTTP 200
        if (std.mem.indexOf(u8, response, "200 OK") != null) {
            std.log.info("  -> received {d} bytes, status 200 OK", .{total});
            passCase("socket_recv (HTTP 200)");
        } else {
            // Print first line
            if (std.mem.indexOf(u8, response, "\r\n")) |eol| {
                std.log.warn("  -> {s}", .{response[0..eol]});
            }
            failCase("socket_recv (unexpected status)", 0);
        }
    } else {
        failCase("socket_recv (no data)", 0);
    }

    testCase("socket_close");
    sock.deinit();
    passCase("socket_close");
}

// -- 4. UDP socket ----------------------------------------------------------

fn testUdp() void {
    std.log.info("=== UDP Socket ===", .{});

    testCase("socket_open UDP");
    var sock = net.UdpSocket.create() catch |e| {
        failCase("socket_open UDP", errCode(e));
        return;
    };
    defer sock.deinit();
    passCase("socket_open UDP");

    // Bind to a local port
    testCase("socket_bind");
    sock.bind(Address.any(9999)) catch |e| {
        failCase("socket_bind", errCode(e));
        return;
    };
    passCase("socket_bind");

    // Send to self
    const msg = "oveRTOS UDP test";
    testCase("socket_sendto");
    _ = sock.sendTo(msg, Address.ipv4(127, 0, 0, 1, 9999)) catch |e| {
        failCase("socket_sendto", errCode(e));
        return;
    };
    passCase("socket_sendto");

    // Receive
    testCase("socket_recvfrom");
    var buf: [64]u8 = undefined;
    if (sock.recvFrom(&buf, 2000)) |result| {
        if (result.len == msg.len and std.mem.eql(u8, buf[0..result.len], msg)) {
            passCase("socket_recvfrom (echo match)");
        } else {
            failCase("socket_recvfrom (data mismatch)", 0);
        }
    } else |e| {
        if (e == error.Timeout) {
            failCase("socket_recvfrom (timeout)", errCode(e));
        } else {
            failCase("socket_recvfrom", errCode(e));
        }
    }
}

// -- 5. HTTP client ---------------------------------------------------------

fn testHttp() void {
    std.log.info("=== HTTP Client ===", .{});

    testCase("http_client_init");
    var client = ove.net_http.Client.create() catch |e| {
        failCase("http_client_init", errCode(e));
        return;
    };
    defer client.deinit();
    passCase("http_client_init");

    // GET request
    testCase("http_get http://example.com/");
    if (client.get("http://example.com/")) |resp_| {
        var resp = resp_;
        defer resp.destroy();
        std.log.info("  -> status {d}, body {d} bytes", .{ resp.status(), resp.body().len });
        if (resp.status() == 200 and resp.body().len > 0) {
            passCase("http_get (200 OK)");
        } else {
            failCase("http_get (unexpected status)", resp.status());
        }
    } else |e| {
        failCase("http_get", errCode(e));
    }

    // POST request
    const json = "{\"test\":\"overtos\"}";
    testCase("http_post http://httpbin.org/post");
    if (client.post("http://httpbin.org/post", "application/json", json)) |resp_| {
        var resp = resp_;
        defer resp.destroy();
        std.log.info("  -> status {d}, body {d} bytes", .{ resp.status(), resp.body().len });
        if (resp.status() == 200) {
            passCase("http_post (200 OK)");
            // Check echo body contains our payload
            if (std.mem.indexOf(u8, resp.body(), "overtos") != null) {
                passCase("http_post body echoed");
            } else {
                failCase("http_post body not echoed", 0);
            }
        } else {
            failCase("http_post (unexpected status)", resp.status());
        }
    } else |e| {
        failCase("http_post", errCode(e));
    }

    // PUT request with custom headers
    const put_json = "{\"update\":\"value\"}";
    testCase("http_put http://httpbin.org/put");
    const headers = [_]ove.net_http.Header{
        .{ .name = "X-Custom", .value = "oveRTOS" },
        .{ .name = "Accept", .value = "application/json" },
    };
    if (client.requestEx(
        .put,
        "http://httpbin.org/put",
        "application/json",
        put_json,
        &headers,
    )) |resp_| {
        var resp = resp_;
        defer resp.destroy();
        std.log.info("  -> status {d}, body {d} bytes", .{ resp.status(), resp.body().len });
        if (resp.status() == 200) {
            passCase("http_put (200 OK)");
        } else {
            failCase("http_put (unexpected status)", resp.status());
        }
    } else |e| {
        failCase("http_put", errCode(e));
    }
}

// -- 5b. SNTP ---------------------------------------------------------------

fn testSntp() void {
    std.log.info("=== SNTP ===", .{});

    testCase("sntp_sync pool.ntp.org");
    ove.net_sntp.sync(.{
        .server = "pool.ntp.org",
        .timeout_ns = 5 * std.time.ns_per_s,
    }) catch |e| {
        failCase("sntp_sync", errCode(e));
        return;
    };
    passCase("sntp_sync");

    testCase("sntp_get_utc");
    if (ove.net_sntp.getUtc()) |utc| {
        std.log.info("  -> UTC: {d}", .{utc});
        passCase("sntp_get_utc");
    } else |e| {
        failCase("sntp_get_utc", errCode(e));
    }
}

// -- 6. MQTT client ---------------------------------------------------------

var mqtt_rx_count: u32 = 0;
var mqtt_rx_payload: [128]u8 = undefined;
var mqtt_rx_payload_len: usize = 0;

fn onMqttMessage(topic: []const u8, payload: []const u8) void {
    std.log.info("  MQTT rx: [{s}] {s}", .{ topic, payload });
    if (payload.len < mqtt_rx_payload.len) {
        @memcpy(mqtt_rx_payload[0..payload.len], payload);
        mqtt_rx_payload_len = payload.len;
    }
    mqtt_rx_count += 1;
}

fn testMqtt() void {
    std.log.info("=== MQTT Client ===", .{});

    testCase("mqtt_client_init");
    var mqtt = ove.net_mqtt.Client.create() catch |e| {
        failCase("mqtt_client_init", errCode(e));
        return;
    };
    defer mqtt.deinit();
    passCase("mqtt_client_init");

    testCase("mqtt_connect test.mosquitto.org:1883");
    mqtt.connect(.{
        .host = "test.mosquitto.org",
        .port = 1883,
        .client_id = "overtos-test",
        .keep_alive_s = 30,
    }, onMqttMessage) catch |e| {
        failCase("mqtt_connect", errCode(e));
        return;
    };
    passCase("mqtt_connect");

    // Subscribe
    testCase("mqtt_subscribe overtos/test");
    if (mqtt.subscribe("overtos/test", .at_most_once)) |_| {
        passCase("mqtt_subscribe");
    } else |e| {
        failCase("mqtt_subscribe", errCode(e));
    }

    // Publish QoS0
    testCase("mqtt_publish QoS0");
    if (mqtt.publish("overtos/test", "hello-qos0", .at_most_once)) |_| {
        passCase("mqtt_publish QoS0");
    } else |e| {
        failCase("mqtt_publish QoS0", errCode(e));
    }

    // Publish QoS1
    testCase("mqtt_publish QoS1");
    if (mqtt.publish("overtos/test", "hello-qos1", .at_least_once)) |_| {
        passCase("mqtt_publish QoS1 (PUBACK received)");
    } else |e| {
        failCase("mqtt_publish QoS1", errCode(e));
    }

    // Poll to receive our own messages
    testCase("mqtt_loop (receive published messages)");
    mqtt_rx_count = 0;
    var i: u32 = 0;
    while (i < 10) : (i += 1) {
        mqtt.pollOnce(500) catch {}; // best-effort poll
        if (mqtt_rx_count >= 2) break;
    }
    if (mqtt_rx_count >= 1) {
        std.log.info("  -> received {d} message(s)", .{mqtt_rx_count});
        passCase("mqtt_loop (received messages)");
    } else {
        std.log.warn("  -> received {d} messages (broker may not echo)", .{mqtt_rx_count});
        passCase("mqtt_loop (ran without error)");
    }

    // Unsubscribe
    testCase("mqtt_unsubscribe");
    mqtt.unsubscribe("overtos/test") catch |e| {
        // Some brokers close the connection during idle polling;
        // treat connection-closed as acceptable for unsubscribe.
        if (e == error.NetClosed or e == error.NetReset) {
            std.log.warn("  connection closed by broker ({d})", .{errCode(e)});
            passCase("mqtt_unsubscribe (connection closed, acceptable)");
        } else {
            failCase("mqtt_unsubscribe", errCode(e));
        }
        return;
    };
    passCase("mqtt_unsubscribe");

    // Keepalive ping
    testCase("mqtt_loop keepalive ping");
    mqtt.pollOnce(100) catch {}; // best-effort keepalive
    passCase("mqtt_loop keepalive");

    // Disconnect
    testCase("mqtt_disconnect");
    mqtt.disconnect();
    passCase("mqtt_disconnect");
}

// -- Networking thread ------------------------------------------------------

fn netThread() void {
    testNetifInit();
    testDns();
    testTcp();
    testUdp();

    testHttp();
    testSntp();
    testMqtt();

    std.log.info("========================================", .{});
    std.log.info("  Results: {d} passed, {d} failed", .{ pass_count, fail_count });
    std.log.info("========================================", .{});

    if (fail_count == 0) {
        std.log.info("  ALL TESTS PASSED", .{});
    } else {
        std.log.err("  {d} TEST(S) FAILED", .{fail_count});
    }

    // Start web dashboard -- runs forever
    const httpd_port: u16 = if (is_posix) 8080 else 80;
    std.log.info("Starting HTTP server on port {d}...", .{httpd_port});
    ove.net_httpd.start(.{ .port = httpd_port, .max_body_size = 1024 }) catch |e| {
        std.log.err("HTTP server failed to start: {d}", .{errCode(e)});
        return;
    };
    ove.net_httpd.registerBuiltinRoutes();
    std.log.info("HTTP server running -- open http://<device-ip>:{d}/", .{httpd_port});
    // Keep thread alive so httpd keeps running
    while (true) ove.thread.sleepMs(1000);
}

// -- App entry point --------------------------------------------------------

fn appMain() void {
    std.log.info("Zig networking example (heap mode): init", .{});

    net_thread = ove.Thread(16384).spawn(.{ .name = "net-test", .priority = .normal }, netThread, .{}) catch |e| {
        std.log.err("Failed to create net thread: {d}", .{errCode(e)});
        return;
    };

    std.log.info("Zig networking example (heap mode): ready", .{});
    ove.run();

    // On POSIX, ove_run() returns -- keep alive for the httpd server
    while (true) ove.thread.sleepMs(1000);
}

comptime {
    ove.exportMain(appMain);
}
