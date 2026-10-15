/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Console UART for QEMU MPS2-AN500: CMSDK APB UART0, which QEMU connects to
 * its stdio (-nographic, or -serial stdio). RX is interrupt driven into a
 * ring; TX is polled, since QEMU drains the one-byte holding register as soon
 * as its backend takes the byte.
 */

#include "serial_wrapper.h"
#include "board_desc.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

#define UART0_BASE 0x40004000u
#define UART_REG(off) (*(volatile uint32_t *)(uintptr_t)(UART0_BASE + (off)))
#define UART_DATA UART_REG(0x00u)
#define UART_STATE UART_REG(0x04u)
#define UART_CTRL UART_REG(0x08u)
#define UART_INTSTATUS UART_REG(0x0cu)
#define UART_BAUDDIV UART_REG(0x10u)

#define UART_STATE_TXFULL (1u << 0)
#define UART_STATE_RXFULL (1u << 1)
#define UART_STATE_RXOVR (1u << 3)
#define UART_CTRL_TX_EN (1u << 0)
#define UART_CTRL_RX_EN (1u << 1)
#define UART_CTRL_RX_INTEN (1u << 3)
#define UART_INT_RX (1u << 1)

/* UART0 RX is NVIC line 0 on this machine (TX is line 1). */
#define UART0_RX_IRQN 0u
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define NVIC_IPR ((volatile uint8_t *)0xE000E400u)

#define CIRC_BUFF_SIZE OVE_SERIAL_CONSOLE_RX_BUFFER_SIZE
#define CIRC_BUFF_MASK (CIRC_BUFF_SIZE - 1U)
/* TX polls a register QEMU drains immediately; a backend that stops taking
 * output (a pty nobody reads) makes the write give up instead of hanging. */
#define SERIAL_TX_SPIN_LIMIT 100000U
#define SERIAL_LOCK_MS 1000U

static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;

static unsigned char circBuff[CIRC_BUFF_SIZE];
static volatile unsigned int head;
static volatile unsigned int tail;
static ove_console_ready_fn rx_ready_callback;
static const void *rx_ready_context;

static int serial_put_byte(unsigned char c)
{
	for (unsigned int spin = 0; UART_STATE & UART_STATE_TXFULL; spin++) {
		if (spin == SERIAL_TX_SPIN_LIMIT)
			return 0;
	}
	UART_DATA = c;
	return 1;
}

void serial_init(void)
{
	/* QEMU rejects a divisor below 16 as an invalid baud rate. */
	UART_BAUDDIV = 16u;
	UART_CTRL = UART_CTRL_TX_EN | UART_CTRL_RX_EN | UART_CTRL_RX_INTEN;

	/* The ready callback may give a FreeRTOS semaphore, so keep the line at
	 * or below the kernel-call ceiling. */
	NVIC_IPR[UART0_RX_IRQN] = configMAX_SYSCALL_INTERRUPT_PRIORITY;
	NVIC_ISER0 = 1u << UART0_RX_IRQN;

	mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
}

void serial_write(const unsigned char *data, unsigned int length)
{
	if (data == NULL || length == 0 || mutex == NULL) {
		return;
	}

	TickType_t lock_ticks = pdMS_TO_TICKS(SERIAL_LOCK_MS);
	if (lock_ticks == 0)
		lock_ticks = 1;
	if (xSemaphoreTake(mutex, lock_ticks) != pdTRUE)
		return;

	for (unsigned int i = 0; i < length; i++) {
		if (data[i] == '\n' && !serial_put_byte('\r'))
			break;
		if (!serial_put_byte(data[i]))
			break;
	}

	xSemaphoreGive(mutex);
}

void serial_safe_write(const char *str, unsigned int len)
{
	if (str == NULL) {
		return;
	}
	for (unsigned int i = 0; i < len; i++) {
		if (!serial_put_byte((unsigned char)str[i]))
			return;
	}
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

int serial_rx_ready(void)
{
	return (head != tail) ? 1 : 0;
}

void serial_set_rx_ready_callback(ove_console_ready_fn callback, const void *context)
{
	taskENTER_CRITICAL();
	rx_ready_context = context;
	rx_ready_callback = callback;
	taskEXIT_CRITICAL();
}

void UART0RX_IRQHandler(void)
{
	int published = 0;

	/* The RX interrupt latches until cleared; reading DATA lets QEMU hand
	 * over the next byte, possibly before this loop checks RXFULL again. */
	for (;;) {
		UART_INTSTATUS = UART_INT_RX;
		if (!(UART_STATE & UART_STATE_RXFULL))
			break;
		unsigned char c = (unsigned char)(UART_DATA & 0xffu);
		unsigned int nextHead = (head + 1) & CIRC_BUFF_MASK;
		if (nextHead != tail) {
			circBuff[head] = c;
			head = nextHead;
			published = 1;
		}
	}
	UART_STATE = UART_STATE_RXOVR;

	if (published && rx_ready_callback)
		rx_ready_callback(rx_ready_context);
}
