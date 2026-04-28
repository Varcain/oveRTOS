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
 * Override newlib's _sbrk (normally provided by rdimon.specs's
 * librdimon.a, which asks the host for a heap block via SYS_HEAPINFO —
 * another call Renode 1.16.x doesn't implement).  Back it with the
 * linker-reserved `_end` → stack-bottom region instead.
 *
 * The STM32F746NGHx linker script already defines:
 *   _end            — first free byte after BSS
 *   _estack         — top-of-stack (grows down)
 * We use the range [_end, _estack - 8 KB) as the malloc arena, leaving
 * headroom for the stack that newlib/rdimon can't normally see.  Small
 * enough for cmocka (~64 KB usage) plus a safety margin.
 */

#include <errno.h>

extern char _end;
extern char _estack;

#ifndef RENODE_HEAP_STACK_RESERVE
#define RENODE_HEAP_STACK_RESERVE (8 * 1024)
#endif

void *_sbrk(int incr)
{
	static char *heap_ptr = NULL;
	if (heap_ptr == NULL) {
		heap_ptr = &_end;
	}
	char *heap_limit = &_estack - RENODE_HEAP_STACK_RESERVE;

	if (heap_ptr + incr > heap_limit) {
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
