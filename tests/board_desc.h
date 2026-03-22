/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Test-only board_desc.h — simulates host-pc board for stub testing.
 */

#ifndef OVE_BOARD_DESC_H
#define OVE_BOARD_DESC_H

/* Board identity */
#define OVE_BOARD_NAME       "test-stub"
#define OVE_BOARD_MCU_FAMILY "posix"
#define OVE_BOARD_MCU        "x86_64"

/* GPIO configuration */
#define OVE_GPIO_PORT_COUNT    8
#define OVE_GPIO_PINS_PER_PORT 16

/* LED configuration */
#define OVE_LED_COUNT 8
#define OVE_LED0_PORT       0
#define OVE_LED0_PIN        0
#define OVE_LED0_ACTIVE_LOW 0
#define OVE_LED1_PORT       0
#define OVE_LED1_PIN        1
#define OVE_LED1_ACTIVE_LOW 0
#define OVE_LED2_PORT       0
#define OVE_LED2_PIN        2
#define OVE_LED2_ACTIVE_LOW 0
#define OVE_LED3_PORT       0
#define OVE_LED3_PIN        3
#define OVE_LED3_ACTIVE_LOW 0
#define OVE_LED4_PORT       0
#define OVE_LED4_PIN        4
#define OVE_LED4_ACTIVE_LOW 0
#define OVE_LED5_PORT       0
#define OVE_LED5_PIN        5
#define OVE_LED5_ACTIVE_LOW 0
#define OVE_LED6_PORT       0
#define OVE_LED6_PIN        6
#define OVE_LED6_ACTIVE_LOW 0
#define OVE_LED7_PORT       0
#define OVE_LED7_PIN        7
#define OVE_LED7_ACTIVE_LOW 0

/* Display configuration */
#define OVE_DISPLAY_WIDTH  480
#define OVE_DISPLAY_HEIGHT 272
#define OVE_DISPLAY_COLOR_FORMAT "RGB565"

/* Serial console configuration */
#define OVE_SERIAL_CONSOLE_BAUD 115200
#define OVE_SERIAL_CONSOLE_TX_PIN "none"
#define OVE_SERIAL_CONSOLE_RX_PIN "none"
#define OVE_SERIAL_CONSOLE_RX_BUFFER_SIZE 1024

/* Audio I2S configuration */
#define OVE_AUDIO_I2S_SAMPLE_RATE 44100
#define OVE_AUDIO_I2S_BIT_DEPTH 16
#define OVE_AUDIO_I2S_CHANNELS 1
#define OVE_AUDIO_I2S_BUFFER_SAMPLES 512

/* Runtime board descriptor */
#include "ove/board_types.h"

#ifdef __cplusplus
extern "C" {
#endif

static const struct ove_led_desc ove_board_leds[8] = {
    { 0, 0, 0 },
    { 0, 1, 0 },
    { 0, 2, 0 },
    { 0, 3, 0 },
    { 0, 4, 0 },
    { 0, 5, 0 },
    { 0, 6, 0 },
    { 0, 7, 0 },
};

static const struct ove_board_desc ove_board_descriptor = {
    .name = OVE_BOARD_NAME,
    .mcu_family = OVE_BOARD_MCU_FAMILY,
    .mcu = OVE_BOARD_MCU,
    .gpio_port_count = 8,
    .gpio_pins_per_port = 16,
    .led_count = 8,
    .leds = ove_board_leds,
};

#ifdef __cplusplus
}
#endif

#endif /* OVE_BOARD_DESC_H */
