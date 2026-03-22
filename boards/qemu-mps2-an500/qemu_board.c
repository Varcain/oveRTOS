/*
 * Minimal board initialization for QEMU MPS2-AN500.
 * Semihosting is used for I/O — no real hardware to configure.
 */

#include "ove/hal/hal_board.h"
#include "ove/types.h"

extern void stub_gpio_reset(void);

int ove_hal_board_init(void)
{
	stub_gpio_reset();
	return OVE_OK;
}
