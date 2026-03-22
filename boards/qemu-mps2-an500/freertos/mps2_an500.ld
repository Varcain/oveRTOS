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
        __HeapBase = .;
        __end__ = .;
        end = .;
        PROVIDE(end = .);
        KEEP(*(.heap*))
        . = . + 0x40000;  /* 256 KB heap for semihosting malloc */
        __HeapLimit = .;
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
