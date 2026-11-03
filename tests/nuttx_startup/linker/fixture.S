/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <nuttx/config.h>
.syntax unified
.thumb
.eabi_attribute 24, 1
.section .vectors,"a",%progbits
.balign 512
.global _vectors
.type _vectors,%object
_vectors:
.word _ebss + CONFIG_IDLETHREAD_STACKSIZE
.word __start
.size _vectors, .-_vectors
.section .text.__start,"ax",%progbits
.global __start
.thumb_func
.type __start,%function
__start:
bx lr
.size __start, .-__start
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
.space OVE_TEST_LIVE_BYTES
.size fixture_live, .-fixture_live
