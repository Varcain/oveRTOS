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
    PSRAM (rwx) : ORIGIN = 0x60000000, LENGTH = 16M
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
        /* FreeRTOS-MPU (ARM_CM4_MPU) annotates kernel code PRIVILEGED_FUNCTION ->
         * section "privileged_functions", and the V1 wrappers put the unprivileged
         * system-call entry points in "freertos_system_calls". BOTH must land here,
         * INSIDE .text and BEFORE __etext — otherwise they become orphan sections
         * placed after __etext, which pushes .data's flash LMA past __etext and the
         * startup's `.data`-copy-from-__etext reads garbage (corrupting __stdio etc.).
         * The whole flash is unprivileged-RX via the static MPU region, so no separate
         * region is needed. Empty for the non-MPU port. */
        *(privileged_functions)
        *(freertos_system_calls)
        *(.rodata*)

        KEEP(*(.init))
        KEEP(*(.fini))

        /* C++ static constructors / destructors.
         * picolibc's __libc_init_array iterates a single
         * __bothinit_array_{start,end} range covering both preinit and
         * init.  Without these symbols the iteration sees start==end==0
         * and no constructors run — including gcc's __gcov_init, which
         * silently breaks coverage dumping. */
        . = ALIGN(4);
        __preinit_array_start = .;
        __bothinit_array_start = .;
        KEEP(*(.preinit_array*))
        __preinit_array_end = .;

        . = ALIGN(4);
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        __init_array_end = .;
        __bothinit_array_end = .;

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

    .data :
    {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM AT > FLASH

    /* Picolibc TLS sections.  configUSE_PICOLIBC_TLS=1 in FreeRTOSConfig.h
     * pulls in picolibc-freertos.h, whose _tls_size()/_tls_align() and
     * libc's inittls.c reference __tdata_size / __tdata_source /
     * __tbss_offset / __tbss_size / __tls_size / __tls_align /
     * __arm32_tls_tcb_offset.  picolibc.specs only defines these via
     * `-Tpicolibc.ld` when no `-T` is given, but the FreeRTOS firmware
     * build passes its own `-T mps2_an500.ld`, so we mirror the bits
     * picolibc.ld would have provided.  Layout matches picolibc.ld:
     * .tdata is initialized template (loaded from flash), .tbss is the
     * uninitialized template; .tbss_space reserves real RAM since
     * ld.bfd does not advance the location counter for NOLOAD sections. */
    .tdata : ALIGN(4)
    {
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        PROVIDE(__tdata_end = .);
    } > RAM AT > FLASH
    PROVIDE(__tdata_start = ADDR(.tdata));
    PROVIDE(__tdata_source = LOADADDR(.tdata));
    PROVIDE(__tdata_size = SIZEOF(.tdata));

    .tbss (NOLOAD) : ALIGN(4)
    {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        PROVIDE(__tls_end = .);
        PROVIDE(__tbss_end = .);
    } > RAM
    PROVIDE(__tbss_start = ADDR(.tbss));
    PROVIDE(__tbss_size = SIZEOF(.tbss));
    PROVIDE(__tls_first = SIZEOF(.tdata) > 0 ? ADDR(.tdata) : ADDR(.tbss));
    PROVIDE(__tls_base = __tls_first);
    PROVIDE(__tbss_offset = ADDR(.tbss) - __tls_first);
    PROVIDE(__tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));
    PROVIDE(__tls_size = __tls_end - __tls_base);
    PROVIDE(__arm32_tls_tcb_offset = MAX(8, __tls_align));

    .tbss_space (NOLOAD) :
    {
        . = . + SIZEOF(.tbss);
    } > RAM

    .bss :
    {
        __bss_start__ = .;
        *(.bss*)
        /* FreeRTOS-MPU kernel data (PRIVILEGED_DATA) -> section "privileged_data";
         * place it in RAM .bss (covered by the privileged background region — no
         * unprivileged access). Empty for the non-MPU port. */
        *(privileged_data)
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

    /* The Linux rootfs cpio is XIP'd from the bottom 12 MiB of PSRAM @ 0x60000000, where QEMU's
     * `-device loader` places it at reset (see qemu-run.sh) rather than embedding it in the ELF —
     * so the 11 MB rootfs never competes with the 4 MB internal FLASH. This is the QEMU analog of
     * the STM32 board, which XIPs the cpio from external QSPI NOR @ 0x90000000. The cpio window is
     * deliberately NOT reserved as an ELF section: a NOLOAD reservation still emits a PT_LOAD
     * program header spanning 0x60000000, which QEMU then flags as a ROM region overlapping the
     * -device-loader cpio. Instead the FDPIC program pools are placed at 0x60C00000 (top 4 MiB),
     * leaving 0x60000000..0x60C00000 as unclaimed ELF address space that QEMU fills. The guest
     * gets an unprivileged-RX MPU window over the bottom 12 MiB in freertos_spawn_common. */
    .psram 0x60C00000 (NOLOAD) :
    {
        *(.psram*)
    } > PSRAM

    /* Stack at top of RAM. Size comes from startup .s (.space 0x2000). */
    .stack (ORIGIN(RAM) + LENGTH(RAM) - 0x2000) :
    {
        __StackLimit = .;
        KEEP(*(.stack*))
        __StackTop = .;
    } > RAM

    __stack = __StackTop;
    PROVIDE(__stack = __StackTop);

    /* FreeRTOS-MPU (ARM_CM4_MPU) static-region bounds, referenced by prvSetupMPU().
     * The unprivileged-flash region spans the whole internal flash RX so guest entry/park
     * trampolines remain executable. ld.so/libc/busybox XIP from the PSRAM cpio through the
     * per-task RO+X windows described above. The privileged sub-regions are DEGENERATE
     * (start==end at the segment end → a harmless 32 B region):
     * isolation comes from the MPU background region (unprivileged access to any unmapped
     * address faults) + the per-task regions, not from privilege-gating the kernel here. */
    __FLASH_segment_start__        = ORIGIN(FLASH);
    __FLASH_segment_end__          = ORIGIN(FLASH) + LENGTH(FLASH);
    __SRAM_segment_start__         = ORIGIN(RAM);
    __SRAM_segment_end__           = ORIGIN(RAM) + LENGTH(RAM);
    __privileged_functions_start__ = ORIGIN(FLASH) + LENGTH(FLASH);
    __privileged_functions_end__   = ORIGIN(FLASH) + LENGTH(FLASH);
    __privileged_data_start__      = ORIGIN(RAM) + LENGTH(RAM);
    __privileged_data_end__        = ORIGIN(RAM) + LENGTH(RAM);
}
