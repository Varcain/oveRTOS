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

/*
 * Override newlib/picolibc's _sbrk.  The linker reserves an explicit
 * `.heap` section sized by `__heap_size` (set via -Wl,--defsym in
 * this CMakeLists); we hand out `[__heap_start, __heap_end)` to the
 * libc nano-malloc.  The `.heap` section lives in SDRAM rather than
 * main SRAM — the test binary's BSS exhausts main SRAM, and Renode
 * models SDRAM out-of-the-box.  No more `_end` / `_estack` distance
 * arithmetic (which conflated heap headroom with stack headroom).
 */

#include <errno.h>

extern char __heap_start;
extern char __heap_end;

void *_sbrk(int incr)
{
	static char *heap_ptr = NULL;
	if (heap_ptr == NULL) {
		heap_ptr = &__heap_start;
	}
	if (heap_ptr + incr > &__heap_end) {
		errno = 12; /* ENOMEM */
		return (void *)-1;
	}
	char *prev = heap_ptr;
	heap_ptr += incr;
	return prev;
}

/* `stub_board.c::ove_hal_board_init` calls `stub_gpio_reset()`, which
 * previously lived in `stub_gpio.c`.  We dropped the stub GPIO so the
 * real `freertos_gpio.c` can drive Renode's STM32_GPIOPort models —
 * provide a no-op shim here so the board init still links. */
void stub_gpio_reset(void);
void stub_gpio_reset(void)
{
}
