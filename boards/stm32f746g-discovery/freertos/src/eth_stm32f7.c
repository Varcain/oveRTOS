/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * STM32F746G-Discovery Ethernet driver for lwIP (HALv2 API).
 *
 * Uses the STM32 HAL ETH v2 API with the callback-based Rx path.
 * Provides ethernetif_init / ethernetif_input for lwIP integration.
 *
 * Pin mapping (STM32F746G-Discovery, RMII):
 *   PA1  RMII_REF_CLK    PA2  RMII_MDIO     PA7  RMII_CRS_DV
 *   PC1  RMII_MDC        PC4  RMII_RXD0     PC5  RMII_RXD1
 *   PG11 RMII_TX_EN      PG13 RMII_TXD0     PG14 RMII_TXD1
 */

#include "stm32f7xx_hal.h"
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#include <string.h>

/* ── Configuration ───────────────────────────────────────────── */

#ifndef ETH_RX_DESC_CNT
#define ETH_RX_DESC_CNT  4
#endif
#ifndef ETH_TX_DESC_CNT
#define ETH_TX_DESC_CNT  4
#endif
#define ETH_RX_BUF_SIZE  1536

/* LAN8742A PHY */
#define LAN8742A_ADDR    0
#define PHY_BSR          0x01
#define PHY_BSR_LINK     0x0004

/* ── DMA descriptors and buffers ─────────────────────────────── */

ETH_HandleTypeDef heth;
ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT]
    __attribute__((aligned(4)));
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT]
    __attribute__((aligned(4)));

static uint8_t RxBuff[ETH_RX_DESC_CNT][ETH_RX_BUF_SIZE]
    __attribute__((aligned(4)));

static uint8_t MACAddr[6] = {0x02, 0x00, 0x00, 0xDE, 0xAD, 0x01};

/* ── GPIO init ───────────────────────────────────────────────── */

static void eth_gpio_init(void)
{
	GPIO_InitTypeDef gpio = {0};

	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOC_CLK_ENABLE();
	__HAL_RCC_GPIOG_CLK_ENABLE();

	gpio.Mode = GPIO_MODE_AF_PP;
	gpio.Pull = GPIO_NOPULL;
	gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	gpio.Alternate = GPIO_AF11_ETH;

	gpio.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7;
	HAL_GPIO_Init(GPIOA, &gpio);

	gpio.Pin = GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5;
	HAL_GPIO_Init(GPIOC, &gpio);

	gpio.Pin = GPIO_PIN_11 | GPIO_PIN_13 | GPIO_PIN_14;
	HAL_GPIO_Init(GPIOG, &gpio);

	__HAL_RCC_ETH_CLK_ENABLE();
}

void HAL_ETH_MspInit(ETH_HandleTypeDef *h)
{
	(void)h;
	eth_gpio_init();
}

/* ── HAL Rx callbacks ────────────────────────────────────────── */

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
	static int alloc_idx;
	*buff = RxBuff[alloc_idx];
	alloc_idx = (alloc_idx + 1) % ETH_RX_DESC_CNT;
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd,
			    uint8_t *buff, uint16_t Length)
{
	struct pbuf **ppStart = (struct pbuf **)pStart;
	struct pbuf *p;

	p = pbuf_alloc(PBUF_RAW, Length, PBUF_POOL);
	if (p != NULL) {
		pbuf_take(p, buff, Length);
		p->next = NULL;
	}

	if (!*ppStart) {
		*ppStart = p;
	} else if (p) {
		struct pbuf *tail = (struct pbuf *)*pEnd;
		if (tail) tail->next = p;
	}
	*pEnd = p;

	if (*ppStart) {
		uint16_t total = 0;
		for (struct pbuf *q = *ppStart; q; q = q->next)
			total += q->len;
		for (struct pbuf *q = *ppStart; q; q = q->next) {
			q->tot_len = total;
			total -= q->len;
		}
	}
}

/* ── MAC init ────────────────────────────────────────────────── */

static int eth_mac_init(void)
{
	heth.Instance = ETH;
	heth.Init.MACAddr = MACAddr;
	heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
	heth.Init.TxDesc = DMATxDscrTab;
	heth.Init.RxDesc = DMARxDscrTab;
	heth.Init.RxBuffLen = ETH_RX_BUF_SIZE;

	if (HAL_ETH_Init(&heth) != HAL_OK)
		return -1;

	/* Assign Rx buffer addresses (DESC2 = Buffer1 in legacy format) */
	for (int i = 0; i < ETH_RX_DESC_CNT; i++) {
		DMARxDscrTab[i].DESC2 = (uint32_t)RxBuff[i];
		DMARxDscrTab[i].BackupAddr0 = (uint32_t)RxBuff[i];
	}

	/* Wait for PHY link */
	for (int i = 0; i < 50; i++) {
		uint32_t bsr = 0;
		if (HAL_ETH_ReadPHYRegister(&heth, LAN8742A_ADDR,
					    PHY_BSR, &bsr) == HAL_OK) {
			if (bsr & PHY_BSR_LINK) return 0;
		}
		HAL_Delay(100);
	}
	return 0; /* continue even without link */
}

/* ── lwIP netif: low-level output ─────────────────────────────── */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	(void)netif;
	ETH_BufferTypeDef tx_buf;
	ETH_TxPacketConfigTypeDef tx_cfg;

	memset(&tx_buf, 0, sizeof(tx_buf));
	memset(&tx_cfg, 0, sizeof(tx_cfg));

	tx_buf.buffer = p->payload;
	tx_buf.len = p->tot_len;
	tx_buf.next = NULL;

	tx_cfg.Length = p->tot_len;
	tx_cfg.TxBuffer = &tx_buf;

	if (HAL_ETH_Transmit(&heth, &tx_cfg, 100) != HAL_OK)
		return ERR_IF;

	return ERR_OK;
}

/* ── Public: lwIP netif init ─────────────────────────────────── */

err_t ethernetif_init(struct netif *netif)
{
#if LWIP_NETIF_HOSTNAME
	netif->hostname = "overtos";
#endif
	netif->name[0] = 's';
	netif->name[1] = 't';
	netif->output = etharp_output;
	netif->linkoutput = low_level_output;

	netif->hwaddr_len = ETH_HWADDR_LEN;
	memcpy(netif->hwaddr, MACAddr, ETH_HWADDR_LEN);
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
		       NETIF_FLAG_LINK_UP;

	if (eth_mac_init() != 0)
		return ERR_IF;

	HAL_ETH_Start(&heth);
	return ERR_OK;
}

/* ── Public: poll for received frames ────────────────────────── */

void ethernetif_input(struct netif *netif)
{
	struct pbuf *p = NULL;

	if (HAL_ETH_ReadData(&heth, (void **)&p) == HAL_OK) {
		if (p != NULL) {
			if (netif->input(p, netif) != ERR_OK) {
				pbuf_free(p);
			}
		}
	}

	/* Kick the DMA Rx if it's suspended (buffer unavailable) */
	if (heth.Instance->DMASR & ETH_DMASR_RBUS) {
		heth.Instance->DMASR = ETH_DMASR_RBUS;
		heth.Instance->DMARPDR = 0;
	}
}
