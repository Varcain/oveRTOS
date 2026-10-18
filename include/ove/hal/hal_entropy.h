/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_ENTROPY_H
#define OVE_HAL_ENTROPY_H

/**
 * @defgroup ove_hal_entropy HAL entropy source
 * @brief Random bytes for seeding cryptography (the TLS layer's DRBG).
 * Each engine or board supplies it: the STM32 hardware RNG on the F746, the
 * kernel's random pool where the RTOS has one. The QEMU boards have no RNG
 * and supply a test-only source that says so at first use.
 * @{
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fill @p buf with @p len random bytes. OVE_OK or a negative error. */
int ove_hal_entropy_fill(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_ENTROPY_H */
