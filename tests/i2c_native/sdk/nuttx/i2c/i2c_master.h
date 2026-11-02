/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OVE_TEST_I2C_NUTTX_MASTER_H
#define OVE_TEST_I2C_NUTTX_MASTER_H
#include <stdint.h>
#include <sys/types.h>
#define I2CIOC_TRANSFER 0x1234ul
#define I2C_M_READ 0x0001
#define I2C_M_NOSTOP 0x0040
/* Dependency declarations follow the pinned native message API. The real
 * target builds separately verify the native ABI; this fixture traces calls. */
struct i2c_msg_s {
	uint32_t frequency;
	uint16_t addr, flags;
	uint8_t *buffer;
	ssize_t length;
};
struct i2c_transfer_s {
	struct i2c_msg_s *msgv;
	size_t msgc;
};
#endif
