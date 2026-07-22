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
 * spending another configurable MPU region (the personality already uses all three):
 *   - DMA descriptor tables live in DTCM (.eth_desc, 0x20000000).  DTCM is
 *     architecturally never cached yet is reachable by the ETH DMA via the
 *     Cortex-M7 AHBS slave port (ST AN4839), so the descriptors — which the
 *     HAL builds and hands to the DMA internally, with no hook to clean the
 *     cache in between — are inherently coherent.
 *   - RX buffers stay in cacheable SRAM; the CPU invalidates each frame's
 *     range before reading it (HAL_ETH_RxLinkCallback), so pbuf_take copies
 *     the DMA-written data, not a stale cache line.
 *   - TX payloads are coalesced into the non-cacheable SDRAM bounce buffer below before
 *     HAL_ETH_Transmit, so DMA never observes dirty cacheable pbuf lines.
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

/* Fixed fallback MAC. With CONFIG_OVE_NET_MAC_FROM_UID (default y) eth_mac_init
 * overwrites this with a per-chip, locally-administered address derived from the
 * STM32 UID, so two boards on one LAN cannot collide on this address. */
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

#if defined(CONFIG_OVE_NET_MAC_FROM_UID)
/* Derive a per-chip, locally-administered unicast MAC from the STM32 96-bit
 * unique device ID (0x1FF0F420) so two boards on one LAN cannot collide on the
 * fixed fallback address. The UID is globally unique per chip; take the
 * high-entropy low bytes of word 0 (wafer X/Y + lot) plus a fold of all three
 * words, and set the locally-administered bit (0x02) with the multicast bit
 * clear. Mirrors the NuttX STM32 ethmac driver, which derives its MAC the same
 * way (NuttX/Zephyr already do this natively; only this lwIP driver was fixed). */
static void eth_derive_mac_from_uid(uint8_t mac[6])
{
	const volatile uint32_t *uid = (const volatile uint32_t *)0x1FF0F420u;
	uint32_t w0 = uid[0], w1 = uid[1], w2 = uid[2];
	uint32_t h = w0 ^ w1 ^ w2;
	mac[0] = 0x02; /* locally administered, unicast */
	mac[1] = (uint8_t)(w0 >> 0);
	mac[2] = (uint8_t)(w0 >> 8);
	mac[3] = (uint8_t)(h >> 8);
	mac[4] = (uint8_t)(h >> 16);
	mac[5] = (uint8_t)(h >> 24);
}
#endif

static int eth_mac_init(void)
{
	heth.Instance = ETH;
#if defined(CONFIG_OVE_NET_MAC_FROM_UID)
	eth_derive_mac_from_uid(MACAddr);
#endif
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

/* ETH TX bounce buffer in external SDRAM, ABOVE the guest program pools, in its OWN linker
 * region (.eth_txbuf / ETH_TXBUF @ 0xC07FF800 — see STM32F746NGHx_FLASH.ld).  It is covered by NO
 * MPU region, so it falls to the ARM background map, which types 0xC0000000-0xDFFFFFFF as DEVICE =
 * NON-CACHEABLE in EVERY task context (guest handler + privileged), AND the ETH DMA reaches it
 * directly over the FMC (verified: the DMA reads/writes SDRAM descriptors fine).  low_level_output
 * coalesces each outgoing frame here and transmits from it: with the D-cache ON the lwIP TX pbuf is
 * cacheable SRAM and the per-frame SCB_CleanDCache does NOT reliably reach the ETH DMA on rapid
 * back-to-back sends (proven: the frame ships the PREVIOUS record → TLS bad_record_mac → curl
 * fails; D-cache OFF fixes it but costs ~36% LVGL FPS).  A non-cacheable bounce removes the hazard
 * with the D-cache ON — no MPU-region budget needed (the personality's 3 configurable MPU regions
 * are all taken by the guest).  Byte-wise copy: Device memory forbids unaligned multi-byte
 * accesses.  (Backup SRAM 0x40024000 was tried first — the ETH DMA cannot reach it, board hangs.) */
#define ETH_TX_BOUNCE_MAX 1536u
static uint8_t eth_txbuf[ETH_TX_BOUNCE_MAX] __attribute__((section(".eth_txbuf"), aligned(32)));

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	(void)netif;
	ETH_BufferTypeDef tx_buf = {0};
	ETH_TxPacketConfigTypeDef tx_cfg = {0};
	volatile uint8_t *buf = (volatile uint8_t *)eth_txbuf;

	if (p->tot_len == 0 || p->tot_len > ETH_TX_BOUNCE_MAX)
		return ERR_BUF;

	uint32_t off = 0;
	for (struct pbuf *q = p; q != NULL; q = q->next) {
		const uint8_t *s = (const uint8_t *)q->payload;
		for (uint16_t i = 0; i < q->len; i++)
			buf[off + i] = s[i]; /* byte-wise: Device dest allows only aligned accesses */
		off += q->len;
	}
	__DSB(); /* ensure the Device writes have drained before the DMA reads */

	tx_buf.buffer = eth_txbuf;
	tx_buf.len = off;
	tx_cfg.Length = off;
	tx_cfg.TxBuffer = &tx_buf;

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

	/* eth_mac_init derives MACAddr from the chip UID (CONFIG_OVE_NET_MAC_FROM_UID),
	 * so it must run BEFORE MACAddr is copied into netif->hwaddr below — otherwise
	 * lwIP advertises the stale fallback MAC while the HAL RX filter uses the derived
	 * one, and that mismatch drops all inbound unicast. (Harmless when MACAddr was a
	 * compile-time constant; the runtime derivation makes the ordering matter.) */
	if (eth_mac_init() != 0)
		return ERR_IF;

	netif->hwaddr_len = ETH_HWADDR_LEN;
	memcpy(netif->hwaddr, MACAddr, ETH_HWADDR_LEN);
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

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

/* Drain every frame the DMA has posted this poll cycle (returns the count so the caller can
 * wake the socket coordinator once per batch). Reading a single frame per 1 ms poll capped
 * RX — and TX, since ACKs are the RX that reopens the send window — at ~1 frame/ms ≈ 1.5 MB/s;
 * looping until the ring is empty (the ST LwIP demos' pattern) lets a burst clear in one pass. */
int ethernetif_input(struct netif *netif)
{
	int n = 0;
	struct pbuf *p;

	for (;;) {
		p = NULL;
		if (HAL_ETH_ReadData(&heth, (void **)&p) != HAL_OK || p == NULL)
			break;
		if (netif->input(p, netif) != ERR_OK)
			pbuf_free(p);
		n++;
	}

	/* Kick the DMA Rx if it's suspended (buffer unavailable) */
	if (heth.Instance->DMASR & ETH_DMASR_RBUS) {
		heth.Instance->DMASR = ETH_DMASR_RBUS;
		heth.Instance->DMARPDR = 0;
	}

	return n;
}
