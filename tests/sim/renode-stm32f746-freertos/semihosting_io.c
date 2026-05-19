/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Renode-compatible semihosting stdio.
 *
 * Renode 1.16.x's ARM Cortex-M semihosting handler implements
 * SYS_WRITEC (0x03), SYS_WRITE0 (0x04), SYS_EXIT (0x18), and a handful
 * of others — but NOT SYS_WRITE (0x05), the call that newlib's
 * rdimon-backed `_write` emits.  A firmware linked with
 * `--specs=rdimon.specs` that calls printf/fprintf/putchar therefore
 * produces no output under Renode.
 *
 * Workaround: override `_write` to emit one byte at a time via
 * SYS_WRITEC, which Renode supports.  It's slow but perfectly fine for
 * test-console throughput (CMocka summaries, a few hundred KB at most).
 *
 * SYS_WRITEC (0x03): r1 points to a single char to write.
 *   AAPCS: r0 = op, r1 = param; bkpt 0xAB executes the call.
 *
 * This file is only linked into the Renode test firmware; production
 * and QEMU builds keep newlib's unmodified `_write`.
 */

#include <stdint.h>
#include <unistd.h>

#define SYS_WRITEC 0x03

static inline void sh_writec(char c)
{
	register uintptr_t r0 __asm__("r0") = SYS_WRITEC;
	register uintptr_t r1 __asm__("r1") = (uintptr_t)&c;
	__asm__ volatile("bkpt #0xAB" : : "r"(r0), "r"(r1) : "memory");
}

/* Override newlib's _write.  Returns bytes written (or -1 on error,
 * which we never signal — Renode's SemihostingUart can't fail here). */
int _write(int fd, const char *buf, int len)
{
	(void)fd; /* stdout + stderr both go to the semihosting UART */
	for (int i = 0; i < len; ++i) {
		sh_writec(buf[i]);
	}
	return len;
}

/* `_sbrk` is intentionally not provided here.  picolibc's nano-malloc
 * is bypassed by backends/freertos/freertos_libc_malloc.c, which wraps
 * malloc/free/calloc/realloc onto pvPortMalloc/vPortFree (FreeRTOS
 * heap_4 ucHeap).  Without those wrappers picolibc would call _sbrk
 * during its own malloc path; with them the symbol is never referenced
 * and the linker drops it.  See the "Single-heap policy" comment in
 * boards/stm32f746g-discovery/freertos/STM32F746NGHx_FLASH.ld. */

/* `stub_board.c::ove_hal_board_init` calls `stub_gpio_reset()`, which
 * previously lived in `stub_gpio.c`.  We dropped the stub GPIO so the
 * real `freertos_gpio.c` can drive Renode's STM32_GPIOPort models —
 * provide a no-op shim here so the board init still links. */
void stub_gpio_reset(void);
void stub_gpio_reset(void)
{
}
