/* SPDX-License-Identifier: GPL-3.0-or-later */
.syntax unified
.thumb

/* Encoding controls only. Their targets need not be backed by this object;
 * none is linked into firmware or executed. Displacements use Thumb PC=.+4. */
.macro branch_fact name, instruction, target
.global \name
.type \name,%function
.thumb_func
\name:
\instruction \target
.size \name, .-\name
.endm
branch_fact narrow_forward, b.n, .+4
branch_fact narrow_backward, b.n, .-12
branch_fact wide_forward, b.w, .+4
branch_fact wide_backward, b.w, .-12
branch_fact wide_plus4m, b.w, .+0x400004
branch_fact wide_plus8m, b.w, .+0x800004
branch_fact wide_minus4m, b.w, .-0x3ffffc
branch_fact wide_minus8m, b.w, .-0x7ffffc
