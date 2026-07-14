/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * STM32F746 hardware RNG adapter. The Cube polling API has a separate 2 ms
 * timeout for every 32-bit word, so a hostile 4 KiB request could accumulate
 * seconds of coordinator work. This adapter polls the peripheral under one
 * deadline for the complete request and checks the clock/seed health flags
 * before consuming each word.
 */

#include "bsp.h"
#include "ove/types.h"

#include <stdint.h>
#include <string.h>

#define BSP_RANDOM_MAX_BYTES 4096u
#define BSP_RANDOM_DEADLINE_MS 3u
#define BSP_RANDOM_POLL_SPIN_LIMIT 1000000u

static RNG_HandleTypeDef g_rng;
static int g_rng_ready;
static uint32_t g_last_word;
static int g_last_word_valid;

/* Verify the clock tree rather than assuming SystemClock_Config stayed intact.
 * RNG/SDMMC require CLK48 <= 48 MHz; this board intentionally supplies exactly
 * 48 MHz from HSE(25 MHz) / PLLM(25) * PLLN(432) / PLLQ(9). */
static int rng_clock_valid(void)
{
	if ((RCC->CR & RCC_CR_PLLRDY) == 0u ||
	    (RCC->DCKCFGR2 & RCC_DCKCFGR2_CK48MSEL) != 0u)
		return 0;

	uint32_t pll = RCC->PLLCFGR;
	uint32_t m = pll & RCC_PLLCFGR_PLLM;
	uint32_t n = (pll & RCC_PLLCFGR_PLLN) >> RCC_PLLCFGR_PLLN_Pos;
	uint32_t q = (pll & RCC_PLLCFGR_PLLQ) >> RCC_PLLCFGR_PLLQ_Pos;
	uint32_t source = (pll & RCC_PLLCFGR_PLLSRC) ? HSE_VALUE : HSI_VALUE;
	if (m == 0u || q == 0u)
		return 0;
	uint64_t hz = ((uint64_t)source * n) / ((uint64_t)m * q);
	return hz == 48000000u;
}

int bsp_random_init(void)
{
	g_rng_ready = 0;
	g_last_word_valid = 0;
	if (!rng_clock_valid())
		return OVE_ERR_BUS_ERROR;

	__HAL_RCC_RNG_CLK_ENABLE();
	__HAL_RCC_RNG_FORCE_RESET();
	__DSB();
	__HAL_RCC_RNG_RELEASE_RESET();
	memset(&g_rng, 0, sizeof(g_rng));
	g_rng.Instance = RNG;
	if (HAL_RNG_Init(&g_rng) != HAL_OK) {
		__HAL_RCC_RNG_CLK_DISABLE();
		return OVE_ERR_BUS_ERROR;
	}
	g_rng_ready = 1;
	return OVE_OK;
}

static int random_fail(void *buf, size_t len, int error)
{
	/* Do not leave a partly refreshed buffer looking successful to a caller that
	 * mishandles the error. This wipe is itself bounded by BSP_RANDOM_MAX_BYTES. */
	memset(buf, 0, len);
	return error;
}

int bsp_random_fill(void *buf, size_t len)
{
	if ((!buf && len != 0u) || len > BSP_RANDOM_MAX_BYTES)
		return OVE_ERR_INVALID_PARAM;
	if (len == 0u)
		return OVE_OK;
	if (!g_rng_ready)
		return OVE_ERR_NOT_REGISTERED;
	if (!rng_clock_valid())
		return random_fail(buf, len, OVE_ERR_BUS_ERROR);

	uint8_t *out = buf;
	size_t done = 0;
	uint32_t started = HAL_GetTick();
	uint32_t spins = 0;
	unsigned seed_restarts = 0;
	while (done < len) {
		uint32_t sr = RNG->SR;

		/* A clock error invalidates future output. A seed error invalidates even
		 * an apparently-ready DR word; discard it and perform Cube's documented
		 * disable/enable recovery once within this request. */
		if ((sr & (RNG_SR_CECS | RNG_SR_CEIS)) != 0u) {
			__HAL_RNG_CLEAR_IT(&g_rng, RNG_IT_CEI);
			return random_fail(buf, len, OVE_ERR_BUS_ERROR);
		}
		if ((sr & (RNG_SR_SECS | RNG_SR_SEIS)) != 0u) {
			if (seed_restarts++ != 0u)
				return random_fail(buf, len, OVE_ERR_BUS_ERROR);
			__HAL_RNG_CLEAR_IT(&g_rng, RNG_IT_SEI);
			__HAL_RNG_DISABLE(&g_rng);
			__DSB();
			__HAL_RNG_ENABLE(&g_rng);
			continue;
		}

		if ((sr & RNG_SR_DRDY) != 0u) {
			uint32_t word = RNG->DR;
			/* Continuous health check: never expose two identical consecutive
			 * 32-bit blocks. A false positive is fail-closed (probability 2^-32). */
			if (g_last_word_valid && word == g_last_word)
				return random_fail(buf, len, OVE_ERR_BUS_ERROR);
			g_last_word = word;
			g_last_word_valid = 1;
			size_t chunk = len - done;
			if (chunk > sizeof(word))
				chunk = sizeof(word);
			memcpy(out + done, &word, chunk);
			done += chunk;
			continue;
		}

		/* HAL_GetTick is wrap-safe here. The spin ceiling remains a second,
		 * independent bound if the system tick is unexpectedly stopped. */
		spins++;
		if ((uint32_t)(HAL_GetTick() - started) >= BSP_RANDOM_DEADLINE_MS ||
		    spins >= BSP_RANDOM_POLL_SPIN_LIMIT)
			return random_fail(buf, len, OVE_ERR_TIMEOUT);
	}
	return OVE_OK;
}
