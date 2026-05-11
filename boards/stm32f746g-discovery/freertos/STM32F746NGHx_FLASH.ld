/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
*****************************************************************************
**

**  File        : LinkerScript.ld
**
**  Abstract    : Linker script for STM32F746IGTx Device with
**                1024KByte FLASH, 320KByte RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used.
**
**  Target      : STMicroelectronics STM32
**
**
**  Distribution: The file is distributed as is, without any warranty
**                of any kind.
**
**  (c)Copyright Ac6.
**  You may use this file as-is or modify it according to the needs of your
**  project. Distribution of this file (unmodified or modified) is not
**  permitted. Ac6 permit registered System Workbench for MCU users the
**  rights to distribute the assembled, compiled & linked contents of this
**  file as part of an application binary file, provided that it is built
**  using the System Workbench for MCU toolchain.
**
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_heap_start = 0x2004DE00;
_estack = _heap_start + 0x2000;
/* Picolibc's picosbrk.c expects __heap_start / __heap_end (double-
 * underscore).  PROVIDE'd so they only land in the symbol table when
 * libc references them — in zero-heap mode picolibc's malloc is
 * dead-stripped, no consumer, and the audit (OveZeroHeapAudit STRICT)
 * sees no heap-region symbols. */
PROVIDE(__heap_start = _heap_start);
PROVIDE(__heap_end = _estack);
PROVIDE(__HeapBase = _heap_start);
PROVIDE(__HeapLimit = _estack);
/* Generate a link error if heap and stack don't fit into RAM */
_Min_Heap_Size = 0x2000;      /* required amount of heap  */
_Min_Stack_Size = 0x200; /* required amount of stack */

/* Specify the memory areas */
MEMORY
{

FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 1024K

/* DTCM (64 KB at 0x20000000) is reserved for audio + DSP.
 * Memory1..Memory4 are the ETH/audio DMA descriptor+buffer regions that
 * hardware DMA reads directly; DSP_DTCM is the remainder of DTCM used for
 * DSP hot working buffers via the .dsp_bss section. */
Memory1 (xrw)   : ORIGIN = 0x20000000, LENGTH = 0xA0       /* ETH RxDescrip  */
Memory2 (xrw)   : ORIGIN = 0x200000A0, LENGTH = 0xA0       /* ETH TxDescrip  */
Memory3 (xrw)   : ORIGIN = 0x20000140, LENGTH = 0x1dc4     /* I2S RX DMA buf */
Memory4 (xrw)   : ORIGIN = 0x20001F04, LENGTH = 0x1dc4     /* I2S TX DMA buf */
DSP_DTCM (xrw)  : ORIGIN = 0x20003CC8, LENGTH = 0xC338     /* 49 KB — DSP    */

/* SRAM1 + SRAM2 (256 KB from 0x20010000) — general-purpose RAM.
 * .RamData2 (used for small-heap ucHeap placement in main.c) is redirected
 * into this same region; it used to live in a dedicated 64 KB DTCM slice,
 * but DTCM is now reserved for audio + DSP. */
RAM (xrw)       : ORIGIN = 0x20010000, LENGTH = 0x40000

SDRAM (xrw)     : ORIGIN = 0xC0000000, LENGTH = 8M
QSPI (rx)       : ORIGIN = 0x90000000, LENGTH = 16M

}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  /* picolibc's __libc_init_array iterates a single contiguous range
   * delimited by __bothinit_array_{start,end}.  We lay preinit_array
   * and init_array back-to-back and bracket the pair with those
   * symbols, while still providing the newlib-style
   * __preinit_array_{start,end} / __init_array_{start,end} pairs so
   * either libc's startup code finds the C++ static constructors. */
  .preinit_array     :
  {
    PROVIDE_HIDDEN (__bothinit_array_start = .);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    PROVIDE_HIDDEN (__bothinit_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code.
   * `AT> FLASH` (auto-advance) rather than `AT(addr)` so the FLASH
   * location counter advances past .data — the .tdata section below
   * uses `AT> FLASH` too and would otherwise overlap .data's LMA. */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */
  } >RAM AT> FLASH

  /* `_edata` is anchored to the next explicit section (.tdata) instead
   * of the end-of-.data so the startup loader's `_sdata`..`_edata`
   * copy covers the linker-orphan `.got` and `.got.plt` sections,
   * which GNU ld places between .data and .tdata. Zig emits TLS-IE
   * accesses whose offset literal (e.g. `errno` at TP+16) lives in
   * .got; if .got is left at uninitialised RAM (0xaaaaaaaa here),
   * the next errno write — first hits inside lwIP's `event_callback`
   * during `lwip_close` — does `str r2, [TP, 0xaaaaaaaa]` and bus-
   * faults. C TLS-LE access uses constant offsets and never reads
   * .got, which is why the C zh build doesn't notice. */
  _edata = ADDR(.tdata);

  /* Picolibc TLS sections.  configUSE_PICOLIBC_TLS=1 in FreeRTOSConfig.h
   * pulls in picolibc-freertos.h, whose _tls_size()/_tls_align() and
   * libc's inittls.c reference __tdata_size / __tdata_source /
   * __tbss_offset / __tbss_size / __tls_size / __tls_align /
   * __arm32_tls_tcb_offset.  picolibc.specs only defines these via
   * `-Tpicolibc.ld` when no `-T` is given, but the FreeRTOS firmware
   * build passes its own `-T STM32F746NGHx_FLASH.ld`, so we mirror the
   * bits picolibc.ld would have provided.  See mps2_an500.ld for the
   * matching block on the QEMU board. */
  .tdata : ALIGN(4)
  {
    *(.tdata .tdata.* .gnu.linkonce.td.*)
    PROVIDE(__tdata_end = .);
  } >RAM AT> FLASH
  PROVIDE(__tdata_start = ADDR(.tdata));
  PROVIDE(__tdata_source = LOADADDR(.tdata));
  PROVIDE(__tdata_size = SIZEOF(.tdata));

  .tbss (NOLOAD) : ALIGN(4)
  {
    *(.tbss .tbss.* .gnu.linkonce.tb.*)
    *(.tcommon)
    PROVIDE(__tls_end = .);
    PROVIDE(__tbss_end = .);
  } >RAM
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
  } >RAM

  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss secion */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* User_heap_stack section, used to check that there is enough RAM left */
  ._user_heap_stack :
  {
    . = ALIGN(4);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(4);
  } >RAM

  

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
  .RxDecripSection (NOLOAD) : { *(.RxDecripSection) } >Memory1
  .TxDescripSection (NOLOAD) : { *(.TxDescripSection) } >Memory2
  .RxBUF (NOLOAD) : { *(.RxBUF) } >Memory3
  .TxBUF (NOLOAD) : { *(.TxBUF) } >Memory4
  .ExtQSPIFlashSection : { *(.ExtQSPIFlashSection) } >QSPI
  .RamData2 (NOLOAD): { *(.RamData2) } >RAM
  .dsp_bss (NOLOAD) : ALIGN(32) { *(.dsp_bss) } >DSP_DTCM
  /*
   * Reserve SDRAM at 0xC0000000 for the LTDC framebuffer.  The BSP
   * (stm32746g_discovery_lcd.h) hardcodes LCD_FB_START_ADDRESS = 0xC0000000,
   * so the linker must keep that region clear — otherwise variables placed
   * via .sdram_bss overlap pixel memory and corrupt the display.
   * Size: 480 x 272 RGB565 = 261120 bytes, rounded up + aligned for cache.
   */
  .lcd_framebuffer (NOLOAD) : ALIGN(32) {
    *(.lcd_framebuffer)
    . = . + (480 * 272 * 2);
    . = ALIGN(32);
  } >SDRAM
  .sdram_bss (NOLOAD) : ALIGN(32) { *(.sdram_bss) } >SDRAM
}


