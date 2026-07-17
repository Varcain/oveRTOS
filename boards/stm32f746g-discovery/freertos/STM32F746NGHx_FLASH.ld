/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Linker script for STM32F746G-Discovery (Cortex-M7) — picolibc + FreeRTOS
 * heap_4.  Mirrors boards/qemu-mps2-an500/freertos/mps2_an500.ld for the
 * generic picolibc/heap_4 pieces; STM32F7-specific additions are limited
 * to DTCM partitioning for I2S DMA buffers + DSP working area, SDRAM
 * reservations for the LTDC framebuffer + .sdram_bss, and the .RamData2
 * section main.c uses for small-heap ucHeap placement.
 *
 * Single-heap policy under heap mode: FreeRTOS heap_4's ucHeap is the
 * only heap, and libc malloc / free / calloc / realloc are wrapped in
 * backends/freertos/freertos_libc_malloc.c to route through
 * pvPortMalloc / vPortFree.  Picolibc therefore never reaches its
 * _sbrk-backed nano-malloc, and the `.heap` section below stays at
 * size 0 by default.
 *
 * Under zero-heap mode (configSUPPORT_DYNAMIC_ALLOCATION=0) there is no
 * pvPortMalloc, so the wrapper is not linked and picolibc malloc DOES
 * go through _sbrk.  Those builds pass `-Wl,--defsym=__heap_size=N` to
 * size `.heap` for whatever libc-side consumer needs it (cmocka in test
 * binaries; production zero-heap apps are expected to avoid libc
 * malloc entirely — backends/common/ove_heap_lock.c audits that).
 */

ENTRY(Reset_Handler)

/* Heap size: defaults to 0 (heap-mode builds have no use for it since
 * libc malloc is wrapped onto pvPortMalloc).  Zero-heap builds that
 * need a libc-side arena override via `-Wl,--defsym=__heap_size=N`. */
PROVIDE(__heap_size = 0);
/* Boot stack size.  Covers main() + `app_main()` setup running on MSP
 * before `ove::run()` calls vTaskStartScheduler — after that, every
 * task has its own stack via xTaskCreate.  Default 4 KB fits Rust/Zig
 * setup paths (Arc/Box, closure boxing inside `spawn_with`, the entirety
 * of `lvgl::init()` + widget creation) which need deeper frames than the
 * C/C++ equivalents.  512 B was sufficient for the C example but Rust's
 * `app_main` peaked at ~592 B mid-`lvgl::init()` and overflowed into the
 * adjacent `work_mem_int` TLSF pool — the overflow corrupted a free-block
 * header, and a later allocation popped a non-Thumb address into PC
 * (INVSTATE hardfault).
 *
 * Builds that don't run a Rust/Zig app (e.g. CMocka test binaries) may
 * shrink this back via `-Wl,--defsym=__stack_size=N`. */
PROVIDE(__stack_size = 0x1000);

MEMORY
{
    FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 1024K

    /* DTCM (64 KB at 0x20000000) is partitioned between I2S DMA and the
     * DSP working area.  Memory3/4 hold I2S RX/TX DMA buffers placed via
     * .RxBUF / .TxBUF section attributes in
     * backends/freertos/freertos_audio.c; DSP_DTCM is the remainder used
     * by external apps for DSP hot working buffers via the .dsp_bss
     * section attribute.  The 0x140 bytes from 0x20000000 to 0x20000140
     * hold the ETH DMA descriptor tables (4 RX + 4 TX × 40 B = exactly
     * 0x140) via the .eth_desc section attribute in stm32f7_eth.c.  DTCM
     * is architecturally never cached by the M7 yet is reachable by the
     * ETH DMA through the Cortex-M7 AHBS slave port, so the descriptors
     * stay coherent with D-cache ON and need no cache maintenance (ST
     * AN4839).  Empty (reclaimed as slack) in non-Ethernet builds. */
    ETH_DESC (xrw)  : ORIGIN = 0x20000000, LENGTH = 0x140      /* ETH DMA descriptors (uncached DTCM) */
    Memory3 (xrw)   : ORIGIN = 0x20000140, LENGTH = 0x2000     /* I2S RX DMA buf (8 KB) */
    Memory4 (xrw)   : ORIGIN = 0x20002140, LENGTH = 0x2000     /* I2S TX DMA buf (8 KB) */
    DSP_DTCM (xrw)  : ORIGIN = 0x20004140, LENGTH = 0xBEC0     /* 47.7 KB — DSP    */

    /* SRAM1 + SRAM2 (256 KB from 0x20010000) — general-purpose RAM. */
    RAM (xrw)       : ORIGIN = 0x20010000, LENGTH = 0x40000

    /* External SDRAM (8 MB) minus a 2 KB tail reserved for the ETH TX bounce buffer, so the
     * framebuffer + guest pools (.sdram_bss) can never grow into it (a linker overflow is caught
     * instead of a silent overlap). */
    SDRAM (xrw)     : ORIGIN = 0xC0000000, LENGTH = 8M - 0x800
    /* ETH TX bounce buffer: SDRAM, above the guest pools, covered by NO MPU region → the ARM
     * background map types it DEVICE (non-cacheable) in every task context, and the ETH DMA reaches
     * it over the FMC — so low_level_output can hand the DMA a coherent frame with the D-cache ON
     * (drivers/freertos/stm32f7/stm32f7_eth.c). */
    ETH_TXBUF (xrw) : ORIGIN = 0xC07FF800, LENGTH = 0x800
}

SECTIONS
{
    /* Code + read-only data + C runtime init/fini arrays + ARM unwind
     * info all live in FLASH.  Init arrays and exception-unwind sections
     * are folded into .text to match the QEMU MPS2 reference script —
     * one output section per FLASH region keeps the script compact. */
    .text :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))

        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)

        /* FreeRTOS-MPU (ARM_CM4_MPU, OVE_LINUX): the privileged kernel functions +
         * system-call wrappers must land in FLASH .text — else they orphan past _etext
         * and the .data LMA copy reads garbage (boots into a HardFault). */
        *(privileged_functions)
        *(freertos_system_calls)

        KEEP(*(.init))
        KEEP(*(.fini))

        *(.rodata)
        *(.rodata*)

        /* picolibc's __libc_init_array iterates a single contiguous
         * range delimited by __bothinit_array_{start,end}.  preinit
         * and init are laid back-to-back and bracketed by those
         * symbols, plus the newlib-style __preinit_array_{start,end}
         * and __init_array_{start,end} pairs so either libc's startup
         * code finds the C++ static constructors. */
        . = ALIGN(4);
        PROVIDE_HIDDEN(__bothinit_array_start = .);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
        PROVIDE_HIDDEN(__bothinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);

        /* C++ exception unwind info (empty under -fno-exceptions). */
        *(.ARM.extab* .gnu.linkonce.armextab.*)
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;

        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* used by the startup to initialize data */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        /* FreeRTOS marks these objects with an explicit PROGBITS section even
         * when their current initial value is zero.  Keep them in the copied
         * data image so non-zero kernel configuration defaults remain valid
         * and cannot turn the following .bss output section into PROGBITS. */
        *(privileged_data)
    } > RAM AT > FLASH

    /* `_edata` is anchored to the next explicit section (.tdata) so the
     * startup loader's `_sdata`..`_edata` copy covers the linker-orphan
     * `.got` / `.got.plt` sections, which GNU ld places between .data
     * and .tdata.  Zig emits TLS-IE accesses whose offset literal lives
     * in .got; an uninitialised .got bus-faults on the first errno
     * write inside lwIP's `event_callback` during `lwip_close`.  C TLS-LE
     * access uses constant offsets and never reads .got. */
    _edata = ADDR(.tdata);

    /* Picolibc TLS sections.  picolibc is built with
     * -Dthread-local-storage=true so libc's errno (and a small number
     * of other internal globals) are TLS variables; .tdata is the
     * initialised template (loaded from flash), .tbss is the
     * uninitialised template, and .tbss_space reserves real RAM for
     * the boot thread's TLS instance (ld.bfd does not advance the
     * location counter for NOLOAD sections, hence the separate
     * non-NOLOAD .tbss_space block). */
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

    /* LXP keeps a bounded stack of interrupted guest register/FP contexts in
     * g_sig_save.  It is privileged-only, never DMA-owned, and is reset by the
     * coordinator before a run, so the unused DTCM tail is a better fit than
     * either cacheable SDRAM or the already-full general SRAM bank.  Consume
     * this input section before the broad .bss* wildcard below. */
    .lxp_signal_state (NOLOAD) :
    {
        . = ALIGN(32);
        __lxp_signal_state_start__ = .;
        *(.bss.g_sig_save)
        . = ALIGN(32);
        __lxp_signal_state_end__ = .;
    } > DSP_DTCM
    ASSERT(__lxp_signal_state_end__ <= ORIGIN(DSP_DTCM) + LENGTH(DSP_DTCM),
           "LXP signal state must fit in internal DTCM")

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        /* PMSAv7 sizes every MPU region to a power of two and requires the base
         * to be aligned to it, so the guest trampoline stacks (1 KB each) and
         * the worker task stack (2 KB) carry large alignments.  Left wherever
         * -fdata-sections happens to emit them they land after arbitrary
         * objects, and each then buys up to (alignment - 1) bytes of fill —
         * 984 + 960 bytes of internal SRAM here, on a bank with ~2 KB spare.
         * Placed first the location counter is still at this output section's
         * own alignment, which is already the largest of its inputs, so they
         * cost nothing.  Order matters and is descending by alignment: the 2 KB
         * stack lands on the section base, leaving the counter 2 KB-aligned and
         * therefore already 1 KB-aligned for the stacks after it.  Add any
         * further MPU stack in alignment order.
         *
         * Matched by their -fdata-sections names, so renaming either variable
         * quietly reinstates the padding — the internal-SRAM margin check in
         * cmake/print_size.cmake is what catches that. */
        KEEP(*(.bss.g_worker_stack))
        KEEP(*(.bss.g_tramp_stacks))
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    /* FreeRTOS-MPU (ARM_CM4_MPU) static-region bounds for prvSetupMPU.  __SRAM_segment is
     * the full on-chip SRAM block (0x20000000, 512K — PMSAv7 needs a size-aligned base, and
     * the ld RAM region at 0x20010000 is not 256K-aligned).  privileged_functions/data are
     * DEGENERATE (start==end at the segment ends): isolation comes from the unprivileged
     * restricted tasks having no background region, not from a dedicated privileged region. */
    /* The unprivileged-flash MPU region always covers the INTERNAL flash: the
     * unprivileged personality guest first executes the kernel's prog_tramp /
     * park_loop trampolines (which live here) before and between running its own
     * code.  With the rootfs in QSPI the guest's cpio text is XIP'd from the
     * 0x90000000 window instead — that window gets its OWN per-task MPU region in
     * freertos_spawn_common (RO+X), since one static region cannot span both
     * internal flash and QSPI.  The internal-flash kernel is reached by
     * privileged code via PRIVDEFENA. */
    __FLASH_segment_start__        = ORIGIN(FLASH);
    __FLASH_segment_end__          = ORIGIN(FLASH) + LENGTH(FLASH);
    __SRAM_segment_start__         = 0x20000000;
    __SRAM_segment_end__           = 0x20080000;
    __privileged_functions_start__ = ORIGIN(FLASH) + LENGTH(FLASH);
    __privileged_functions_end__   = ORIGIN(FLASH) + LENGTH(FLASH);
    __privileged_data_start__      = 0x20080000;
    __privileged_data_end__        = 0x20080000;

    /* Critical privileged-task stacks and guest signal-return state.  The Linux coordinator
     * deliberately accesses guest SDRAM
     * through the Cortex-M7 background Device mapping, so placing host stacks there makes ordinary
     * unaligned locals and hardware exception/FP frames unsafe.  Use the on-chip DTCM working area:
     * it is Normal memory in the FreeRTOS-MPU all-SRAM region and is otherwise available when the
     * Linux personality is selected.  A build that also exhausts DSP_DTCM fails at link time. */
    .host_stacks (NOLOAD) :
    {
        . = ALIGN(32);
        __host_stacks_start__ = .;
        KEEP(*(.host_stacks))
        KEEP(*(.host_stacks.*))
        . = ALIGN(32);
        __host_stacks_end__ = .;
    } > DSP_DTCM
    ASSERT(__host_stacks_start__ >= ORIGIN(DSP_DTCM) &&
           __host_stacks_end__ <= ORIGIN(DSP_DTCM) + LENGTH(DSP_DTCM),
           "critical host stacks must fit in internal DTCM")

    /* Boot stack at the top of main RAM.  Just covers main() until
     * vTaskStartScheduler() runs — per-task stacks come from FreeRTOS
     * after that.  `end` / `_end` are PROVIDE'd at the start of the
     * stack region as zero-sized markers for any leftover code that
     * still references them. */
    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        __StackLimit = .;
        . = . + __stack_size;
        . = ALIGN(8);
        __StackTop = .;
        _estack = .;
    } > RAM
    PROVIDE(__stack = _estack);

    .ARM.attributes 0 : { *(.ARM.attributes) }

    /* I2S audio DMA buffers — backends/freertos/freertos_audio.c places
     * tx_dma[] / rx_dma[] here via section attributes for DTCM speed +
     * AHB DMA reachability. */
    .RxBUF (NOLOAD) : { *(.RxBUF) } > Memory3
    .TxBUF (NOLOAD) : { *(.TxBUF) } > Memory4

    /* ETH DMA descriptor tables — stm32f7_eth.c places DMARxDscrTab /
     * DMATxDscrTab here via `__attribute__((section(".eth_desc")))` so they
     * live in uncached DTCM (coherent with the ETH DMA under D-cache ON). */
    .eth_desc (NOLOAD) : ALIGN(4) { *(.eth_desc) } > ETH_DESC

    /* DSP working buffers — external apps place hot DSP arrays here via
     * `__attribute__((section(".dsp_bss")))` for low-latency DTCM access. */
    .dsp_bss (NOLOAD) : ALIGN(32) { *(.dsp_bss) } > DSP_DTCM

    /* Small-heap ucHeap placement.  boards/stm32f746g-discovery/freertos/
     * main.c's `#elif configTOTAL_HEAP_SIZE > 0x10000` branch puts
     * ucHeap here when the heap fits in SRAM1 without competing with
     * other large BSS items.  Lives in the main RAM region now (used
     * to occupy a dedicated DTCM slice before DTCM was repurposed for
     * audio). */
    .RamData2 (NOLOAD) : { *(.RamData2) } > RAM

    /*
     * SDRAM at 0xC0000000.  Reserve the LTDC framebuffer region first
     * because stm32746g_discovery_lcd.h hardcodes
     * LCD_FB_START_ADDRESS = 0xC0000000.  Without the reservation,
     * variables tagged with .sdram_bss could overlap pixel memory
     * and corrupt the display.  Size: 480 x 272 RGB565 = 261120 B
     * rounded to 32-byte cache lines.
     */
    .lcd_framebuffer (NOLOAD) : ALIGN(32) {
        *(.lcd_framebuffer)
        . = . + (480 * 272 * 2);
        . = ALIGN(32);
    } > SDRAM

    .sdram_bss (NOLOAD) : ALIGN(32) { *(.sdram_bss) } > SDRAM

    /* ETH TX bounce buffer (non-cacheable via the background Device map — see the ETH_TXBUF
     * MEMORY entry).  NOLOAD, own region above the pools so it is never MPU-covered. */
    .eth_txbuf (NOLOAD) : ALIGN(32) { *(.eth_txbuf) } > ETH_TXBUF

    /* Picolibc heap.  In heap mode this is empty (size 0): libc malloc
     * is wrapped onto pvPortMalloc via freertos_libc_malloc.c and never
     * reaches _sbrk.  Zero-heap test builds override `__heap_size` and
     * use this for cmocka's malloc — main RAM is too tight for the
     * test binary's BSS plus a usable arena, so the heap lives in
     * SDRAM (which Renode models out-of-the-box for the LCD framebuffer
     * region; production zero-heap apps don't use libc malloc — audited
     * by backends/common/ove_heap_lock.c). */
    .heap (NOLOAD) : ALIGN(8)
    {
        PROVIDE(__HeapBase = .);
        PROVIDE(__heap_start = .);
        . = . + __heap_size;
        . = ALIGN(8);
        PROVIDE(__HeapLimit = .);
        PROVIDE(__heap_end = .);
    } > SDRAM
}
