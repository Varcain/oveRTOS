/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NET_MQTT_H
#define OVE_NET_MQTT_H

/**
 * @defgroup ove_net_mqtt MQTT Client
 * @brief Lightweight MQTT 3.1.1 client for IoT pub/sub messaging.
 *
 * Supports CONNECT, PUBLISH (QoS 0/1), SUBSCRIBE, keep-alive, and
 * optional TLS.  Uses the oveRTOS socket layer for transport.
 *
 * @note Requires @c CONFIG_OVE_NET_MQTT (implies @c CONFIG_OVE_NET).
 *       When disabled every function is replaced by a no-op stub.
 * @{
 */

#include "ove/types.h"
#include "ove/net.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief MQTT QoS level. */
#ifdef __ZIG_CIMPORT__
typedef uint8_t ove_mqtt_qos_t;
#define OVE_MQTT_QOS0 ((ove_mqtt_qos_t)0) /**< At most once. */
#define OVE_MQTT_QOS1 ((ove_mqtt_qos_t)1) /**< At least once. */
#else
typedef enum {
	OVE_MQTT_QOS0 = 0, /**< At most once. */
	OVE_MQTT_QOS1 = 1, /**< At least once. */
} ove_mqtt_qos_t;
#endif

/**
 * @brief MQTT message callback.
 *
 * @param[in] topic       Topic string (not NUL-terminated).
 * @param[in] topic_len   Topic length in bytes.
 * @param[in] payload     Message payload.
 * @param[in] payload_len Payload length in bytes.
 * @param[in] user_data   Opaque pointer supplied at connect time.
 */
typedef void (*ove_mqtt_msg_cb)(const char *topic, size_t topic_len,
				const void *payload, size_t payload_len,
				void *user_data);

/**
 * @brief MQTT connection configuration.
 */
typedef struct {
	const char     *host;          /**< Broker hostname or IP. */
	uint16_t        port;          /**< Broker port (1883 or 8883). */
	const char     *client_id;     /**< Client identifier. */
	const char     *username;      /**< Username (may be NULL). */
	const char     *password;      /**< Password (may be NULL). */
	uint16_t        keep_alive_s;  /**< Keep-alive interval in seconds. */
	int             use_tls;       /**< Non-zero to use TLS. */
	ove_mqtt_msg_cb on_message;    /**< Message callback. */
	void           *user_data;     /**< Opaque pointer for callback. */
} ove_mqtt_config_t;

#include "ove/storage.h"

#ifdef CONFIG_OVE_NET_MQTT

/**
 * @brief Initialise an MQTT client from caller-supplied storage.
 *
 * @param[out] client  Handle written on success.
 * @param[in]  storage Caller-allocated storage.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_mqtt_client_init(ove_mqtt_client_t *client,
			      ove_mqtt_client_storage_t *storage);

/**
 * @brief De-initialise an MQTT client.
 *
 * @param[in] client Handle returned by ove_mqtt_client_init().
 */
void ove_mqtt_client_deinit(ove_mqtt_client_t client);

/**
 * @brief Connect to an MQTT broker.
 *
 * @param[in] client MQTT client handle.
 * @param[in] cfg    Connection configuration.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_mqtt_connect(ove_mqtt_client_t client,
			  const ove_mqtt_config_t *cfg);

/**
 * @brief Disconnect from the MQTT broker.
 *
 * @param[in] client MQTT client handle.
 */
void ove_mqtt_disconnect(ove_mqtt_client_t client);

/**
 * @brief Publish a message.
 *
 * @param[in] client      MQTT client handle.
 * @param[in] topic       Topic string (NUL-terminated).
 * @param[in] payload     Message payload.
 * @param[in] payload_len Payload length in bytes.
 * @param[in] qos         QoS level.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_mqtt_publish(ove_mqtt_client_t client,
			  const char *topic,
			  const void *payload, size_t payload_len,
			  ove_mqtt_qos_t qos);

/**
 * @brief Subscribe to a topic.
 *
 * @param[in] client MQTT client handle.
 * @param[in] topic  Topic filter (NUL-terminated).
 * @param[in] qos    Maximum QoS level.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_mqtt_subscribe(ove_mqtt_client_t client,
			    const char *topic, ove_mqtt_qos_t qos);

/**
 * @brief Unsubscribe from a topic.
 *
 * @param[in] client MQTT client handle.
 * @param[in] topic  Topic filter (NUL-terminated).
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_mqtt_unsubscribe(ove_mqtt_client_t client, const char *topic);

/**
 * @brief Process incoming packets and send keep-alive pings.
 *
 * Must be called periodically (typically in a loop or timer).
 *
 * @param[in] client     MQTT client handle.
 * @param[in] timeout_ms Maximum time to wait for incoming data.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_mqtt_loop(ove_mqtt_client_t client, uint32_t timeout_ms);

#ifdef OVE_HEAP_NET_MQTT
/**
 * @brief Heap-allocate and initialise an MQTT client.
 *
 * @param[out] client Handle written on success.
 * @return OVE_OK on success, negative error code on failure.
 */
int  ove_mqtt_client_create(ove_mqtt_client_t *client);

/**
 * @brief Destroy a heap-allocated MQTT client.
 *
 * @param[in] client Handle returned by ove_mqtt_client_create().
 */
void ove_mqtt_client_destroy(ove_mqtt_client_t client);
#endif /* OVE_HEAP_NET_MQTT */

#else /* !CONFIG_OVE_NET_MQTT */

/** @cond INTERNAL */
#ifndef CONFIG_OVE_NET_MQTT
typedef struct { uint8_t _unused; } ove_mqtt_client_storage_t;
#endif

static inline int  ove_mqtt_client_init(ove_mqtt_client_t *client, ove_mqtt_client_storage_t *storage) { (void)client; (void)storage; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_mqtt_client_deinit(ove_mqtt_client_t client) { (void)client; }
static inline int  ove_mqtt_connect(ove_mqtt_client_t client, const ove_mqtt_config_t *cfg) { (void)client; (void)cfg; return OVE_ERR_NOT_SUPPORTED; }
static inline void ove_mqtt_disconnect(ove_mqtt_client_t client) { (void)client; }
static inline int  ove_mqtt_publish(ove_mqtt_client_t client, const char *topic, const void *payload, size_t payload_len, ove_mqtt_qos_t qos) { (void)client; (void)topic; (void)payload; (void)payload_len; (void)qos; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_mqtt_subscribe(ove_mqtt_client_t client, const char *topic, ove_mqtt_qos_t qos) { (void)client; (void)topic; (void)qos; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_mqtt_unsubscribe(ove_mqtt_client_t client, const char *topic) { (void)client; (void)topic; return OVE_ERR_NOT_SUPPORTED; }
static inline int  ove_mqtt_loop(ove_mqtt_client_t client, uint32_t timeout_ms) { (void)client; (void)timeout_ms; return OVE_ERR_NOT_SUPPORTED; }
/** @endcond */

#endif /* CONFIG_OVE_NET_MQTT */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_NET_MQTT_H */
