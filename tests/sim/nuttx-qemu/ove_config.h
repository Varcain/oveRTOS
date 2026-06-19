/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_CONFIG_NUTTX_QEMU_H
#define OVE_CONFIG_NUTTX_QEMU_H

/*
 * arm-QEMU (mps2-an500, Cortex-M7) heap-mode config: the shared NuttX heap
 * config plus MPU-backed fault containment. Kept distinct from the shared
 * tests/sim/nuttx/ove_config.h so CONFIG_OVE_PROTECTED is scoped to this
 * Cortex-M target only — the x86 NuttX-sim and the Renode stm32f746 variants
 * keep falling back to the shared config and stay unaffected.
 */
#include "../nuttx/ove_config.h"

#define CONFIG_OVE_PROTECTED 1

#endif /* OVE_CONFIG_NUTTX_QEMU_H */
