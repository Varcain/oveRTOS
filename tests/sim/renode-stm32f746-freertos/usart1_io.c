/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Hardware-target stdio for the STM32F746G-Discovery test firmware.
 *
 * Compiled into the test ELF when CMake is invoked with -DOVE_HW=ON
 * (the HW runner sets this).  Renode builds keep `semihosting_io.c`
 * instead — Renode 1.16's CPU semihosting handler captures stdout via
 * the SemihostingUart we attach in test.resc, so no real UART is
 * needed there.  On real silicon there's no semihosting; we re-target
 * newlib's `_write` to the Discovery's USART1 (the on-board ST-Link
 * VCP), which the HW runner reads via pyserial at 115200 8N1.
 *
 * This file deliberately does NOT depend on FreeRTOS — `_write` may
 * be called from `printf` paths that run before the scheduler starts
 * (e.g. an early failure during board init).  We use a plain blocking
 * HAL_UART_Transmit and skip the production board's mutex layer.
 *
 * Pin map (from boards/stm32f746g-discovery/freertos/src/serial_wrapper.c):
 *   USART1 TX → PA9   (AF7)
 *   USART1 RX → PB7   (AF7) — wired but unused; we don't read.
 */

#include "stm32f7xx_hal.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

static UART_HandleTypeDef s_console_uart;
static int s_console_ready;

static void usart1_console_init(void)
{
	if (s_console_ready) {
		return;
	}

	__HAL_RCC_USART1_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	GPIO_InitTypeDef gpio = {0};
	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_HIGH;
	gpio.Alternate = GPIO_AF7_USART1;

	gpio.Pin = GPIO_PIN_9;
	HAL_GPIO_Init(GPIOA, &gpio);
	gpio.Pin = GPIO_PIN_7;
	HAL_GPIO_Init(GPIOB, &gpio);

	s_console_uart.Instance = USART1;
	s_console_uart.Init.BaudRate = 115200;
	s_console_uart.Init.WordLength = UART_WORDLENGTH_8B;
	s_console_uart.Init.StopBits = UART_STOPBITS_1;
	s_console_uart.Init.Parity = UART_PARITY_NONE;
	s_console_uart.Init.Mode = UART_MODE_TX_RX;
	s_console_uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	s_console_uart.Init.OverSampling = UART_OVERSAMPLING_16;

	if (HAL_UART_Init(&s_console_uart) != HAL_OK) {
		for (;;) {
		}
	}
	s_console_ready = 1;
}

/* Override newlib's `_write`.  CR is injected before LF to match the
 * production board's behaviour and to keep raw `screen`/`minicom`
 * sessions readable. */
int _write(int fd, const char *buf, int len)
{
	(void)fd;
	if (!s_console_ready) {
		usart1_console_init();
	}
	for (int i = 0; i < len; ++i) {
		if (buf[i] == '\n') {
			static const uint8_t cr = '\r';
			HAL_UART_Transmit(&s_console_uart, (uint8_t *)&cr, 1, 1000);
		}
		HAL_UART_Transmit(&s_console_uart, (uint8_t *)&buf[i], 1, 1000);
	}
	return len;
}

/* `_sbrk` — same rationale as in semihosting_io.c.  Without rdimon's
 * librdimon.a the linker pulls in newlib's stub `_sbrk` that always
 * returns -ENOMEM; provide a real one backed by the linker-reserved
 * `_end` → stack-bottom region. */
extern char _end;
extern char _estack;

#ifndef OVE_HW_HEAP_STACK_RESERVE
#define OVE_HW_HEAP_STACK_RESERVE (8 * 1024)
#endif

void *_sbrk(int incr)
{
	static char *heap_ptr = NULL;
	if (heap_ptr == NULL) {
		heap_ptr = &_end;
	}
	char *heap_limit = &_estack - OVE_HW_HEAP_STACK_RESERVE;
	if (heap_ptr + incr > heap_limit) {
		errno = ENOMEM;
		return (void *)-1;
	}
	char *prev = heap_ptr;
	heap_ptr += incr;
	return prev;
}

/* `stub_board.c::ove_hal_board_init` references `stub_gpio_reset()`
 * which previously lived in `stub_gpio.c` (now removed).  Provide a
 * no-op shim — same as semihosting_io.c does. */
void stub_gpio_reset(void);
void stub_gpio_reset(void)
{
}
