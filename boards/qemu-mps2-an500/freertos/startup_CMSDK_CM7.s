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

    /* External interrupts (IRQ 0..31), as QEMU wires the AN500 */
    .long   UART0RX_IRQHandler      /*  0: UART0 RX */
    .long   Default_Handler         /*  1: UART0 TX */
    .long   Default_Handler         /*  2: UART1 RX */
    .long   Default_Handler         /*  3: UART1 TX */
    .long   Default_Handler         /*  4: UART2 RX */
    .long   Default_Handler         /*  5: UART2 TX */
    .long   Default_Handler         /*  6: (not wired) */
    .long   Default_Handler         /*  7: (not wired) */
    .long   Default_Handler         /*  8: TIMER0 */
    .long   Default_Handler         /*  9: TIMER1 */
    .long   Default_Handler         /* 10: DUALTIMER */
    .long   Default_Handler         /* 11: SPI0/1 */
    .long   Default_Handler         /* 12: UART0-2 overflow */
    .long   Default_Handler         /* 13: Ethernet */
    .long   Default_Handler         /* 14: (not wired) */
    .long   Default_Handler         /* 15: (not wired) */
    .long   Default_Handler         /* 16: (not wired) */
    .long   Default_Handler         /* 17: (not wired) */
    .long   Default_Handler         /* 18: UART3 RX */
    .long   Default_Handler         /* 19: UART3 TX */
    .long   Default_Handler         /* 20: UART4 RX */
    .long   Default_Handler         /* 21: UART4 TX */
    .long   Default_Handler         /* 22: ADC SPI */
    .long   Default_Handler         /* 23: (not wired) */
    .long   Default_Handler         /* 24: Shield SPI */
    .long   Default_Handler         /* 25: (not wired) */
    .long   Default_Handler         /* 26: (not wired) */
    .long   Default_Handler         /* 27: (not wired) */
    .long   Default_Handler         /* 28: (not wired) */
    .long   Default_Handler         /* 29: (not wired) */
    .long   Default_Handler         /* 30: (not wired) */
    .long   Default_Handler         /* 31: (not wired) */

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
    def_irq_handler UART0RX_IRQHandler
    def_irq_handler Default_Handler

    .end
