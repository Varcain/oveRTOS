/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * CMSIS Cortex-M7 startup file for CMSDK_CM7 (MPS2-AN500).
 * Derived from ARM CMSIS_5 Device pack.
 *
 * Vector table + Reset_Handler that copies .data, zeros .bss,
 * calls SystemInit(), then main().
 */

    .syntax unified
    .arch   armv7-m

/* ======================================================================== */
/* Stack / Heap                                                              */
/* ======================================================================== */
    .section .stack
    .align  3
    .globl  __StackTop
    .globl  __StackLimit
__StackLimit:
    .space  0x2000          /* 8 KB stack */
__StackTop:
    .size   __StackLimit, . - __StackLimit

    /* No `.section .heap` here.  Picolibc provides its own sbrk
     * (libc/picolib/picosbrk.c) that uses the linker script's
     * __heap_start / __heap_end symbols (PROVIDE'd in mps2_an500.ld).
     * The CMSIS template shipped a 256 KB .heap reservation here
     * for newlib-libgloss-style _sbrk in syscalls.c — that path is
     * gone with the picolibc migration, so the reservation is too. */

/* ======================================================================== */
/* Vector Table                                                              */
/* ======================================================================== */
    .section .isr_vector, "a"
    .align  2
    .globl  __isr_vector
__isr_vector:
    .long   __StackTop              /* Top of Stack */
    .long   Reset_Handler           /* Reset Handler */
    .long   NMI_Handler
    .long   HardFault_Handler
    .long   MemManage_Handler
    .long   BusFault_Handler
    .long   UsageFault_Handler
    .long   0                       /* Reserved */
    .long   0
    .long   0
    .long   0
    .long   SVC_Handler
    .long   DebugMon_Handler
    .long   0                       /* Reserved */
    .long   PendSV_Handler
    .long   SysTick_Handler

    /* External Interrupts (IRQ 0..31) */
    .long   Default_Handler         /*  0: UART0 */
    .long   Default_Handler         /*  1: UART1 */
    .long   Default_Handler         /*  2: TIMER0 */
    .long   Default_Handler         /*  3: TIMER1 */
    .long   Default_Handler         /*  4: DUALTIMER */
    .long   Default_Handler         /*  5: SPI */
    .long   Default_Handler         /*  6: UARTOVF */
    .long   Default_Handler         /*  7: ETHERNET */
    .long   Default_Handler         /*  8: I2S */
    .long   Default_Handler         /*  9: TSC */
    .long   Default_Handler         /* 10: PORT0_ALL */
    .long   Default_Handler         /* 11: PORT1_ALL */
    .long   Default_Handler         /* 12: DMA_ERROR */
    .long   Default_Handler         /* 13: DMA_TC */
    .long   Default_Handler         /* 14: DMA_COMB */
    .long   Default_Handler         /* 15: PORT0_0 */
    .long   Default_Handler         /* 16: PORT0_1 */
    .long   Default_Handler         /* 17: PORT0_2 */
    .long   Default_Handler         /* 18: PORT0_3 */
    .long   Default_Handler         /* 19: PORT0_4 */
    .long   Default_Handler         /* 20: PORT0_5 */
    .long   Default_Handler         /* 21: PORT0_6 */
    .long   Default_Handler         /* 22: PORT0_7 */
    .long   Default_Handler         /* 23: PORT0_8 */
    .long   Default_Handler         /* 24: PORT0_9 */
    .long   Default_Handler         /* 25: PORT0_10 */
    .long   Default_Handler         /* 26: PORT0_11 */
    .long   Default_Handler         /* 27: PORT0_12 */
    .long   Default_Handler         /* 28: PORT0_13 */
    .long   Default_Handler         /* 29: PORT0_14 */
    .long   Default_Handler         /* 30: PORT0_15 */
    .long   Default_Handler         /* 31: SysTick_combined */

    .size   __isr_vector, . - __isr_vector

/* ======================================================================== */
/* Reset Handler                                                             */
/* ======================================================================== */
    .text
    .thumb
    .thumb_func
    .align  2
    .globl  Reset_Handler
    .type   Reset_Handler, %function
Reset_Handler:
    /* Enable FPU (CP10/CP11 full access) */
    ldr     r0, =0xE000ED88       /* CPACR */
    ldr     r1, [r0]
    orr     r1, r1, #(0xF << 20)  /* CP10 + CP11 = full access */
    str     r1, [r0]
    dsb
    isb

    /* Copy .data from flash to RAM */
    ldr     r0, =__etext
    ldr     r1, =__data_start__
    ldr     r2, =__data_end__
.Lcopy_data:
    cmp     r1, r2
    bge     .Lzero_bss
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       .Lcopy_data

    /* Zero .bss */
.Lzero_bss:
    ldr     r0, =__bss_start__
    ldr     r1, =__bss_end__
    movs    r2, #0
.Lzero_loop:
    cmp     r0, r1
    bge     .Lcall_main
    str     r2, [r0], #4
    b       .Lzero_loop

.Lcall_main:
    bl      SystemInit
    /* Picolibc + --oslib=semihost --crt0=hosted handles semihosting stdio
     * setup implicitly via _start in libpicolibc.a; no equivalent of
     * newlib/libgloss's initialise_monitor_handles is needed. */
    bl      __libc_init_array   /* C++ static constructors */
    bl      main
    b       .

    .size   Reset_Handler, . - Reset_Handler

/* ======================================================================== */
/* Default exception / interrupt handlers (weak)                             */
/* ======================================================================== */
    .macro  def_irq_handler handler_name
    .weak   \handler_name
    .type   \handler_name, %function
\handler_name:
    b       .
    .size   \handler_name, . - \handler_name
    .endm

    def_irq_handler NMI_Handler
    def_irq_handler HardFault_Handler
    def_irq_handler MemManage_Handler
    def_irq_handler BusFault_Handler
    def_irq_handler UsageFault_Handler
    def_irq_handler SVC_Handler
    def_irq_handler DebugMon_Handler
    def_irq_handler PendSV_Handler
    def_irq_handler SysTick_Handler
    def_irq_handler Default_Handler

    .end
