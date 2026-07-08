/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F7 Ethernet MAC driver for lwIP (HALv2 API).
 *
 * Uses the STM32 HAL ETH v2 API with the callback-based Rx path.
 * Provides ethernetif_init / ethernetif_input for lwIP integration.
 *
 * The board must provide HAL_ETH_MspInit() (weak override) to configure
 * the RMII/MII GPIO pins and enable the ETH clock.
 *
 * Cache coherency (D-cache runs ON — the personality needs it): the ETH
 * DMA and the M7 D-cache are kept coherent without detuning the cache or
 * spending a scarce MPU region (the FreeRTOS-MPU port claims all 8):
 *   - DMA descriptor tables live in DTCM (.eth_desc, 0x20000000).  DTCM is
 *     architecturally never cached yet is reachable by the ETH DMA via the
 *     Cortex-M7 AHBS slave port (ST AN4839), so the descriptors — which the
 *     HAL builds and hands to the DMA internally, with no hook to clean the
 *     cache in between — are inherently coherent.
 *   - RX buffers stay in cacheable SRAM; the CPU invalidates each frame's
 *     range before reading it (HAL_ETH_RxLinkCallback), so pbuf_take copies
 *     the DMA-written data, not a stale cache line.
 *   - TX payloads (lwIP pbufs in cacheable memory) are cleaned to SRAM
 *     before HAL_ETH_Transmit so the DMA reads the CPU-written frame.
 */

#include "stm32f7xx_hal.h"
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#include "FreeRTOS.h"
#include "task.h"
#include "ove_config.h"

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

/* LAN8742A PHY (datasheet Table 6) */
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
/* PHY Special Control/Status Register: bits[4:2] = HCDSPEED (auto-neg result) */
#define LAN8742A_PHYSCSR 0x1F
#define LAN8742A_HCDSPEED_MASK 0x001CU
#define LAN8742A_HCDSPEED_10HD 0x0004U
#define LAN8742A_HCDSPEED_100HD 0x0008U
#define LAN8742A_HCDSPEED_10FD 0x0014U
#define LAN8742A_HCDSPEED_100FD 0x0018U

/* ── DMA descriptors and buffers ─────────────────────────────── */

/* Descriptors in uncached DTCM (see file header) — coherent with no cache
 * maintenance.  4 RX + 4 TX × sizeof(ETH_DMADescTypeDef)=40 B = exactly the
 * 0x140 .eth_desc window carved from the DTCM base in the linker script. */
ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((section(".eth_desc")));
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((section(".eth_desc")));

/* RX buffers stay in cacheable SRAM; 32-byte aligned (== a D-cache line) so a
 * per-frame SCB_InvalidateDCache_by_Addr can never straddle into a neighbour. */
static uint8_t RxBuff[ETH_RX_DESC_CNT][ETH_RX_BUF_SIZE] __attribute__((aligned(32)));

ETH_HandleTypeDef heth;

static uint8_t MACAddr[6] = {0x02, 0x00, 0x00, 0xDE, 0xAD, 0x01};

/* ── HAL Rx callbacks ────────────────────────────────────────── */

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
	static int alloc_idx;
	*buff = RxBuff[alloc_idx];
	alloc_idx = (alloc_idx + 1) % ETH_RX_DESC_CNT;
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
	struct pbuf **ppStart = (struct pbuf **)pStart;
	struct pbuf *p;

	p = pbuf_alloc(PBUF_RAW, Length, PBUF_POOL);
	if (p != NULL) {
		/* The DMA wrote this frame into cacheable SRAM behind the CPU's back;
		 * drop any stale cache lines so pbuf_take copies the fresh bytes.  buff
		 * is 32-byte aligned and the CPU never writes RxBuff, so invalidating
		 * the frame's range can neither straddle a neighbour nor lose data. */
		SCB_InvalidateDCache_by_Addr((uint32_t *)buff, (int32_t)Length);
		pbuf_take(p, buff, Length);
		p->next = NULL;
	}

	if (!*ppStart) {
		*ppStart = p;
	} else if (p) {
		struct pbuf *tail = (struct pbuf *)*pEnd;
		if (tail)
			tail->next = p;
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

	for (unsigned int i = 0; i < ETH_RX_DESC_CNT; i++) {
		DMARxDscrTab[i].DESC2 = (uint32_t)RxBuff[i];
		DMARxDscrTab[i].BackupAddr0 = (uint32_t)RxBuff[i];
	}

	/* Wait for PHY link + ANEG complete (up to 5 s) */
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

	/* Drive MAC speed/duplex from the PHY's resolved state. HAL
	 * defaults to 100M/full; if the link partner negotiated something
	 * different (e.g. 10M half on a slow switch) the MAC silently
	 * corrupts every frame. PHYSCSR bits[4:2] hold the post-ANEG
	 * highest-common-denominator. */
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

	return 0; /* continue even without link */
}

/* ── lwIP netif: low-level output ─────────────────────────────── */

#define TX_MAX_SEGMENTS 8

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	(void)netif;
	ETH_BufferTypeDef tx_buf[TX_MAX_SEGMENTS] = {0};
	ETH_TxPacketConfigTypeDef tx_cfg = {0};
	int n = 0;

	/* Walk the pbuf chain — pre-existing driver passed only the head
	 * with Length=tot_len, which lied to the MAC about the buffer
	 * size when pbufs were chained. */
	for (struct pbuf *q = p; q != NULL && n < TX_MAX_SEGMENTS; q = q->next) {
		tx_buf[n].buffer = q->payload;
		tx_buf[n].len = q->len;
		/* Flush the CPU-written frame out of the D-cache so the DMA, which
		 * reads this payload straight from SRAM, sees the real bytes and not
		 * a not-yet-written-back cache line.  Clean (vs invalidate) is
		 * non-destructive, so an unaligned pbuf payload is safe. */
		SCB_CleanDCache_by_Addr((uint32_t *)q->payload, (int32_t)q->len);
		if (n > 0)
			tx_buf[n - 1].next = &tx_buf[n];
		n++;
	}
	if (n == 0)
		return ERR_BUF;

	tx_cfg.Length = p->tot_len;
	tx_cfg.TxBuffer = tx_buf;

	if (HAL_ETH_Transmit(&heth, &tx_cfg, 100) != HAL_OK)
		return ERR_IF;

	return ERR_OK;
}

/* ── RMII errata workaround (STM32F746 rev 0x1000) ─────────────
 *
 * Early STM32F7 silicon (rev A = 0x1000, used on the F746G-Discovery)
 * has an RMII state-machine errata where the interface fails to lock
 * at startup — link is up at the PHY but every transmitted frame is
 * corrupted (FCS / alignment / code errors at the peer, observed as
 * rx_pkts == rx_fcs in `ethtool -S` on the receiving host).
 *
 * Workaround mirrors the official ST `LwIP_HTTP_Server_Netconn_RTOS`
 * demo: poll MMC counters, and if CRC errors keep accumulating without
 * any good unicast received, toggle SYSCFG_PMC.MII_RMII_SEL to reset
 * the RMII state machine. Self-terminates once good frames flow. */
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
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

	if (eth_mac_init() != 0)
		return ERR_IF;

	HAL_ETH_Start(&heth);

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
