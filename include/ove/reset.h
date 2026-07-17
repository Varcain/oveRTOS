/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file reset.h
 * @defgroup ove_reset Reset cause
 * @ingroup ove_core
 * @brief Why the MCU last reset.
 *
 * Reads the SoC's reset-cause latch so a watchdog recovery is visible in the
 * log instead of looking like a spontaneous reboot. Ships with the watchdog:
 * meaningful only where a hardware reset latch and the watchdog exist (STM32
 * IWDG today), and reports @ref OVE_RESET_UNKNOWN everywhere else.
 *
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 * @{
 */

#ifndef OVE_RESET_H
#define OVE_RESET_H

#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Why the MCU last came out of reset. */
typedef enum ove_reset_cause {
	OVE_RESET_UNKNOWN = 0, /**< no latch, or a cause not distinguished here */
	OVE_RESET_POWER_ON,	 /**< power-on / power-down reset (cold boot) */
	OVE_RESET_PIN,	   /**< external reset pin (NRST) */
	OVE_RESET_SOFTWARE,	 /**< software-requested reset (SYSRESETREQ) */
	OVE_RESET_WATCHDOG,	 /**< a watchdog timed out — the host stopped feeding */
	OVE_RESET_BROWNOUT,	 /**< brown-out detector tripped */
	OVE_RESET_LOW_POWER, /**< illegal low-power / standby exit */
} ove_reset_cause_t;

/** Human-readable name for @p c (never NULL). */
static inline const char *ove_reset_cause_str(ove_reset_cause_t c)
{
	switch (c) {
	case OVE_RESET_POWER_ON:
		return "power-on";
	case OVE_RESET_PIN:
		return "pin";
	case OVE_RESET_SOFTWARE:
		return "software";
	case OVE_RESET_WATCHDOG:
		return "watchdog";
	case OVE_RESET_BROWNOUT:
		return "brown-out";
	case OVE_RESET_LOW_POWER:
		return "low-power";
	default:
		return "unknown";
	}
}

#ifdef CONFIG_OVE_WATCHDOG

/**
 * @brief The cause of the most recent reset.
 *
 * The latch is read and cleared once at board init, so the value is stable for
 * the life of the boot and reflects the reset that started it. Backend-provided
 * (STM32 reads @c RCC->CSR); returns @ref OVE_RESET_UNKNOWN where no latch is
 * wired.
 *
 * @return the decoded reset cause.
 * @note Requires @c CONFIG_OVE_WATCHDOG.
 */
ove_reset_cause_t ove_reset_cause(void);

#else /* !CONFIG_OVE_WATCHDOG */

static inline ove_reset_cause_t ove_reset_cause(void)
{
	return OVE_RESET_UNKNOWN;
}

#endif /* CONFIG_OVE_WATCHDOG */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_reset group */

#endif /* OVE_RESET_H */
