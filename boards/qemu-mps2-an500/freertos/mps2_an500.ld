/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Linker script for QEMU MPS2-AN500 (Cortex-M7).
 * Flash: 0x00000000 (4 MB)
 * RAM:   0x20000000 (4 MB)
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
}

ENTRY(Reset_Handler)

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        . = ALIGN(4);
        __stext = .;        /* start of real code; profiler uses this as
                               lower bound for PC validity, so small
                               integers in the vector table don't pass
                               the is-text-address filter */
        *(.text*)
        *(.rodata*)

        KEEP(*(.init))
        KEEP(*(.fini))

        /* C++ static constructors / destructors */
        . = ALIGN(4);
        __preinit_array_start = .;
        KEEP(*(.preinit_array*))
        __preinit_array_end = .;

        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        __init_array_end = .;

        . = ALIGN(4);
        __fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        __fini_array_end = .;

        *(.ARM.extab*)
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;

        . = ALIGN(4);
        __etext = .;
    } > FLASH

    .data : AT(__etext)
    {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM

    .bss :
    {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap :
    {
        /* Heap region markers.  All PROVIDE'd so they only land in the
         * symbol table when libc actually references them — in zero-heap
         * mode picolibc's malloc is dead-stripped, no consumer exists,
         * and the audit (cmake/OveZeroHeapAudit.cmake STRICT) sees no
         * heap-region symbols.  Direct assignment instantiated the
         * symbols unconditionally, which made STRICT trip on otherwise-
         * unused linker markers.
         *
         * __heap_start / __heap_end are picolibc's picosbrk.c API.
         * __HeapBase / __HeapLimit are kept for backwards-compatibility
         * with code (and any leftover newlib that might be linked in a
         * non-default config) that still expects the old names.
         * end / __end__ are the historic "sbrk grows from here" symbols
         * and stay PROVIDE-style as before. */
        PROVIDE(__HeapBase = .);
        PROVIDE(__end__ = .);
        PROVIDE(end = .);
        PROVIDE(__heap_start = .);
        KEEP(*(.heap*))
        . = . + 0x180000;  /* 1.5 MB heap for semihosting malloc (LVGL needs ~1 MB) */
        PROVIDE(__HeapLimit = .);
        PROVIDE(__heap_end = .);
    } > RAM

    /* Stack at top of RAM. Size comes from startup .s (.space 0x2000). */
    .stack (ORIGIN(RAM) + LENGTH(RAM) - 0x2000) :
    {
        __StackLimit = .;
        KEEP(*(.stack*))
        __StackTop = .;
    } > RAM

    __stack = __StackTop;
    PROVIDE(__stack = __StackTop);
}
