/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Built-in REST API route handlers for the oveRTOS HTTP server dashboard.
 *
 * Provides device info, LED control, GPIO read/write, network status,
 * and a scrollable log ring buffer — all served as JSON from lightweight
 * stack-allocated buffers.
 */

#include "ove/ove.h"
#include "ove/net_httpd.h"
#include "ove/net.h"
#include "ove/thread.h"

#ifdef CONFIG_OVE_AUDIO
#include "ove/audio.h"
#endif
#ifdef CONFIG_OVE_INFER
#include "ove/infer.h"
#endif

#ifdef CONFIG_OVE_SYNC
#include "ove/sync.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef CONFIG_OVE_NET_HTTPD

/* ── Embedded HTML dashboard (gzip-compressed) ────────────────── */

#include "ove_net_httpd_ui_gz.h"

/* ── Network interface for address queries ────────────────────── */

static ove_netif_t s_httpd_netif;

void ove_httpd_set_netif(ove_netif_t netif)
{
	s_httpd_netif = netif;
}

static void fmt_ip(char *buf, size_t len, const ove_sockaddr_t *addr)
{
	snprintf(buf, len, "%u.%u.%u.%u", addr->addr[0], addr->addr[1], addr->addr[2],
		 addr->addr[3]);
}

static int query_netif_addr(ove_sockaddr_t *ip, ove_sockaddr_t *gw, ove_sockaddr_t *nm)
{
	if (s_httpd_netif)
		return ove_netif_get_addr(s_httpd_netif, ip, gw, nm);

	/* Fallback: try with a temporary netif (works on POSIX). */
	ove_netif_storage_t tmp_storage;
	ove_netif_t tmp;
	if (ove_netif_init(&tmp, &tmp_storage) == OVE_OK)
		return ove_netif_get_addr(tmp, ip, gw, nm);

	return OVE_ERR_NOT_SUPPORTED;
}

/* ── Log ring buffer (mutex-protected) ────────────────────────── */

#define LOG_RING_LINES 32
#define LOG_LINE_MAX 128

static char log_ring[LOG_RING_LINES][LOG_LINE_MAX];
static int log_ring_head;

#ifdef CONFIG_OVE_SYNC
static ove_mutex_t s_log_mutex;
static ove_mutex_storage_t s_log_mutex_storage;
static int s_log_mutex_inited;

static void log_lock(void)
{
	if (s_log_mutex_inited)
		ove_mutex_lock(s_log_mutex, OVE_WAIT_FOREVER);
}

static void log_unlock(void)
{
	if (s_log_mutex_inited)
		ove_mutex_unlock(s_log_mutex);
}
#else
static void log_lock(void)
{
}
static void log_unlock(void)
{
}
#endif

void ove_httpd_log_append(const char *line)
{
	log_lock();
	strncpy(log_ring[log_ring_head], line, LOG_LINE_MAX - 1);
	log_ring[log_ring_head][LOG_LINE_MAX - 1] = '\0';
	log_ring_head = (log_ring_head + 1) % LOG_RING_LINES;
	log_unlock();

#ifdef CONFIG_OVE_NET_HTTPD_WS
	ove_httpd_ws_broadcast("/ws/log", line, strlen(line));
#endif
}

/* ── LED state tracking ────────────────────────────────────────── */

/*
 * The LED API does not expose a getter, so we shadow the on/off state
 * here.  The array is zero-initialised (all LEDs off at boot).
 */
#define LED_MAX 8
static int led_state[LED_MAX];

/* ── Route: GET / ──────────────────────────────────────────────── */

static int handle_index(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;
	return ove_httpd_resp_send_gz(resp, 200, "text/html", httpd_dashboard_gz,
				      httpd_dashboard_gz_len);
}

/* ── Route: GET /api/info ──────────────────────────────────────── */

static int handle_info(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;

	uint64_t us = 0;
	ove_time_get_us(&us);
	unsigned long uptime_s = (unsigned long)(us / 1000000ULL);

	char ip_str[16] = "0.0.0.0";
	ove_sockaddr_t ip_addr;
	if (query_netif_addr(&ip_addr, NULL, NULL) == OVE_OK)
		fmt_ip(ip_str, sizeof(ip_str), &ip_addr);

	char buf[256];
	snprintf(buf, sizeof(buf),
		 "{\"board\":\"%s\",\"rtos\":\"%s\","
		 "\"uptime\":%lu,\"ip\":\"%s\","
		 "\"version\":\"0.0.0\"}",
		 ove_board_name(), OVE_RTOS_NAME, uptime_s, ip_str);

	return ove_httpd_resp_json(resp, 200, buf);
}

/* ── Route: GET /api/leds ──────────────────────────────────────── */

static int handle_leds_get(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;

#ifdef CONFIG_OVE_LED
	unsigned int count = ove_led_count();
	char buf[512];
	int off = 0;

	off += snprintf(buf + off, sizeof(buf) - (size_t)off, "{\"leds\":[");
	for (unsigned int i = 0; i < count && i < LED_MAX; i++) {
		if (i > 0)
			off += snprintf(buf + off, sizeof(buf) - (size_t)off, ",");
		off += snprintf(buf + off, sizeof(buf) - (size_t)off, "{\"id\":%u,\"on\":%s}", i,
				led_state[i] ? "true" : "false");
	}
	off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]}");
	(void)off;

	return ove_httpd_resp_json(resp, 200, buf);
#else
	return ove_httpd_resp_json(resp, 200, "[]");
#endif
}

/* ── Route: POST /api/leds/:id ─────────────────────────────────── */

static int handle_leds_post(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
#ifdef CONFIG_OVE_LED
	const char *seg = ove_httpd_req_segment(req, 2);
	if (!seg)
		return ove_httpd_resp_error(resp, 400, "missing led id");

	int id = atoi(seg);
	if (id < 0 || (unsigned int)id >= ove_led_count() || id >= LED_MAX)
		return ove_httpd_resp_error(resp, 404, "invalid led id");

	const char *body = ove_httpd_req_body(req);
	if (!body)
		return ove_httpd_resp_error(resp, 400, "missing body");

	int val = 0;
	const char *on_key = strstr(body, "\"on\":");
	if (on_key) {
		if (strstr(on_key, "true"))
			val = 1;
		else if (strstr(on_key, "false"))
			val = 0;
		else
			return ove_httpd_resp_error(resp, 400, "expected true or false");
	} else {
		return ove_httpd_resp_error(resp, 400, "missing \"on\" field");
	}

	ove_led_set((unsigned int)id, val);
	led_state[id] = val;

	char buf[64];
	snprintf(buf, sizeof(buf), "{\"id\":%d,\"on\":%s}", id, val ? "true" : "false");
	return ove_httpd_resp_json(resp, 200, buf);
#else
	(void)req;
	return ove_httpd_resp_error(resp, 501, "LED support not enabled");
#endif
}

/* ── Route: GET /api/gpio/:port/:pin ───────────────────────────── */

static int handle_gpio_get(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
#ifdef CONFIG_OVE_GPIO
	const char *seg_port = ove_httpd_req_segment(req, 2);
	const char *seg_pin = ove_httpd_req_segment(req, 3);
	if (!seg_port || !seg_pin)
		return ove_httpd_resp_error(resp, 400, "missing port or pin");

	unsigned int port = (unsigned int)atoi(seg_port);
	unsigned int pin = (unsigned int)atoi(seg_pin);

	int value = ove_gpio_get(port, pin);
	if (value < 0)
		return ove_httpd_resp_error(resp, 500, "gpio read failed");

	char buf[128];
	snprintf(buf, sizeof(buf), "{\"port\":%u,\"pin\":%u,\"value\":%d}", port, pin, value);
	return ove_httpd_resp_json(resp, 200, buf);
#else
	(void)req;
	return ove_httpd_resp_error(resp, 501, "GPIO support not enabled");
#endif
}

/* ── Route: POST /api/gpio/:port/:pin ──────────────────────────── */

static int handle_gpio_post(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
#ifdef CONFIG_OVE_GPIO
	const char *seg_port = ove_httpd_req_segment(req, 2);
	const char *seg_pin = ove_httpd_req_segment(req, 3);
	if (!seg_port || !seg_pin)
		return ove_httpd_resp_error(resp, 400, "missing port or pin");

	unsigned int port = (unsigned int)atoi(seg_port);
	unsigned int pin = (unsigned int)atoi(seg_pin);

	const char *body = ove_httpd_req_body(req);
	if (!body)
		return ove_httpd_resp_error(resp, 400, "missing body");

	const char *val_key = strstr(body, "\"value\":");
	if (!val_key)
		return ove_httpd_resp_error(resp, 400, "missing \"value\" field");

	int value = atoi(val_key + 8); /* skip "value": */

	int ret = ove_gpio_set(port, pin, value);
	if (ret != OVE_OK)
		return ove_httpd_resp_error(resp, 500, "gpio write failed");

	char buf[128];
	snprintf(buf, sizeof(buf), "{\"port\":%u,\"pin\":%u,\"value\":%d}", port, pin, value);
	return ove_httpd_resp_json(resp, 200, buf);
#else
	(void)req;
	return ove_httpd_resp_error(resp, 501, "GPIO support not enabled");
#endif
}

/* ── Route: GET /api/network ───────────────────────────────────── */

static int handle_network(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;

	char ip_str[16] = "0.0.0.0";
	char gw_str[16] = "0.0.0.0";
	char nm_str[16] = "0.0.0.0";
	ove_sockaddr_t ip_addr, gw_addr, nm_addr;

	if (query_netif_addr(&ip_addr, &gw_addr, &nm_addr) == OVE_OK) {
		fmt_ip(ip_str, sizeof(ip_str), &ip_addr);
		fmt_ip(gw_str, sizeof(gw_str), &gw_addr);
		fmt_ip(nm_str, sizeof(nm_str), &nm_addr);
	}

	char buf[256];
	snprintf(buf, sizeof(buf),
		 "{\"ip\":\"%s\",\"gateway\":\"%s\","
		 "\"netmask\":\"%s\"}",
		 ip_str, gw_str, nm_str);

	return ove_httpd_resp_json(resp, 200, buf);
}

/* ── Route: GET /api/system/memory ─────────────────────────────── */

static int handle_system_memory(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;

	struct ove_mem_stats ms;
	if (ove_sys_get_mem_stats(&ms) != OVE_OK)
		return ove_httpd_resp_error(resp, 501, "not supported");

	char buf[128];
	snprintf(buf, sizeof(buf), "{\"total\":%zu,\"free\":%zu,\"used\":%zu,\"peak\":%zu}",
		 ms.total, ms.free, ms.used, ms.peak_used);

	return ove_httpd_resp_json(resp, 200, buf);
}

/* ── Route: GET /api/system/threads ───────────────────────────── */

static const char *state_str(ove_thread_state_t s)
{
	switch (s) {
	case OVE_THREAD_STATE_RUNNING:
		return "running";
	case OVE_THREAD_STATE_READY:
		return "ready";
	case OVE_THREAD_STATE_BLOCKED:
		return "blocked";
	case OVE_THREAD_STATE_SUSPENDED:
		return "suspended";
	case OVE_THREAD_STATE_TERMINATED:
		return "terminated";
	default:
		return "unknown";
	}
}

static int handle_system_threads(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;

	struct ove_thread_info threads[16];
	size_t count = 0;

	ove_thread_list(threads, 16, &count);

	char buf[2048];
	int off = 0;

	off += snprintf(buf + off, sizeof(buf) - (size_t)off, "{\"threads\":[");

	for (size_t i = 0; i < count; i++) {
		if (i > 0)
			off += snprintf(buf + off, sizeof(buf) - (size_t)off, ",");
		off += snprintf(buf + off, sizeof(buf) - (size_t)off,
				"{\"name\":\"%s\",\"state\":\"%s\","
				"\"priority\":%d,\"stack_used\":%zu}",
				threads[i].name ? threads[i].name : "?",
				state_str(threads[i].state), threads[i].priority,
				threads[i].stack_used);
		if ((size_t)off >= sizeof(buf) - 2)
			break;
	}

	off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]}");
	(void)off;

	return ove_httpd_resp_json(resp, 200, buf);
}

/* ── Route: GET /api/log ───────────────────────────────────────── */

static int handle_log(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;

	char buf[2048];
	int off = 0;

	off += snprintf(buf + off, sizeof(buf) - (size_t)off, "{\"lines\":[");

	log_lock();
	int first = 1;
	for (int i = 0; i < LOG_RING_LINES; i++) {
		int idx = (log_ring_head + i) % LOG_RING_LINES;

		if (log_ring[idx][0] == '\0')
			continue;

		if (strchr(log_ring[idx], '"') || strchr(log_ring[idx], '\\'))
			continue;

		if (!first)
			off += snprintf(buf + off, sizeof(buf) - (size_t)off, ",");
		first = 0;

		off += snprintf(buf + off, sizeof(buf) - (size_t)off, "\"%s\"", log_ring[idx]);

		if ((size_t)off >= sizeof(buf) - 2)
			break;
	}
	log_unlock();

	off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]}");
	(void)off;

	return ove_httpd_resp_json(resp, 200, buf);
}

/* ── Registration ──────────────────────────────────────────────── */

/* ── Audio / ML stats (conditional) ────────────────────────────── */

#ifdef CONFIG_OVE_AUDIO
static struct ove_audio_graph *s_audio_graph;

void ove_httpd_set_audio_graph(struct ove_audio_graph *g)
{
	s_audio_graph = g;
}

static int handle_audio_stats(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;
	if (!s_audio_graph)
		return ove_httpd_resp_error(resp, 501, "no audio graph set");

	struct ove_audio_graph_stats st;
	if (ove_audio_graph_get_stats(s_audio_graph, &st) != 0)
		return ove_httpd_resp_error(resp, 500, "stats failed");

	char buf[256];
	snprintf(buf, sizeof(buf),
		 "{\"cycles\":%u,\"underruns\":%u,\"overruns\":%u,"
		 "\"node_errors\":%u,\"max_us\":%u,\"avg_us\":%u}",
		 st.cycles, st.underruns, st.overruns, st.node_errors, st.max_process_us,
		 st.avg_process_us);

	return ove_httpd_resp_json(resp, 200, buf);
}
#endif /* CONFIG_OVE_AUDIO */

#ifdef CONFIG_OVE_INFER
static ove_model_t s_infer_model;

void ove_httpd_set_model(ove_model_t model)
{
	s_infer_model = model;
}

static int handle_infer_stats(ove_httpd_req_t *req, ove_httpd_resp_t *resp)
{
	(void)req;
	if (!s_infer_model)
		return ove_httpd_resp_error(resp, 501, "no model set");

	uint64_t latency = ove_model_last_inference_us(s_infer_model);

	char buf[64];
	snprintf(buf, sizeof(buf), "{\"last_inference_us\":%lu}", (unsigned long)latency);

	return ove_httpd_resp_json(resp, 200, buf);
}
#endif /* CONFIG_OVE_INFER */

/* ── WebSocket shell terminal ─────────────────────────────────── */

#if defined(CONFIG_OVE_NET_HTTPD_WS) && defined(CONFIG_OVE_SHELL)
#include "ove/shell.h"

static ove_httpd_ws_conn_t *s_shell_ws_conn;

static void ws_shell_output(const char *data, size_t len)
{
	if (s_shell_ws_conn)
		ove_httpd_ws_send(s_shell_ws_conn, data, len);
}

static void ws_shell_on_message(ove_httpd_ws_conn_t *conn, const void *data, size_t len)
{
	s_shell_ws_conn = conn;
	ove_shell_set_output_hook(ws_shell_output);

	char line[128];
	size_t copy = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
	memcpy(line, data, copy);
	line[copy] = '\0';

	ove_shell_process_line(line);
}

static void ws_shell_on_close(ove_httpd_ws_conn_t *conn)
{
	if (s_shell_ws_conn == conn) {
		s_shell_ws_conn = NULL;
		ove_shell_set_output_hook(NULL);
	}
}
#endif /* CONFIG_OVE_NET_HTTPD_WS && CONFIG_OVE_SHELL */

/* ── Registration ──────────────────────────────────────────────── */

void ove_httpd_register_builtin_routes(void)
{
#ifdef CONFIG_OVE_SYNC
	if (!s_log_mutex_inited) {
		ove_mutex_init(&s_log_mutex, &s_log_mutex_storage);
		s_log_mutex_inited = 1;
	}
#endif

	ove_httpd_route("GET", "/", handle_index);
	ove_httpd_route("GET", "/api/info", handle_info);
	ove_httpd_route("GET", "/api/leds", handle_leds_get);
	ove_httpd_route("POST", "/api/leds", handle_leds_post);
	ove_httpd_route("GET", "/api/gpio", handle_gpio_get);
	ove_httpd_route("POST", "/api/gpio", handle_gpio_post);
	ove_httpd_route("GET", "/api/network", handle_network);
	ove_httpd_route("GET", "/api/log", handle_log);
	ove_httpd_route("GET", "/api/system/memory", handle_system_memory);
	ove_httpd_route("GET", "/api/system/threads", handle_system_threads);

#ifdef CONFIG_OVE_AUDIO
	ove_httpd_route("GET", "/api/audio/stats", handle_audio_stats);
#endif
#ifdef CONFIG_OVE_INFER
	ove_httpd_route("GET", "/api/infer/stats", handle_infer_stats);
#endif

#ifdef CONFIG_OVE_NET_HTTPD_WS
	ove_httpd_ws_route("/ws/log", NULL, NULL);
#ifdef CONFIG_OVE_SHELL
	ove_httpd_ws_route("/ws/shell", ws_shell_on_message, ws_shell_on_close);
#endif
#endif
}

#endif /* CONFIG_OVE_NET_HTTPD */
