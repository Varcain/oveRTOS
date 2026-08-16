/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_PROTECTED)

#include "ove/protected.h"
#include "ove_protected_nuttx.h"

#include <nuttx/irq.h> /* irq_attach, xcpt_t; pulls arch/irq.h for REG_PC/REG_XPSR */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> /* abort */

/*
 * NuttX / ARMv7-M (Cortex-M) fault containment — Level 1.
 *
 * This generic CONFIG_OVE_PROTECTED backend keeps an ordinary NuttX FLAT task
 * privileged and provides Level-1 fault recovery around one no-access guard; it
 * does not implement per-task address spaces. Do not confuse it with the Linux
 * personality's specialized LXP NuttX port: that port sets CONTROL.nPRIV on every
 * guest and swaps complete per-program MPU views on context switches, still in
 * BUILD_FLAT. Here, the no-access region traps a stray load/store into MemManage,
 * which we recover from rather than panicking — the on-target analog of the host
 * backend's PROT_NONE page + SIGSEGV handler.
 *
 * Mechanism (public NuttX APIs + architectural registers only):
 *  - We program the ARMv7-M MPU and System Control Block directly. NuttX's
 *    flat build does not enable the MPU, so we own it; PRIVDEFENA keeps the
 *    default memory map live for every privileged access except the single
 *    guard region we mark no-access, leaving the rest of the system untouched.
 *    The MPU is enabled only for the duration of a run, so it has no residual
 *    effect on other on-target tests (e.g. the loader executing from RAM).
 *  - MemManage is enabled (SHCSR.MEMFAULTENA) and reaches us through NuttX's
 *    standard exception path (exception_common -> arm_doirq -> irq_dispatch),
 *    so irq_attach(MemManage) installs our handler in place of the panicking
 *    default.
 *  - On a contained fault the handler rewrites the saved exception frame's PC
 *    to a recovery thunk; NuttX's exception return resumes thread mode there
 *    and the thunk longjmp()s back into ove_ptask_run(). This is the same
 *    saved-frame rewrite NuttX itself uses to deliver signals.
 *
 * Single-run: the jump buffer, armed flag and guard region are global, so a
 * protected run must not overlap another (matches the host backend's
 * documented restriction).
 */

/* ARMv7-M System Control Space — fixed addresses (Armv7-M ARM, B3.4 / B3.5).
 * NuttX keeps these in arch-private headers (arch/arm/src/armv7-m/{nvic,mpu}.h)
 * that are not on the application include path, so we restate the few we need. */
#define SCS_BASE 0xe000e000u
#define REG_SHCSR (*(volatile uint32_t *)(SCS_BASE + 0x0d24u)) /* sys handler ctrl/state */
#define REG_CFSR (*(volatile uint32_t *)(SCS_BASE + 0x0d28u))  /* config fault status */
#define SHCSR_MEMFAULTENA (1u << 16)
#define CFSR_MMFSR_MASK 0x000000ffu /* low byte = MemManage fault status (W1C) */

#define MPU_BASE 0xe000ed90u
#define REG_MPU_CTRL (*(volatile uint32_t *)(MPU_BASE + 0x04u))
#define REG_MPU_RNR (*(volatile uint32_t *)(MPU_BASE + 0x08u))
#define REG_MPU_RBAR (*(volatile uint32_t *)(MPU_BASE + 0x0cu))
#define REG_MPU_RASR (*(volatile uint32_t *)(MPU_BASE + 0x10u))
#define MPU_CTRL_ENABLE (1u << 0)
#define MPU_CTRL_PRIVDEFENA (1u << 2)
#define MPU_RASR_ENABLE (1u << 0)
#define MPU_RASR_AP_NONO (0u << 24)		 /* no access, privileged or unprivileged */
#define MPU_RASR_SIZE(log2) (((log2) - 1u) << 1) /* region size = 2^log2 bytes */

/* ARMv7-M MemManage exception/IRQ number (== NuttX's internal NVIC_IRQ_MEMFAULT). */
#define OVE_IRQ_MEMFAULT 4

/* Guard region: 32 bytes, naturally aligned (the ARMv7-M minimum region size).
 * This standalone protected-mode test owns the MPU and may use region 7;
 * linux_interop's separate flat-build seam uses it for copied text. */
#define GUARD_REGION 7u
#define GUARD_SIZE 32u
#define GUARD_LOG2 5u /* 2^5 == 32 */

static uint8_t g_guard[GUARD_SIZE] __attribute__((aligned(GUARD_SIZE)));

static jmp_buf g_jmp;
static volatile int g_armed;
static volatile unsigned long g_fault_count;
static int g_inited;

static inline void dsb_isb(void)
{
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
}

/* Resumed in thread mode (via the rewritten exception-return PC) after a
 * contained fault; unwinds back to ove_ptask_run(). */
static void ove_ptask_recover(void)
{
	longjmp(g_jmp, 1);
}

/* A MemManage outside an armed run is a genuine bug — nothing but a protected
 * task should ever touch the guard region. Fail loudly rather than spin on the
 * un-acknowledged fault. */
static void ove_ptask_unexpected(void)
{
	abort();
}

static int ove_memfault_handler(int irq, void *context, void *arg)
{
	uint32_t *regs = (uint32_t *)context;
	(void)irq;
	(void)arg;

	/* Acknowledge the MemManage fault (write-1-clear its status byte). */
	REG_CFSR = REG_CFSR & CFSR_MMFSR_MASK;

	if (g_armed) {
		g_armed = 0;
		g_fault_count++;
		regs[REG_PC] = (uint32_t)&ove_ptask_recover & ~1u;
	} else {
		regs[REG_PC] = (uint32_t)&ove_ptask_unexpected & ~1u;
	}
	regs[REG_XPSR] |= (1u << 24); /* keep Thumb state on exception return */
	return 0;
}

static void ove_ptask_init(void)
{
	/* Program the no-access guard region (left disabled until a run). */
	REG_MPU_RNR = GUARD_REGION;
	REG_MPU_RBAR = (uint32_t)g_guard & ~(GUARD_SIZE - 1u);
	REG_MPU_RASR = MPU_RASR_AP_NONO | MPU_RASR_SIZE(GUARD_LOG2) | MPU_RASR_ENABLE;

	/* Enable MemManage and replace the panicking default handler. */
	REG_SHCSR |= SHCSR_MEMFAULTENA;
	dsb_isb();
	irq_attach(OVE_IRQ_MEMFAULT, ove_memfault_handler, NULL);

	g_inited = 1;
}

static inline void mpu_enable(void)
{
	REG_MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
	dsb_isb();
}

static inline void mpu_disable(void)
{
	REG_MPU_CTRL = 0u;
	dsb_isb();
}

int ove_ptask_run(ove_ptask_fn entry, void *arg, ove_ptask_result_t *result)
{
	if (!entry)
		return OVE_ERR_INVALID_PARAM;

	if (!g_inited)
		ove_ptask_init();

	mpu_enable();

	ove_ptask_result_t r;
	if (setjmp(g_jmp) == 0) {
		g_armed = 1;
		entry(arg);
		g_armed = 0;
		r = OVE_PTASK_OK;
	} else {
		/* Resumed here from ove_ptask_recover() via longjmp. */
		r = OVE_PTASK_FAULT;
	}
	g_armed = 0;

	mpu_disable();

	if (result)
		*result = r;
	return OVE_OK;
}

unsigned long ove_ptask_fault_count(void)
{
	return g_fault_count;
}

const void *ove_nuttx_ptask_guarded_region(size_t *size)
{
	if (size)
		*size = GUARD_SIZE;
	return g_guard;
}

#endif /* CONFIG_OVE_PROTECTED */
