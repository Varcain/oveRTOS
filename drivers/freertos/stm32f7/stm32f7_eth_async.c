/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F7 Ethernet MAC transport for embassy-net.
 *
 * Kept parallel to stm32f7_eth.c (the lwIP backend): the MAC/PHY init,
 * speed/duplex resolution, and RMII errata workaround are identical;
 * the difference is the RX path uses a fixed-size byte-buffer queue
 * instead of allocating lwIP pbufs, and the API matches
 * qemu_net_async.h so the Rust QemuShmDriver pattern carries over.
 *
 * If you fix a bug in the init / RMII / PHY logic here, also fix it
 * in stm32f7_eth.c.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_ASYNC_NET

#include "stm32f7_eth_async.h"
#include "stm32f7xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"
#include "ove/types.h"

#include <string.h>

/* ── Configuration ───────────────────────────────────────────── */

#ifndef ETH_RX_DESC_CNT
#define ETH_RX_DESC_CNT 4
#endif
#ifndef ETH_TX_DESC_CNT
#define ETH_TX_DESC_CNT 4
#endif
#ifndef ETH_RX_BUF_SIZE
#define ETH_RX_BUF_SIZE 1536
#endif

#define LAN8742A_ADDR 0
#ifndef PHY_BSR
#define PHY_BSR 0x01
#endif
#ifndef PHY_BSR_LINK
#define PHY_BSR_LINK 0x0004
#endif
#ifndef PHY_BSR_ANEG_DONE
#define PHY_BSR_ANEG_DONE 0x0020
#endif
#define LAN8742A_PHYSCSR 0x1F
#define LAN8742A_HCDSPEED_MASK 0x001CU
#define LAN8742A_HCDSPEED_10HD 0x0004U
#define LAN8742A_HCDSPEED_100HD 0x0008U
#define LAN8742A_HCDSPEED_10FD 0x0014U
#define LAN8742A_HCDSPEED_100FD 0x0018U

/* ── DMA descriptors and buffers (parallel to stm32f7_eth.c) ── */

ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((aligned(4)));
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((aligned(4)));

static uint8_t RxBuff[ETH_RX_DESC_CNT][ETH_RX_BUF_SIZE] __attribute__((aligned(4)));

ETH_HandleTypeDef heth;
static uint8_t s_mac[6];

/* ── ISR notify hook ───────────────────────────────────────────
 *
 * ETH_IRQHandler dispatches to HAL_ETH_IRQHandler which calls the
 * HAL_ETH_*CpltCallback weak symbols. We override them and fire a
 * user-registered notify cb (typically an AtomicWaker::wake() shim
 * on the Rust side) so the embassy-net runner re-polls receive(). */

typedef void (*eth_notify_cb)(void *ud);
static volatile eth_notify_cb s_notify_cb;
static volatile void *s_notify_ud;

void ove_stm32f7_eth_async_set_notify(eth_notify_cb cb, void *ud)
{
	s_notify_ud = ud;
	__atomic_store_n((void *volatile *)&s_notify_cb, (void *)cb, __ATOMIC_RELEASE);
}

static inline void fire_notify(void)
{
	eth_notify_cb cb =
		(eth_notify_cb)__atomic_load_n((void *volatile *)&s_notify_cb, __ATOMIC_ACQUIRE);
	if (cb != NULL)
		cb((void *)s_notify_ud);
}

/* ── RX queue ────────────────────────────────────────────────
 *
 * HAL_ETH_RxLinkCallback fires synchronously inside HAL_ETH_ReadData
 * with a (buf, len) pair for each received frame. We push that onto
 * a small SPSC queue; the Rust receive() function calls _rx() which
 * drives ReadData + pops one entry. */

struct rx_entry {
	uint8_t *buf;
	uint16_t len;
};

#define RX_QUEUE_CAP ETH_RX_DESC_CNT
static struct rx_entry s_rx_q[RX_QUEUE_CAP];
static volatile uint32_t s_rx_head; /* producer */
static volatile uint32_t s_rx_tail; /* consumer */

/* ── HAL Rx callbacks ────────────────────────────────────────── */

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
	static int alloc_idx;
	*buff = RxBuff[alloc_idx];
	alloc_idx = (alloc_idx + 1) % ETH_RX_DESC_CNT;
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
	(void)pStart;
	(void)pEnd;
	uint32_t head = s_rx_head;
	uint32_t next = (head + 1) % RX_QUEUE_CAP;
	if (next == s_rx_tail) {
		/* Queue full — drop the frame; descriptor is recycled by
		 * HAL_ETH_BuildRxDescriptors on the next ReadData. */
		return;
	}
	s_rx_q[head].buf = buff;
	s_rx_q[head].len = Length;
	__atomic_store_n(&s_rx_head, next, __ATOMIC_RELEASE);
}

/* ── ETH ISR + HAL completion callbacks ──────────────────────── */

void ETH_IRQHandler(void)
{
	HAL_ETH_IRQHandler(&heth);
}

void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *h)
{
	(void)h;
	fire_notify();
}

void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *h)
{
	(void)h;
	fire_notify();
}

void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *h)
{
	(void)h;
	fire_notify();
}

/* ── MAC init (parallel to stm32f7_eth.c::eth_mac_init) ──────── */

static int eth_mac_init(void)
{
	heth.Instance = ETH;
	heth.Init.MACAddr = s_mac;
	heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
	heth.Init.TxDesc = DMATxDscrTab;
	heth.Init.RxDesc = DMARxDscrTab;
	heth.Init.RxBuffLen = ETH_RX_BUF_SIZE;

	if (HAL_ETH_Init(&heth) != HAL_OK)
		return OVE_ERR_BUS_ERROR;

	for (unsigned int i = 0; i < ETH_RX_DESC_CNT; i++) {
		DMARxDscrTab[i].DESC2 = (uint32_t)RxBuff[i];
		DMARxDscrTab[i].BackupAddr0 = (uint32_t)RxBuff[i];
	}

	/* PHY autoneg (up to 5 s) — same window as the lwIP path. */
	uint32_t bsr = 0;
	int linked = 0;
	for (int i = 0; i < 50; i++) {
		if (HAL_ETH_ReadPHYRegister(&heth, LAN8742A_ADDR, PHY_BSR, &bsr) == HAL_OK &&
		    (bsr & PHY_BSR_LINK) && (bsr & PHY_BSR_ANEG_DONE)) {
			linked = 1;
			break;
		}
		HAL_Delay(100);
	}

	if (linked) {
		uint32_t scsr = 0;
		uint32_t speed = ETH_SPEED_100M;
		uint32_t duplex = ETH_FULLDUPLEX_MODE;
		if (HAL_ETH_ReadPHYRegister(&heth, LAN8742A_ADDR, LAN8742A_PHYSCSR, &scsr) ==
		    HAL_OK) {
			switch (scsr & LAN8742A_HCDSPEED_MASK) {
			case LAN8742A_HCDSPEED_10HD:
				speed = ETH_SPEED_10M;
				duplex = ETH_HALFDUPLEX_MODE;
				break;
			case LAN8742A_HCDSPEED_100HD:
				speed = ETH_SPEED_100M;
				duplex = ETH_HALFDUPLEX_MODE;
				break;
			case LAN8742A_HCDSPEED_10FD:
				speed = ETH_SPEED_10M;
				duplex = ETH_FULLDUPLEX_MODE;
				break;
			case LAN8742A_HCDSPEED_100FD:
			default:
				break;
			}
		}
		ETH_MACConfigTypeDef macconf = {0};
		HAL_ETH_GetMACConfig(&heth, &macconf);
		macconf.Speed = speed;
		macconf.DuplexMode = duplex;
		HAL_ETH_SetMACConfig(&heth, &macconf);
	}
	return OVE_OK;
}

/* ── RMII errata workaround (parallel to stm32f7_eth.c) ───────── */

static void rmii_watchdog_task(void *arg)
{
	(void)arg;
	for (;;) {
		if (heth.Instance->MMCRGUFCR > 0U) {
			vTaskDelete(NULL);
		} else if (heth.Instance->MMCRFCECR > 10U) {
			SYSCFG->PMC &= ~SYSCFG_PMC_MII_RMII_SEL;
			SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;
			heth.Instance->MMCCR |= ETH_MMCCR_CR;
		} else {
			vTaskDelay(pdMS_TO_TICKS(200));
		}
	}
}

/* ── Public API ──────────────────────────────────────────────── */

int ove_stm32f7_eth_async_init(const uint8_t mac[6])
{
	memcpy(s_mac, mac, 6);
	s_rx_head = 0;
	s_rx_tail = 0;

	int ret = eth_mac_init();
	if (ret != OVE_OK)
		return ret;

	HAL_ETH_Start(&heth);

	/* Enable ETH IRQ so RX completions wake the embassy-net runner
	 * without polling. Priority chosen to be ≥
	 * configMAX_SYSCALL_INTERRUPT_PRIORITY so FreeRTOS API calls from
	 * the user notify cb (e.g. ove_event_signal_from_isr inside an
	 * AtomicWaker::wake()) are legal. */
	HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(ETH_IRQn);

	if (HAL_GetREVID() == 0x1000U) {
#ifdef CONFIG_OVE_ZERO_HEAP
		static StaticTask_t s_rmii_wd_tcb;
		static StackType_t s_rmii_wd_stack[256];
		xTaskCreateStatic(rmii_watchdog_task, "rmii_wd", 256, NULL, 4, s_rmii_wd_stack,
				  &s_rmii_wd_tcb);
#else
		xTaskCreate(rmii_watchdog_task, "rmii_wd", 256, NULL, 4, NULL);
#endif
	}
	return OVE_OK;
}

int ove_stm32f7_eth_async_tx(const void *frame, uint32_t len)
{
	if (frame == NULL || len == 0)
		return OVE_ERR_INVALID_PARAM;

	ETH_BufferTypeDef tx_buf = {0};
	ETH_TxPacketConfigTypeDef tx_cfg = {0};
	tx_buf.buffer = (uint8_t *)(uintptr_t)frame;
	tx_buf.len = len;
	tx_cfg.Length = len;
	tx_cfg.TxBuffer = &tx_buf;

	if (HAL_ETH_Transmit(&heth, &tx_cfg, 100) != HAL_OK)
		return OVE_ERR_BUS_ERROR;
	return OVE_OK;
}

int ove_stm32f7_eth_async_rx(void *buf, uint32_t buf_size, uint32_t *out_len)
{
	void *dummy = NULL;
	/* Advances DMA descriptors, runs the Allocate + Link callbacks
	 * which push frames onto s_rx_q. Returns HAL_ERROR when no frame
	 * is currently ready. */
	(void)HAL_ETH_ReadData(&heth, &dummy);

	/* Kick DMA if it suspended (rx buffer unavailable) — same logic
	 * as the lwIP backend. */
	if (heth.Instance->DMASR & ETH_DMASR_RBUS) {
		heth.Instance->DMASR = ETH_DMASR_RBUS;
		heth.Instance->DMARPDR = 0;
	}

	uint32_t head = __atomic_load_n(&s_rx_head, __ATOMIC_ACQUIRE);
	if (s_rx_tail == head)
		return OVE_ERR_NOT_FOUND;

	struct rx_entry *e = &s_rx_q[s_rx_tail];
	if (e->len > buf_size) {
		/* Drop oversize. */
		s_rx_tail = (s_rx_tail + 1) % RX_QUEUE_CAP;
		return OVE_ERR_NO_MEMORY;
	}
	memcpy(buf, e->buf, e->len);
	*out_len = e->len;
	s_rx_tail = (s_rx_tail + 1) % RX_QUEUE_CAP;
	return OVE_OK;
}

int ove_stm32f7_eth_async_link_up(void)
{
	uint32_t bsr = 0;
	if (HAL_ETH_ReadPHYRegister(&heth, LAN8742A_ADDR, PHY_BSR, &bsr) != HAL_OK)
		return 0;
	return (bsr & PHY_BSR_LINK) ? 1 : 0;
}

#endif /* CONFIG_OVE_ASYNC_NET */
