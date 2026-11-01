/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file net_mqtt.hpp
 * @brief RAII C++ wrappers for the oveRTOS MQTT client API
 */

#pragma once

#include <ove/net_mqtt.h>
#include <ove/types.hpp>
#include <ove/error.hpp>
#include <string_view>

#ifdef CONFIG_OVE_NET_MQTT

namespace ove::mqtt
{

/**
 * @namespace ove::mqtt
 * @brief C++ wrappers around the oveRTOS MQTT client API.
 *
 * Available when `CONFIG_OVE_NET_MQTT` is enabled.  Provides a RAII `Client`
 * that manages the MQTT connection lifecycle, QoS-typed publish/subscribe,
 * and a stateless callback trampoline for incoming messages.
 */

/**
 * @enum Qos
 * @brief MQTT Quality of Service level.
 */
enum class Qos : uint8_t {
	AtMostOnce = 0,	 /**< Fire and forget (QoS 0). */
	AtLeastOnce = 1, /**< Acknowledged delivery (QoS 1). */
};

/**
 * @struct Config
 * @brief MQTT connection configuration.
 *
 * Broker credentials and session settings with C++ default member initialisers.
 */
struct Config {
	const char *host{};	   /**< Broker hostname or IP. */
	uint16_t port{1883};	   /**< Broker port (1883 or 8883). */
	const char *client_id{};   /**< Client identifier. */
	const char *username{};	   /**< Username (may be nullptr). */
	const char *password{};	   /**< Password (may be nullptr). */
	uint16_t keep_alive_s{30}; /**< Keep-alive interval in seconds. */
	bool use_tls{false};	   /**< Set true to enable TLS. */
};

/**
 * @class Client
 * @brief RAII wrapper around an oveRTOS MQTT client handle.
 *
 * Constructs the underlying MQTT client on creation and destroys it on
 * destruction. A static trampoline routes through this object's own callback
 * field, without an allocated closure. The C context retains this object's
 * address, so Client is non-copyable and non-movable in both allocation modes.
 * Serialize calls on one client. Message views last until callback return;
 * a callback may disconnect, but must not reconnect, destroy the client, or
 * start another packet operation on it. Heap buffers are reused until cleanup.
 */
class Client
{
      public:
	/**
	 * @brief Stateless message callback type.
	 *
	 * Must be a plain function pointer (or a stateless lambda convertible
	 * to one).  Receives the topic and payload as `std::string_view`.
	 */
	using MsgFn = void (*)(std::string_view topic, std::string_view payload);

	/**
	 * @brief Constructs and initialises the MQTT client.
	 *
	 * Calls `ove_mqtt_client_init` (zero-heap) or `ove_mqtt_client_create`
	 * (heap).  Asserts at startup if initialisation fails.
	 */
	Client()
	{
#ifdef CONFIG_OVE_ZERO_HEAP
		int err = ove_mqtt_client_init(&handle_, &storage_);
#else
		int err = ove_mqtt_client_create(&handle_);
#endif
		OVE_STATIC_INIT_ASSERT(err == OVE_OK);
	}

	/**
	 * @brief Destroys the MQTT client, releasing the underlying resource.
	 *
	 * Closes a live transport and releases retained heap buffers. If the
	 * handle is null the destructor is a no-op.
	 */
	~Client() noexcept
	{
		if (!handle_)
			return;
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_mqtt_client_deinit(handle_);
#else
		ove_mqtt_client_destroy(handle_);
#endif
	}

	Client(const Client &) = delete;
	Client &operator=(const Client &) = delete;
	Client(Client &&) = delete;
	Client &operator=(Client &&) = delete;

	/**
	 * @brief Connects to an MQTT broker.
	 *
	 * Optionally registers a stateless message callback that is invoked
	 * for every incoming PUBLISH. Each instance retains its own callback.
	 * An established client returns Busy with its original callback and
	 * session unchanged. Failed attempts release transport, retain one reusable
	 * buffer pair, and preserve callback ownership. Success starts a clean session.
	 * Credentials and the entire encoded CONNECT must fit configured bounds;
	 * RX must hold at least four bytes. Calls require caller serialization.
	 *
	 * @param[in] cfg        Connection configuration.
	 * @param[in] on_message Message callback (nullptr to disable).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> connect(const Config &cfg, MsgFn on_message = nullptr) noexcept
	{
		MsgFn previous = msg_fn_;
		msg_fn_ = on_message;
		ove_mqtt_config_t c{};
		c.host = cfg.host;
		c.port = cfg.port;
		c.client_id = cfg.client_id;
		c.username = cfg.username;
		c.password = cfg.password;
		c.keep_alive_s = cfg.keep_alive_s;
		c.use_tls = cfg.use_tls ? 1 : 0;
		c.on_message = on_message ? trampoline_ : nullptr;
		c.user_data = this;
		int rc = ove_mqtt_connect(handle_, &c);
		if (rc != OVE_OK)
			msg_fn_ = previous;
		return from_rc(rc);
	}

	/**
	 * @brief Disconnects from the MQTT broker.
	 * Releases transport even if the farewell fails; retains buffers until
	 * cleanup. May be called inside the message callback without invalidating
	 * its packet views. Repeated disconnect on a live client is harmless.
	 */
	void disconnect()
	{
		ove_mqtt_disconnect(handle_);
	}

	/**
	 * @brief Publishes a message (raw pointer + length).
	 * @param[in] topic   Topic string (NUL-terminated).
	 * @param[in] payload Message payload.
	 * @param[in] len     Payload length in bytes.
	 * @param[in] qos     QoS level (default: AtMostOnce).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> publish(const char *topic, const void *payload, size_t len,
					   Qos qos = Qos::AtMostOnce) noexcept
	{
		return from_rc(ove_mqtt_publish(handle_, topic, payload, len,
						static_cast<ove_mqtt_qos_t>(qos)));
	}

	/**
	 * @brief Publishes a message from a string_view.
	 * @param[in] topic   Topic string (NUL-terminated).
	 * @param[in] payload Message payload as a string_view.
	 * @param[in] qos     QoS level (default: AtMostOnce).
	 * @return As @ref publish(const char*, const void*, size_t, Qos).
	 */
	[[nodiscard]] Result<void> publish(const char *topic, std::string_view payload,
					   Qos qos = Qos::AtMostOnce) noexcept
	{
		return publish(topic, payload.data(), payload.size(), qos);
	}

	/**
	 * @brief Subscribes to a topic.
	 * @param[in] topic Topic filter (NUL-terminated).
	 * @param[in] qos   Maximum QoS level (default: AtMostOnce).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> subscribe(const char *topic, Qos qos = Qos::AtMostOnce) noexcept
	{
		return from_rc(
			ove_mqtt_subscribe(handle_, topic, static_cast<ove_mqtt_qos_t>(qos)));
	}

	/**
	 * @brief Unsubscribes from a topic.
	 * @param[in] topic Topic filter (NUL-terminated).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void> unsubscribe(const char *topic) noexcept
	{
		return from_rc(ove_mqtt_unsubscribe(handle_, topic));
	}

	/**
	 * @brief Processes incoming packets and sends keep-alive pings.
	 *
	 * Must be called periodically (typically in a loop or timer).
	 *
	 * @param[in] timeout Maximum time to wait for incoming data (any
	 *                    `std::chrono::duration` unit; defaults to
	 *                    500 ms).
	 * @return Empty `Result<void>` on success; `unexpected`
	 *         @ref Error on failure.
	 */
	[[nodiscard]] Result<void>
	loop(std::chrono::nanoseconds timeout = std::chrono::milliseconds{500}) noexcept
	{
		return from_rc(ove_mqtt_loop(handle_, to_timeout_ns(timeout)));
	}

	/**
	 * @brief Returns `true` if the underlying client handle is non-null.
	 * @return `true` when the client was successfully initialised.
	 */
	bool valid() const
	{
		return handle_ != nullptr;
	}

	/**
	 * @brief Returns the raw oveRTOS MQTT client handle.
	 * @return The opaque `ove_mqtt_client_t` handle.
	 */
	ove_mqtt_client_t handle() const
	{
		return handle_;
	}

      private:
	/**
	 * @brief Internal C-callable trampoline that adapts the C callback
	 *        signature to the C++ `MsgFn` type.
	 */
	static void trampoline_(const char *topic, size_t topic_len, const void *payload,
				size_t payload_len, void *user_data)
	{
		auto *self = static_cast<Client *>(user_data);
		if (self && self->msg_fn_) {
			self->msg_fn_(std::string_view(topic, topic_len),
				      std::string_view(static_cast<const char *>(payload),
						       payload_len));
		}
	}

	MsgFn msg_fn_{};

	ove_mqtt_client_t handle_ = nullptr;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_mqtt_client_storage_t storage_ = {};
#endif
};

} /* namespace ove::mqtt */

#endif /* CONFIG_OVE_NET_MQTT */
