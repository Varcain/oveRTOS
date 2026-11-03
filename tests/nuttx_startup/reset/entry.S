/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <nuttx/config.h>
.syntax unified
.thumb

/* Link-only definitions for the real native vector translation unit. */
.macro unused_entry name
.section .text.\name,"ax",%progbits
.global \name
.type \name,%function
.thumb_func
\name:
bx lr
.size \name, .-\name
.endm
unused_entry __start
unused_entry exception_common
unused_entry exception_direct
unused_entry __cyg_profile_func_enter
unused_entry __cyg_profile_func_exit
/* The actual vector flags retain unwind tables. Supply only their link-time
 * personality reference; no unwinder or fixture entry is ever executed. */
unused_entry __aeabi_unwind_cpp_pr0

.section .rodata.g_idle_topstack,"a",%progbits
.balign 4
.global g_idle_topstack
.type g_idle_topstack,%object
g_idle_topstack:
.word _ebss + CONFIG_IDLETHREAD_STACKSIZE
.size g_idle_topstack, .-g_idle_topstack
.section .bss.fixture_live,"aw",%nobits
.balign 4
.global fixture_live
.type fixture_live,%object
fixture_live:
.space 4
.size fixture_live, .-fixture_live
