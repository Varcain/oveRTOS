/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "serial_wrapper.h"
#include "stm32f7xx_hal.h"
#include "board_desc.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdint.h>

#define CIRC_BUFF_SIZE OVE_SERIAL_CONSOLE_RX_BUFFER_SIZE
#define CIRC_BUFF_MASK (CIRC_BUFF_SIZE - 1U)

static UART_HandleTypeDef uartHandle;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;

static unsigned char circBuff[CIRC_BUFF_SIZE];
static volatile unsigned int head;
static volatile unsigned int tail;

void serial_init(void)
{
	GPIO_InitTypeDef gpioInit;

	__HAL_RCC_USART1_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	gpioInit.Alternate = GPIO_AF7_USART1;
	gpioInit.Mode = GPIO_MODE_AF_PP;
	gpioInit.Pin = GPIO_PIN_9;
	gpioInit.Pull = GPIO_NOPULL;
	gpioInit.Speed = GPIO_SPEED_HIGH;
	HAL_GPIO_Init(GPIOA, &gpioInit);

	gpioInit.Pin = GPIO_PIN_7;
	HAL_GPIO_Init(GPIOB, &gpioInit);

	uartHandle.Instance = USART1;
	uartHandle.Init.BaudRate = OVE_SERIAL_CONSOLE_BAUD;
	uartHandle.Init.WordLength = UART_WORDLENGTH_8B;
	uartHandle.Init.StopBits = UART_STOPBITS_1;
	uartHandle.Init.Parity = UART_PARITY_NONE;
	uartHandle.Init.Mode = UART_MODE_TX_RX;
	uartHandle.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	uartHandle.Init.OverSampling = UART_OVERSAMPLING_16;

	if (HAL_UART_Init(&uartHandle) != HAL_OK) {
		while (1) {
		}
	}

	HAL_NVIC_SetPriority(USART1_IRQn, 2, 0);
	HAL_NVIC_EnableIRQ(USART1_IRQn);
	__HAL_UART_ENABLE_IT(&uartHandle, UART_IT_RXNE);

	mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
}

void serial_write(const unsigned char *data, unsigned int length)
{
	unsigned int start;
	unsigned int i;
	static const uint8_t cr = '\r';

	if (data == NULL || length == 0 || mutex == NULL) {
		return;
	}

	xSemaphoreTake(mutex, portMAX_DELAY);

	/* Translate \n to \r\n on output */
	start = 0;
	for (i = 0; i < length; i++) {
		if (data[i] == '\n') {
			if (i > start) {
				HAL_UART_Transmit(&uartHandle, (uint8_t *)&data[start], i - start,
						  1000);
			}
			HAL_UART_Transmit(&uartHandle, (uint8_t *)&cr, 1, 1000);
			HAL_UART_Transmit(&uartHandle, (uint8_t *)&data[i], 1, 1000);
			start = i + 1;
		}
	}
	if (start < length) {
		HAL_UART_Transmit(&uartHandle, (uint8_t *)&data[start], length - start, 1000);
	}

	xSemaphoreGive(mutex);
}

/* Safe UART write for exception handlers - no FreeRTOS APIs */
void serial_safe_write(const char *str, unsigned int len)
{
	if (str == NULL || len == 0) {
		return;
	}

	HAL_UART_Transmit(&uartHandle, (uint8_t *)str, len, 1000);
}

unsigned char serial_getChar(void)
{
	unsigned char c = 0;
	unsigned int localTail = tail;

	if (localTail != head) {
		c = circBuff[localTail];
		tail = (localTail + 1) & CIRC_BUFF_MASK;
	}

	return c;
}

void USART1_IRQHandler(void)
{
	unsigned int nextHead;

	if (__HAL_UART_GET_FLAG(&uartHandle, UART_FLAG_RXNE)) {
		nextHead = (head + 1) & CIRC_BUFF_MASK;

		if (nextHead != tail) {
			circBuff[head] = (unsigned char)(uartHandle.Instance->RDR & 0xFFU);
			head = nextHead;
		} else {
			volatile unsigned char dummy =
				(unsigned char)(uartHandle.Instance->RDR & 0xFFU);
			(void)dummy;
		}

		__HAL_UART_CLEAR_OREFLAG(&uartHandle);
	}
}

/* ---- polled USART1 console for the Linux personality (see serial_wrapper.h) ---- */
void serial_poll_begin(void)
{
	if (mutex == NULL) /* serial_init() creates `mutex` last — call it once to bring USART1 up */
		serial_init();
	/* Hand the receiver to polled access: the personality reads RXNE/RDR directly from the
	 * svc-exception context, so the IRQ-driven RX must not consume bytes ahead of it. */
	__HAL_UART_DISABLE_IT(&uartHandle, UART_IT_RXNE);
	HAL_NVIC_DisableIRQ(USART1_IRQn);
}

int serial_poll_rx_ready(void)
{
	return __HAL_UART_GET_FLAG(&uartHandle, UART_FLAG_RXNE) ? 1 : 0;
}

int serial_poll_getc(void)
{
	return (int)(uartHandle.Instance->RDR & 0xFFU);
}

void serial_poll_putc(char c)
{
	while (!__HAL_UART_GET_FLAG(&uartHandle, UART_FLAG_TXE)) {
	}
	uartHandle.Instance->TDR = (unsigned char)c;
}
