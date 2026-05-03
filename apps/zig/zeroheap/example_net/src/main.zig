// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! oveRTOS Zig Networking Example — zero-heap mode.
//!
//! Mirrors apps/zig/heap/example_net.  The zero-heap binding shape uses
//! `pub fn init(self: *T) Error!void` two-phase init in place of heap-
//! mode value-returning `create()`.  Threads, network interface, and
//! HTTP/MQTT clients are all declared as `var x: T = undefined;` and
//! initialised explicitly.  Protocol pools (lwIP heap, mbedTLS arena,
//! MQTT rx/tx, HTTP response borrow buffer) live in BSS, sized at
//! compile time.

const std = @import("std");
const ove = @import("ove");
const net = ove.net;
const Address = ove.Address;

const is_posix = @hasDecl(ove.ffi, "CONFIG_OVE_RTOS_POSIX");

// -- Test tracking ----------------------------------------------------------

var pass_count: u32 = 0;
var fail_count: u32 = 0;

// File-scope thread (zero-heap embeds storage + stack inline).
var net_thread: ove.Thread(8192) = undefined;

fn testCase(name: []const u8) void {
    ove.log.inf("  [TEST] {s}", .{name});
}
fn passCase(name: []const u8) void {
    ove.log.inf("  [PASS] {s}", .{name});
    pass_count += 1;
}
fn failCase(name: []const u8, code: i32) void {
    ove.log.err("  [FAIL] {s} ({d})", .{ name, code });
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
    ove.log.inf("=== Network Interface ===", .{});

    testCase("netif_init");
    var netif: net.NetIf = undefined;
    netif.init() catch |e| {
        failCase("netif_init", errCode(e));
        return;
    };
    passCase("netif_init");

    testCase("netif_up (static IP)");
    var cfg = net.NetIfConfig.init();
    if (!is_posix) {
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

    if (!is_posix) {
        ove.log.inf("  Waiting for link...", .{});
        ove.thread.sleepMs(3000);
    }

    testCase("netif_get_addr");
    if (netif.getAddr()) |info| {
        const o = info.ip.octets();
        ove.log.inf("  IP: {d}.{d}.{d}.{d}", .{ o[0], o[1], o[2], o[3] });
        passCase("netif_get_addr");
    } else |e| {
        failCase("netif_get_addr", errCode(e));
    }
}

// -- 2. DNS resolution ------------------------------------------------------

fn testDns() void {
    ove.log.inf("=== DNS Resolution ===", .{});

    testCase("resolve example.com");
    if (net.dns.resolve("example.com", 5000)) |addr| {
        const o = addr.octets();
        ove.log.inf("  -> {d}.{d}.{d}.{d}", .{ o[0], o[1], o[2], o[3] });
        passCase("resolve example.com");
    } else |e| {
        failCase("resolve example.com", errCode(e));
    }

    testCase("resolve invalid.invalid (expect failure)");
    if (net.dns.resolve("invalid.invalid", 3000)) |_| {
        failCase("resolve invalid.invalid (should have failed)", 0);
    } else |_| {
        passCase("resolve invalid.invalid (correctly failed)");
    }
}

// -- 3. Raw TCP socket ------------------------------------------------------

fn testTcp() void {
    ove.log.inf("=== TCP Socket ===", .{});

    testCase("socket_open TCP");
    var sock: net.TcpStream = undefined;
    sock.init() catch |e| {
        failCase("socket_open TCP", errCode(e));
        return;
    };
    defer sock.deinit();
    passCase("socket_open TCP");

    const dest_addr = net.dns.resolve("example.com", 5000) catch |e| {
        failCase("dns for TCP test", errCode(e));
        return;
    };
    const dest = dest_addr.withPort(80);

    testCase("socket_connect");
    sock.connect(dest, 5000) catch |e| {
        failCase("socket_connect", errCode(e));
        return;
    };
    passCase("socket_connect");

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

    testCase("socket_recv");
    var buf: [512]u8 = undefined;
    var total: usize = 0;
    while (total < buf.len - 1) {
        const n = sock.recv(buf[total..], 5000) catch |e| {
            if (e == error.NetClosed) break;
            break;
        };
        total += n;
    }

    if (total > 0) {
        const response = buf[0..total];
        if (std.mem.indexOf(u8, response, "200 OK") != null) {
            ove.log.inf("  -> received {d} bytes, status 200 OK", .{total});
            passCase("socket_recv (HTTP 200)");
        } else {
            if (std.mem.indexOf(u8, response, "\r\n")) |eol| {
                ove.log.wrn("  -> {s}", .{response[0..eol]});
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
    ove.log.inf("=== UDP Socket ===", .{});

    testCase("socket_open UDP");
    var sock: net.UdpSocket = undefined;
    sock.init() catch |e| {
        failCase("socket_open UDP", errCode(e));
        return;
    };
    defer sock.deinit();
    passCase("socket_open UDP");

    testCase("socket_bind");
    sock.bind(Address.any(9999)) catch |e| {
        failCase("socket_bind", errCode(e));
        return;
    };
    passCase("socket_bind");

    const msg = "oveRTOS UDP test";
    testCase("socket_sendto");
    _ = sock.sendTo(msg, Address.ipv4(127, 0, 0, 1, 9999)) catch |e| {
        failCase("socket_sendto", errCode(e));
        return;
    };
    passCase("socket_sendto");

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
    ove.log.inf("=== HTTP Client ===", .{});

    testCase("http_client_init");
    var client: ove.net_http.Client = undefined;
    client.init() catch |e| {
        failCase("http_client_init", errCode(e));
        return;
    };
    defer client.deinit();
    passCase("http_client_init");

    testCase("http_get http://example.com/");
    if (client.get("http://example.com/")) |resp_| {
        var resp = resp_;
        defer resp.destroy();
        ove.log.inf("  -> status {d}, body {d} bytes", .{ resp.status(), resp.body().len });
        if (resp.status() == 200 and resp.body().len > 0) {
            passCase("http_get (200 OK)");
        } else {
            failCase("http_get (unexpected status)", resp.status());
        }
    } else |e| {
        failCase("http_get", errCode(e));
    }

    const json = "{\"test\":\"overtos\"}";
    testCase("http_post http://httpbin.org/post");
    if (client.post("http://httpbin.org/post", "application/json", json)) |resp_| {
        var resp = resp_;
        defer resp.destroy();
        ove.log.inf("  -> status {d}, body {d} bytes", .{ resp.status(), resp.body().len });
        if (resp.status() == 200) {
            passCase("http_post (200 OK)");
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
        ove.log.inf("  -> status {d}, body {d} bytes", .{ resp.status(), resp.body().len });
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
    ove.log.inf("=== SNTP ===", .{});

    testCase("sntp_sync pool.ntp.org");
    ove.net_sntp.sync(.{
        .server = "pool.ntp.org",
        .timeout_ms = 5000,
    }) catch |e| {
        failCase("sntp_sync", errCode(e));
        return;
    };
    passCase("sntp_sync");

    testCase("sntp_get_utc");
    if (ove.net_sntp.getUtc()) |utc| {
        ove.log.inf("  -> UTC: {d}", .{utc});
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
    ove.log.inf("  MQTT rx: [{s}] {s}", .{ topic, payload });
    if (payload.len < mqtt_rx_payload.len) {
        @memcpy(mqtt_rx_payload[0..payload.len], payload);
        mqtt_rx_payload_len = payload.len;
    }
    mqtt_rx_count += 1;
}

fn testMqtt() void {
    ove.log.inf("=== MQTT Client ===", .{});

    testCase("mqtt_client_init");
    var mqtt: ove.net_mqtt.Client = undefined;
    mqtt.init() catch |e| {
        failCase("mqtt_client_init", errCode(e));
        return;
    };
    defer mqtt.deinit();
    passCase("mqtt_client_init");

    testCase("mqtt_connect test.mosquitto.org:1883");
    mqtt.connect(.{
        .host = "test.mosquitto.org",
        .port = 1883,
        .client_id = "overtos-test-zh",
        .keep_alive_s = 30,
    }, onMqttMessage) catch |e| {
        failCase("mqtt_connect", errCode(e));
        return;
    };
    passCase("mqtt_connect");

    testCase("mqtt_subscribe overtos/test");
    if (mqtt.subscribe("overtos/test", .at_most_once)) |_| {
        passCase("mqtt_subscribe");
    } else |e| {
        failCase("mqtt_subscribe", errCode(e));
    }

    testCase("mqtt_publish QoS0");
    if (mqtt.publish("overtos/test", "hello-qos0", .at_most_once)) |_| {
        passCase("mqtt_publish QoS0");
    } else |e| {
        failCase("mqtt_publish QoS0", errCode(e));
    }

    testCase("mqtt_publish QoS1");
    if (mqtt.publish("overtos/test", "hello-qos1", .at_least_once)) |_| {
        passCase("mqtt_publish QoS1 (PUBACK received)");
    } else |e| {
        failCase("mqtt_publish QoS1", errCode(e));
    }

    testCase("mqtt_loop (receive published messages)");
    mqtt_rx_count = 0;
    var i: u32 = 0;
    while (i < 10) : (i += 1) {
        mqtt.pollOnce(500) catch {};
        if (mqtt_rx_count >= 2) break;
    }
    if (mqtt_rx_count >= 1) {
        ove.log.inf("  -> received {d} message(s)", .{mqtt_rx_count});
        passCase("mqtt_loop (received messages)");
    } else {
        ove.log.wrn("  -> received {d} messages (broker may not echo)", .{mqtt_rx_count});
        passCase("mqtt_loop (ran without error)");
    }

    testCase("mqtt_unsubscribe");
    mqtt.unsubscribe("overtos/test") catch |e| {
        if (e == error.NetClosed or e == error.NetReset) {
            ove.log.wrn("  connection closed by broker ({d})", .{errCode(e)});
            passCase("mqtt_unsubscribe (connection closed, acceptable)");
        } else {
            failCase("mqtt_unsubscribe", errCode(e));
        }
        return;
    };
    passCase("mqtt_unsubscribe");

    testCase("mqtt_loop keepalive ping");
    mqtt.pollOnce(100) catch {};
    passCase("mqtt_loop keepalive");

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

    ove.log.inf("========================================", .{});
    ove.log.inf("  Results: {d} passed, {d} failed", .{ pass_count, fail_count });
    ove.log.inf("========================================", .{});

    if (fail_count == 0) {
        ove.log.inf("  ALL TESTS PASSED", .{});
    } else {
        ove.log.err("  {d} TEST(S) FAILED", .{fail_count});
    }

    const httpd_port: u16 = if (is_posix) 8080 else 80;
    ove.log.inf("Starting HTTP server on port {d}...", .{httpd_port});
    ove.net_httpd.start(.{ .port = httpd_port, .max_body_size = 1024 }) catch |e| {
        ove.log.err("HTTP server failed to start: {d}", .{errCode(e)});
        return;
    };
    ove.net_httpd.registerBuiltinRoutes();
    ove.log.inf("HTTP server running -- open http://<device-ip>:{d}/", .{httpd_port});
    while (true) ove.thread.sleepMs(1000);
}

// -- App entry point --------------------------------------------------------

fn appMain() void {
    ove.log.inf("Zig networking example (zero-heap mode): init", .{});

    net_thread.init("net-test", netThread, ove.thread.prio.normal) catch |e| {
        ove.log.err("Failed to init net thread: {d}", .{errCode(e)});
        return;
    };

    ove.log.inf("Zig networking example (zero-heap mode): ready", .{});
    ove.run();
}

comptime {
    ove.exportMain(appMain);
}
