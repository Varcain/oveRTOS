/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define CONFIG_OVE_NET_MQTT 1
#ifndef CONFIG_OVE_NET_MQTT_RX_BUF
#define CONFIG_OVE_NET_MQTT_RX_BUF 1024
#endif
#ifndef CONFIG_OVE_NET_MQTT_TX_BUF
#define CONFIG_OVE_NET_MQTT_TX_BUF 512
#endif
#define ove_mqtt_client_init ove_test_cpp_mqtt_init
#define ove_mqtt_client_create ove_test_cpp_mqtt_create
#define ove_mqtt_client_deinit ove_test_cpp_mqtt_deinit
#define ove_mqtt_client_destroy ove_test_cpp_mqtt_destroy
#define ove_mqtt_connect ove_test_cpp_mqtt_connect
#include "../framework/ove_test.hpp"
#include <ove/net_mqtt.hpp>
#undef ove_mqtt_client_init
#undef ove_mqtt_client_create
#undef ove_mqtt_client_deinit
#undef ove_mqtt_client_destroy
#undef ove_mqtt_connect

#include <array>
#include <cstring>

namespace
{
struct CallbackClient {
	ove_mqtt_client_t handle = nullptr;
	ove_mqtt_config_t config{};
	bool connected = false;
#ifndef CONFIG_OVE_ZERO_HEAP
	ove_mqtt_client_storage_t storage{};
#endif
};

std::array<CallbackClient, 4> clients;
unsigned first_calls;
unsigned second_calls;

CallbackClient *find_client(ove_mqtt_client_t handle)
{
	for (auto &client : clients)
		if (client.handle == handle)
			return &client;
	return nullptr;
}

void first_callback(std::string_view, std::string_view)
{
	++first_calls;
}

void second_callback(std::string_view, std::string_view)
{
	++second_calls;
}

void deliver(unsigned index)
{
	auto &client = clients[index];
	if (client.config.on_message)
		client.config.on_message("t", 1, "v", 1, client.config.user_data);
}

int setup(void **)
{
	clients = {};
	first_calls = second_calls = 0;
	return 0;
}

void test_cpp_mqtt_callbacks_are_per_client(void **)
{
	bool connected;
	{
		ove::mqtt::Client first;
		ove::mqtt::Client second;
		ove::mqtt::Config config{.host = "broker", .client_id = "client"};
		auto a = first.connect(config, first_callback);
		auto b = second.connect(config, second_callback);
		connected = a.has_value() && b.has_value();
		deliver(0);
		deliver(1);
	}
	assert_true(connected);
	assert_int_equal(first_calls, 1);
	assert_int_equal(second_calls, 1);
	assert_null(clients[0].handle);
	assert_null(clients[1].handle);
}

void test_cpp_mqtt_failed_connect_keeps_callback(void **)
{
	bool connected, rejected;
	{
		ove::mqtt::Client client;
		ove::mqtt::Config config{.host = "broker", .client_id = "client"};
		connected = client.connect(config, first_callback).has_value();
		auto result = client.connect(config, second_callback);
		rejected = !result.has_value() && static_cast<int>(result.error()) == OVE_ERR_BUSY;
		deliver(0);
	}
	assert_true(connected && rejected);
	assert_int_equal(first_calls, 1);
	assert_int_equal(second_calls, 0);
	assert_null(clients[0].handle);
}
} // namespace

extern "C" int ove_test_cpp_mqtt_init(ove_mqtt_client_t *handle, ove_mqtt_client_storage_t *storage)
{
	for (auto &client : clients) {
		if (client.handle)
			continue;
		std::memset(storage, 0, sizeof(*storage));
		client.handle = storage;
		*handle = storage;
		return OVE_OK;
	}
	return OVE_ERR_NO_MEMORY;
}

#ifndef CONFIG_OVE_ZERO_HEAP
extern "C" int ove_test_cpp_mqtt_create(ove_mqtt_client_t *handle)
{
	for (auto &client : clients)
		if (!client.handle)
			return ove_test_cpp_mqtt_init(handle, &client.storage);
	return OVE_ERR_NO_MEMORY;
}
#endif

extern "C" void ove_test_cpp_mqtt_deinit(ove_mqtt_client_t handle)
{
	if (auto *client = find_client(handle)) {
		client->handle = nullptr;
		client->connected = false;
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
extern "C" void ove_test_cpp_mqtt_destroy(ove_mqtt_client_t handle)
{
	ove_test_cpp_mqtt_deinit(handle);
}
#endif

extern "C" int ove_test_cpp_mqtt_connect(ove_mqtt_client_t handle, const ove_mqtt_config_t *config)
{
	auto *client = find_client(handle);
	if (!client || !config)
		return OVE_ERR_INVALID_PARAM;
	if (client->connected)
		return OVE_ERR_BUSY;
	client->config = *config;
	client->connected = true;
	return OVE_OK;
}

int test_cpp_net_mqtt_run(void)
{
	const CMUnitTest tests[] = {
		cmocka_unit_test_setup(test_cpp_mqtt_callbacks_are_per_client, setup),
		cmocka_unit_test_setup(test_cpp_mqtt_failed_connect_keeps_callback, setup),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
