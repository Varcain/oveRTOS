/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Portable MQTT 3.1.1 client.
 *
 * Implements the MQTT 3.1.1 packet format and state machine.
 * Uses oveRTOS socket (and optionally TLS) for transport.
 * Lives in backends/common/ because it has no platform-specific code.
 */

#include "ove/ove.h"
#include "ove/net_mqtt.h"
#include "ove_backend_common.h"
#include "ove_mqtt_topic.h"

#include <string.h>

/* ---------- MQTT packet types ---------- */

#define MQTT_CONNECT 0x10
#define MQTT_CONNACK 0x20
#define MQTT_PUBLISH 0x30
#define MQTT_PUBACK 0x40
#define MQTT_SUBSCRIBE 0x82
#define MQTT_SUBACK 0x90
#define MQTT_UNSUBSCRIBE 0xA2
#define MQTT_UNSUBACK 0xB0
#define MQTT_PINGREQ 0xC0
#define MQTT_PINGRESP 0xD0
#define MQTT_DISCONNECT 0xE0

/* ---------- I/O helpers ---------- */

static int mqtt_send(struct ove_mqtt_client *c, const void *data, size_t len)
{
	size_t sent = 0;
#ifdef CONFIG_OVE_NET_TLS
	if (c->tls) {
		return ove_tls_send(c->tls, data, len, &sent);
	}
#endif
	return ove_socket_send(c->sock, data, len, &sent);
}

static int mqtt_recv(struct ove_mqtt_client *c, void *buf, size_t len, size_t *received,
		     uint32_t timeout_ms)
{
#ifdef CONFIG_OVE_NET_TLS
	if (c->tls) {
		return ove_tls_recv(c->tls, buf, len, received);
	}
#endif
	return ove_socket_recv(c->sock, buf, len, received, timeout_ms);
}

/* ---------- Encoding helpers ---------- */

static size_t encode_remaining_length(uint8_t *buf, size_t len)
{
	size_t i = 0;
	do {
		uint8_t byte = (uint8_t)(len % 128);
		len /= 128;
		if (len > 0)
			byte |= 0x80;
		buf[i++] = byte;
	} while (len > 0);
	return i;
}

/*
 * Decode an MQTT Remaining Length varint (MQTT 3.1.1 §2.2.3).
 * The spec caps the encoding at 4 bytes (max value 268,435,455).
 * Returns the number of bytes consumed, or 0 on malformed input.
 */
static size_t decode_remaining_length(const uint8_t *buf, size_t buflen, size_t *value)
{
	*value = 0;
	size_t multiplier = 1;
	for (size_t i = 0; i < 4; i++) {
		if (i >= buflen)
			return 0;
		uint8_t b = buf[i];
		*value += (b & 0x7F) * multiplier;
		if ((b & 0x80) == 0)
			return i + 1;
		multiplier *= 128;
	}
	/* 4th byte still had the continuation bit — malformed per spec. */
	return 0;
}

static void put_u16(uint8_t *buf, uint16_t val)
{
	buf[0] = (uint8_t)(val >> 8);
	buf[1] = (uint8_t)(val & 0xFF);
}

static uint16_t get_u16(const uint8_t *buf)
{
	return (uint16_t)((buf[0] << 8) | buf[1]);
}

/* ---------- MQTT wildcard topic matching ---------- */

/*
 * Filter matching lives in ove_mqtt_topic.c so unit tests can link it
 * without pulling in the full MQTT client and its socket/TLS deps.
 */

static int mqtt_any_sub_matches(struct ove_mqtt_client *c, const char *topic, size_t tlen)
{
	/* No filters stored → accept all (backward compat) */
	if (c->sub_count == 0)
		return 1;

	for (unsigned int i = 0; i < c->sub_count; i++) {
		if (ove_mqtt_topic_matches(c->sub_filters[i], strlen(c->sub_filters[i]), topic,
					   tlen))
			return 1;
	}
	return 0;
}

/* ---------- Internal: dispatch incoming PUBLISH ---------- */

static void dispatch_publish(struct ove_mqtt_client *c, const uint8_t *pkt, size_t pkt_len)
{
	if (!c->on_message)
		return;
	if (pkt_len < 2)
		return;

	size_t rem_len = 0;
	size_t rl_bytes = decode_remaining_length(pkt + 1, pkt_len - 1, &rem_len);
	if (rl_bytes == 0)
		return;
	size_t hdr_bytes = 1 + rl_bytes;
	if (hdr_bytes > pkt_len || rem_len > pkt_len - hdr_bytes)
		return;

	/* Cursor bounds: the variable header + payload live in
	 * [pkt + hdr_bytes, pkt + hdr_bytes + rem_len). Every advance
	 * below must be checked against `end` — the packet is attacker
	 * controlled and MQTT field lengths come from the wire. */
	const uint8_t *ptr = pkt + hdr_bytes;
	const uint8_t *end = ptr + rem_len;

	if ((size_t)(end - ptr) < 2)
		return;
	uint16_t tlen = get_u16(ptr);
	ptr += 2;

	if ((size_t)(end - ptr) < tlen)
		return;
	const char *topic = (const char *)ptr;
	ptr += tlen;

	uint8_t qos = (pkt[0] >> 1) & 0x03;
	if (qos >= 1) {
		if ((size_t)(end - ptr) < 2)
			return;
		uint16_t pkt_id = get_u16(ptr);
		ptr += 2;
		if (qos == 1) {
			uint8_t ack[4] = {MQTT_PUBACK, 0x02, 0, 0};
			put_u16(ack + 2, pkt_id);
			mqtt_send(c, ack, 4);
		}
	}

	size_t payload_len = (size_t)(end - ptr);

	if (mqtt_any_sub_matches(c, topic, tlen))
		c->on_message(topic, tlen, ptr, payload_len, c->user_data);
}

/*
 * Compute the total length of one MQTT packet starting at buf.
 * Returns 0 if the buffer is too short to contain a complete packet.
 */
static size_t mqtt_packet_len(const uint8_t *buf, size_t buflen)
{
	if (buflen < 2)
		return 0;
	size_t rem = 0;
	size_t hdr = 1 + decode_remaining_length(buf + 1, buflen - 1, &rem);
	if (hdr <= 1)
		return 0;
	size_t total = hdr + rem;
	return (total <= buflen) ? total : 0;
}

/*
 * Read packets until the expected type arrives (or timeout).
 * Dispatches any incoming PUBLISH messages to the callback.
 * Handles multiple MQTT packets arriving in a single TCP read.
 */
static int mqtt_wait_for(struct ove_mqtt_client *c, uint8_t expected_type, uint32_t timeout_ms)
{
	for (int attempts = 0; attempts < 10; attempts++) {
		size_t got = 0;
		int ret = mqtt_recv(c, c->rx_buf, c->rx_size, &got, timeout_ms);
		if (ret == OVE_ERR_TIMEOUT)
			continue; /* try again */
		if (ret != OVE_OK)
			return ret;
		if (got == 0)
			continue;

		/* Scan through all packets in the buffer */
		size_t offset = 0;
		while (offset < got) {
			size_t pkt_len = mqtt_packet_len(c->rx_buf + offset, got - offset);
			if (pkt_len == 0)
				break;

			uint8_t pkt_type = c->rx_buf[offset] & 0xF0;
			if (pkt_type == expected_type)
				return OVE_OK;

			if (pkt_type == MQTT_PUBLISH) {
				dispatch_publish(c, c->rx_buf + offset, pkt_len);
			}
			offset += pkt_len;
		}
	}
	return OVE_ERR_TIMEOUT;
}

/* ---------- MQTT client ---------- */

int ove_mqtt_client_init(ove_mqtt_client_t *client, ove_mqtt_client_storage_t *storage)
{
	if (!client || !storage)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mqtt_client *c = (struct ove_mqtt_client *)storage;
	memset(c, 0, sizeof(*c));
	*client = c;
	return OVE_OK;
}

void ove_mqtt_client_deinit(ove_mqtt_client_t client)
{
	if (client) {
#ifndef CONFIG_OVE_ZERO_HEAP
		if (client->rx_buf)
			OVE_BACKEND_FREE(client->rx_buf);
		if (client->tx_buf)
			OVE_BACKEND_FREE(client->tx_buf);
#endif
		memset(client, 0, sizeof(*client));
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_mqtt_client_create(ove_mqtt_client_t *client)
{
	if (!client)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mqtt_client *c = OVE_BACKEND_MALLOC(sizeof(*c));
	if (!c)
		return OVE_ERR_NO_MEMORY;
	memset(c, 0, sizeof(*c));
	*client = c;
	return OVE_OK;
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

#ifndef CONFIG_OVE_ZERO_HEAP
void ove_mqtt_client_destroy(ove_mqtt_client_t client)
{
	if (client) {
		ove_mqtt_client_deinit(client);
		OVE_BACKEND_FREE(client);
	}
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

int ove_mqtt_connect(ove_mqtt_client_t client, const ove_mqtt_config_t *cfg)
{
	if (!client || !cfg || !cfg->host || !cfg->client_id)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mqtt_client *c = client;

	/* Allocate buffers */
#ifdef CONFIG_OVE_ZERO_HEAP
	c->rx_buf = c->_rx_buf;
	c->rx_size = sizeof(c->_rx_buf);
	c->tx_buf = c->_tx_buf;
	c->tx_size = sizeof(c->_tx_buf);
#else
#ifdef CONFIG_OVE_NET_MQTT_RX_BUF
	c->rx_size = CONFIG_OVE_NET_MQTT_RX_BUF;
#else
	c->rx_size = 1024;
#endif
#ifdef CONFIG_OVE_NET_MQTT_TX_BUF
	c->tx_size = CONFIG_OVE_NET_MQTT_TX_BUF;
#else
	c->tx_size = 512;
#endif
	c->rx_buf = OVE_BACKEND_MALLOC(c->rx_size);
	c->tx_buf = OVE_BACKEND_MALLOC(c->tx_size);
	if (!c->rx_buf || !c->tx_buf)
		return OVE_ERR_NO_MEMORY;
#endif

	c->keep_alive_s = cfg->keep_alive_s ? cfg->keep_alive_s : 60;
	c->on_message = cfg->on_message;
	c->user_data = cfg->user_data;
	c->pkt_id = 1;

	/* DNS resolve */
	ove_sockaddr_t addr;
	int ret = ove_dns_resolve(cfg->host, &addr, 10000);
	if (ret != OVE_OK)
		return ret;
	addr.port = cfg->port;

	/* Open socket (use embedded storage so it survives function return) */
	ret = ove_socket_open(&c->sock, &c->sock_storage, OVE_AF_INET, OVE_SOCK_STREAM);
	if (ret != OVE_OK)
		return ret;

	ret = ove_socket_connect(c->sock, &addr, 10000);
	if (ret != OVE_OK) {
		ove_socket_close(c->sock);
		c->sock = NULL;
		return ret;
	}

	/* TLS if requested */
#ifdef CONFIG_OVE_NET_TLS
	if (cfg->use_tls) {
		ove_tls_storage_t tls_storage;
		ove_tls_t tls = NULL;
		ret = ove_tls_init(&tls, &tls_storage);
		if (ret != OVE_OK) {
			ove_socket_close(c->sock);
			c->sock = NULL;
			return ret;
		}
		ove_tls_config_t tls_cfg = {
			.ca_cert = cfg->tls_ca_cert,
			.ca_cert_len = cfg->tls_ca_cert_len,
			.hostname = cfg->host,
			.allow_insecure = cfg->tls_allow_insecure,
		};
		ret = ove_tls_handshake(tls, c->sock, &tls_cfg);
		if (ret != OVE_OK) {
			ove_tls_deinit(tls);
			ove_socket_close(c->sock);
			c->sock = NULL;
			return ret;
		}
		c->tls = tls;
	}
#endif

	/* Build CONNECT packet */
	size_t cid_len = strlen(cfg->client_id);
	size_t usr_len = cfg->username ? strlen(cfg->username) : 0;
	size_t pwd_len = cfg->password ? strlen(cfg->password) : 0;

	/* Variable header: protocol name(6) + level(1) + flags(1) + keepalive(2) = 10 */
	size_t remaining = 10 + 2 + cid_len;
	uint8_t flags = 0x02; /* clean session */
	if (cfg->username) {
		flags |= 0x80;
		remaining += 2 + usr_len;
	}
	if (cfg->password) {
		flags |= 0x40;
		remaining += 2 + pwd_len;
	}

	uint8_t *p = c->tx_buf;
	*p++ = MQTT_CONNECT;
	p += encode_remaining_length(p, remaining);

	/* Protocol name */
	put_u16(p, 4);
	p += 2;
	memcpy(p, "MQTT", 4);
	p += 4;
	*p++ = 0x04; /* protocol level 3.1.1 */
	*p++ = flags;
	put_u16(p, c->keep_alive_s);
	p += 2;

	/* Client ID */
	put_u16(p, (uint16_t)cid_len);
	p += 2;
	memcpy(p, cfg->client_id, cid_len);
	p += cid_len;

	/* Username */
	if (cfg->username) {
		put_u16(p, (uint16_t)usr_len);
		p += 2;
		memcpy(p, cfg->username, usr_len);
		p += usr_len;
	}
	/* Password */
	if (cfg->password) {
		put_u16(p, (uint16_t)pwd_len);
		p += 2;
		memcpy(p, cfg->password, pwd_len);
		p += pwd_len;
	}

	ret = mqtt_send(c, c->tx_buf, (size_t)(p - c->tx_buf));
	if (ret != OVE_OK)
		goto fail;

	/* Read CONNACK */
	size_t got = 0;
	ret = mqtt_recv(c, c->rx_buf, 4, &got, 10000);
	if (ret != OVE_OK)
		goto fail;
	if (got < 4 || c->rx_buf[0] != MQTT_CONNACK || c->rx_buf[3] != 0) {
		ret = OVE_ERR_NOT_SUPPORTED;
		goto fail;
	}

	c->connected = 1;
	return OVE_OK;

fail:
#ifdef CONFIG_OVE_NET_TLS
	if (c->tls) {
		ove_tls_close(c->tls);
		ove_tls_deinit(c->tls);
		c->tls = NULL;
	}
#endif
	ove_socket_close(c->sock);
	c->sock = NULL;
	return ret;
}

void ove_mqtt_disconnect(ove_mqtt_client_t client)
{
	if (!client || !client->connected)
		return;
	struct ove_mqtt_client *c = client;

	uint8_t pkt[2] = {MQTT_DISCONNECT, 0x00};
	mqtt_send(c, pkt, 2);
	c->connected = 0;

#ifdef CONFIG_OVE_NET_TLS
	if (c->tls) {
		ove_tls_close(c->tls);
		ove_tls_deinit(c->tls);
		c->tls = NULL;
	}
#endif
	ove_socket_close(c->sock);
	c->sock = NULL;
}

int ove_mqtt_publish(ove_mqtt_client_t client, const char *topic, const void *payload,
		     size_t payload_len, ove_mqtt_qos_t qos)
{
	if (!client || !topic || !client->connected)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mqtt_client *c = client;

	size_t topic_len = strlen(topic);
	size_t remaining = 2 + topic_len + payload_len;
	if (qos == OVE_MQTT_QOS1)
		remaining += 2; /* packet ID */

	uint8_t *p = c->tx_buf;
	*p = MQTT_PUBLISH;
	if (qos == OVE_MQTT_QOS1)
		*p |= 0x02;
	p++;
	p += encode_remaining_length(p, remaining);

	put_u16(p, (uint16_t)topic_len);
	p += 2;
	memcpy(p, topic, topic_len);
	p += topic_len;

	if (qos == OVE_MQTT_QOS1) {
		put_u16(p, c->pkt_id++);
		p += 2;
		if (c->pkt_id == 0)
			c->pkt_id = 1;
	}

	if (payload && payload_len > 0) {
		memcpy(p, payload, payload_len);
		p += payload_len;
	}

	int ret = mqtt_send(c, c->tx_buf, (size_t)(p - c->tx_buf));
	if (ret != OVE_OK)
		return ret;

	/* Wait for PUBACK on QoS 1 */
	if (qos == OVE_MQTT_QOS1) {
		ret = mqtt_wait_for(c, MQTT_PUBACK, 5000);
		if (ret != OVE_OK)
			return ret;
	}

	return OVE_OK;
}

int ove_mqtt_subscribe(ove_mqtt_client_t client, const char *topic, ove_mqtt_qos_t qos)
{
	if (!client || !topic || !client->connected)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mqtt_client *c = client;

	size_t topic_len = strlen(topic);
	size_t remaining = 2 + 2 + topic_len + 1; /* pkt_id + topic + qos */

	uint8_t *p = c->tx_buf;
	*p++ = MQTT_SUBSCRIBE;
	p += encode_remaining_length(p, remaining);

	put_u16(p, c->pkt_id++);
	p += 2;
	if (c->pkt_id == 0)
		c->pkt_id = 1;

	put_u16(p, (uint16_t)topic_len);
	p += 2;
	memcpy(p, topic, topic_len);
	p += topic_len;
	*p++ = (uint8_t)qos;

	int ret = mqtt_send(c, c->tx_buf, (size_t)(p - c->tx_buf));
	if (ret != OVE_OK)
		return ret;

	/* Wait for SUBACK */
	ret = mqtt_wait_for(c, MQTT_SUBACK, 5000);
	if (ret != OVE_OK)
		return ret;

	/* Store filter for wildcard matching */
	if (c->sub_count < CONFIG_OVE_NET_MQTT_MAX_SUBS) {
		strncpy(c->sub_filters[c->sub_count], topic, 63);
		c->sub_filters[c->sub_count][63] = '\0';
		c->sub_count++;
	}

	return OVE_OK;
}

int ove_mqtt_unsubscribe(ove_mqtt_client_t client, const char *topic)
{
	if (!client || !topic || !client->connected)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mqtt_client *c = client;

	size_t topic_len = strlen(topic);
	size_t remaining = 2 + 2 + topic_len; /* pkt_id + topic */

	uint8_t *p = c->tx_buf;
	*p++ = MQTT_UNSUBSCRIBE;
	p += encode_remaining_length(p, remaining);

	put_u16(p, c->pkt_id++);
	p += 2;
	if (c->pkt_id == 0)
		c->pkt_id = 1;

	put_u16(p, (uint16_t)topic_len);
	p += 2;
	memcpy(p, topic, topic_len);
	p += topic_len;

	int ret = mqtt_send(c, c->tx_buf, (size_t)(p - c->tx_buf));
	if (ret != OVE_OK)
		return ret;

	/* Wait for UNSUBACK */
	ret = mqtt_wait_for(c, MQTT_UNSUBACK, 2000);
	if (ret != OVE_OK)
		return ret;

	/* Remove filter from subscription table */
	for (unsigned int i = 0; i < c->sub_count; i++) {
		if (strncmp(c->sub_filters[i], topic, 63) == 0) {
			/* Shift remaining entries down */
			for (unsigned int j = i; j + 1 < c->sub_count; j++)
				memcpy(c->sub_filters[j], c->sub_filters[j + 1], 64);
			c->sub_count--;
			break;
		}
	}

	return OVE_OK;
}

int ove_mqtt_loop(ove_mqtt_client_t client, uint32_t timeout_ms)
{
	if (!client || !client->connected)
		return OVE_ERR_INVALID_PARAM;
	struct ove_mqtt_client *c = client;

	size_t got = 0;
	int ret = mqtt_recv(c, c->rx_buf, c->rx_size, &got, timeout_ms);
	if (ret == OVE_ERR_TIMEOUT) {
		/* Send PINGREQ on timeout (keepalive) */
		uint8_t ping[2] = {MQTT_PINGREQ, 0x00};
		return mqtt_send(c, ping, 2);
	}
	if (ret != OVE_OK)
		return ret;

	/* Process received packet */
	uint8_t pkt_type = c->rx_buf[0] & 0xF0;

	if (pkt_type == MQTT_PUBLISH) {
		dispatch_publish(c, c->rx_buf, got);
	} else if (pkt_type == MQTT_PINGRESP) {
		/* Pong received, nothing to do */
	}

	return OVE_OK;
}
