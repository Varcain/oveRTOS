/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file ove.hpp
 * @brief Umbrella header — includes all oveRTOS C++ wrappers
 */

#pragma once

#include <ove/types.hpp>
#include <ove/sync.hpp>
#include <ove/eventgroup.hpp>
#include <ove/queue.hpp>
#include <ove/timer.hpp>
#include <ove/thread.hpp>
#include <ove/console.hpp>
#include <ove/time.hpp>
#include <ove/nvs.hpp>
#include <ove/shell.hpp>
#include <ove/board.hpp>
#include <ove/gpio.hpp>
#include <ove/led.hpp>
#include <ove/bsp.hpp>
#include <ove/audio.hpp>
#include <ove/watchdog.hpp>
#include <ove/fs.hpp>
#include <ove/stream.hpp>
#include <ove/workqueue.hpp>
#include <ove/app.hpp>
#include <ove/net.hpp>
#include <ove/net_tls.hpp>
#include <ove/net_http.hpp>
#include <ove/net_mqtt.hpp>
#include <ove/net_httpd.hpp>
#include <ove/pm.hpp>
