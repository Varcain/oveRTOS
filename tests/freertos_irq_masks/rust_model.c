/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define main model_observer_main
#include "model.c"
#undef main

void irq_rust_setup(unsigned kind, unsigned canary)
{
	++cases;
	initialize((struct model_masks){0, 0, 0}, 0, false, 0);
	switch (kind) {
	case 0:
		break;
	case 1:
		exception = 17;
		priority[17] = 144;
		break;
	case 2:
		exception = 17;
		priority[17] = 64;
		expect_reject = true;
		break;
	case 3:
		exception = 2;
		expect_reject = true;
		break;
	case 4:
		control |= 1u;
		expect_reject = !OVE_MODEL_MPU;
		break;
	case 5:
		group = 4;
		expect_reject = true;
		break;
	case 6:
		exception = 15;
		priority[15] = 64;
		expect_reject = true;
		break;
	case 7:
		exception = 14;
		priority[14] = 144;
		break;
	case 8:
		exception = 5;
		priority[5] = 240;
		expect_reject = true;
		break;
	case 9:
		registers.b = required.b = target.b = 64;
		break;
	case 10:
		exception = 15;
		priority[15] = 240;
		break;
	case 11:
		exception = 11;
		priority[11] = 240;
		expect_reject = true;
		break;
	case 12:
		exception = 17;
		control |= 1u;
		priority[17] = 144;
		break;
	case 13:
		exception = 17;
		control |= 1u;
		priority[17] = 64;
		expect_reject = true;
		break;
	default:
		exit(2);
	}
	if (canary)
		check(false, "early failure survives every later IRQ control");
}
void irq_rust_callback(void)
{
	++delivered;
	check(!expect_reject, "unsupported caller cannot receive a Rust callback");
	check(registers.p == target.p && registers.f == target.f &&
		      registers.b == (target.b && target.b < 128 ? target.b : 128),
	      "Rust body receives the admitted native mask state");
}
int irq_rust_finish(void)
{
	check(!expect_reject && delivered == 1, "exactly one eligible Rust callback completed");
	check(equal(registers, target) && !bad_access && !bad_origin && !bad_write && !weakened,
	      "Rust scope restores masks without forbidden native operations");
	check(enters == 1 && exits == 1 && depth == 0 && isr_depth == 0,
	      "Rust scope retires native ownership before return");
	footer();
	return failures != 0;
}
