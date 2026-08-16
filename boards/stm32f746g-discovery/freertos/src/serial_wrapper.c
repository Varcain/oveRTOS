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
#include "ove_config.h" /* CONFIG_OVE_STACK_CANARIES — gates __stack_chk_fail below */
#include "FreeRTOS.h"
#include "semphr.h"
#include <stdint.h>

#define CIRC_BUFF_SIZE OVE_SERIAL_CONSOLE_RX_BUFFER_SIZE
#define CIRC_BUFF_MASK (CIRC_BUFF_SIZE - 1U)
#define SERIAL_TX_BUDGET_MS 1000U

static UART_HandleTypeDef uartHandle;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;

static unsigned char circBuff[CIRC_BUFF_SIZE];
static volatile unsigned int head;
static volatile unsigned int tail;

static uint32_t serial_tx_remaining(uint32_t started)
{
	uint32_t elapsed = HAL_GetTick() - started; /* unsigned subtraction is wrap-safe */
	return elapsed < SERIAL_TX_BUDGET_MS ? SERIAL_TX_BUDGET_MS - elapsed : 0U;
}

static int serial_tx_chunk(const uint8_t *data, unsigned int length, uint32_t started)
{
	while (length != 0U) {
		uint16_t chunk = length > 0xffffU ? 0xffffU : (uint16_t)length;
		uint32_t remaining = serial_tx_remaining(started);
		if (remaining == 0U ||
		    HAL_UART_Transmit(&uartHandle, (uint8_t *)data, chunk, remaining) != HAL_OK)
			return 0;
		data += chunk;
		length -= chunk;
	}
	return 1;
}

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

	uint32_t started = HAL_GetTick();
	TickType_t lock_ticks = pdMS_TO_TICKS(SERIAL_TX_BUDGET_MS);
	if (lock_ticks == 0)
		lock_ticks = 1;
	if (xSemaphoreTake(mutex, lock_ticks) != pdTRUE)
		return;

	/* Translate \n to \r\n on output. Every chunk shares one deadline: a buffer
	 * containing many newlines cannot multiply the HAL timeout indefinitely. */
	start = 0;
	for (i = 0; i < length; i++) {
		if (data[i] == '\n') {
			if (i > start) {
				if (!serial_tx_chunk(&data[start], i - start, started))
					goto out;
			}
			if (!serial_tx_chunk(&cr, 1, started) ||
			    !serial_tx_chunk(&data[i], 1, started))
				goto out;
			start = i + 1;
		}
	}
	if (start < length) {
		(void)serial_tx_chunk(&data[start], length - start, started);
	}

out:
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

/* Fatal host-fault diagnostic supplied to LXP's FreeRTOS port by the oveRTOS host binding. Runs
 * in fault context after the port decides a fault belongs to host/privileged code and cannot be
 * contained, so it pokes USART1 directly — no HAL (its
 * tick-based timeout is frozen with interrupts masked), no FreeRTOS, no VFP (general-regs-only, so
 * a pending lazy-FP stack to an invalid frame can't nest another fault) — then halts. A watchdog,
 * if armed, reboots; R8's reset-cause read then reports it. */
#define FAULT_GPR __attribute__((target("general-regs-only")))
static FAULT_GPR void fault_putc(char c)
{
	while (!(USART1->ISR & USART_ISR_TXE)) {
	}
	USART1->TDR = (uint8_t)c;
}
static FAULT_GPR void fault_puts(const char *s)
{
	while (*s)
		fault_putc(*s++);
}
static FAULT_GPR void fault_puthex(uint32_t v)
{
	for (int i = 28; i >= 0; i -= 4)
		fault_putc("0123456789abcdef"[(v >> i) & 0xfu]);
}

void ove_freertos_lxp_host_fatal(uint32_t cfsr, uint32_t hfsr, uint32_t pc);

FAULT_GPR void ove_freertos_lxp_host_fatal(uint32_t cfsr, uint32_t hfsr, uint32_t pc)
{
	fault_puts("\n!!! HOST FAULT (privileged/host context - not a guest, cannot contain)\n!!! pc=0x");
	fault_puthex(pc);
	fault_puts(" cfsr=0x");
	fault_puthex(cfsr);
	fault_puts(" hfsr=0x");
	fault_puthex(hfsr);
	fault_puts("\n!!! host state compromised - halting; watchdog will reset\n");
	for (;;) {
	}
}

#if defined(CONFIG_OVE_STACK_CANARIES)
/* -fstack-protector-strong calls this when a function's stack canary was overwritten: the host
 * stack is corrupted, so — like a host fault — it is not recoverable. no_stack_protector so a
 * smashed frame in the handler itself cannot recurse. Unlike the seam's fault handler this runs in
 * THREAD mode (an ordinary call from the smashed function's epilogue), so a bare spin would halt
 * only this task while the scheduler kept the rest of a compromised system running — mask
 * interrupts first so the halt is system-wide. The IWDG (independent LSI clock) still expires and
 * reboots. */
__attribute__((no_stack_protector)) void __stack_chk_fail(void)
{
	fault_puts("\n!!! STACK SMASH detected (host stack corrupted) - halting; watchdog will reset\n");
	__disable_irq();
	for (;;) {
	}
}
#endif
#undef FAULT_GPR

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

int serial_rx_ready(void)
{
	return (head != tail) ? 1 : 0;
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
