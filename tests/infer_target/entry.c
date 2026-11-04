/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

#ifdef OVE_QUALIFY_CMSIS_NN
#include "arm_nn_math_types.h"
#if !defined(__arm__) || !defined(ARM_MATH_DSP)
#error "CMSIS qualification requires the actual ARM DSP path"
#endif
#endif

#if configSUPPORT_DYNAMIC_ALLOCATION
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(8)));
#endif

extern void ove_main(void);
extern void xPortSysTickHandler(void);

/* Keep the board's startup and MSP; qualify before SIM or scheduler startup. */
int main(void)
{
	ove_main();
	return 1;
}

void SysTick_Handler(void)
{
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
		xPortSysTickHandler();
}
