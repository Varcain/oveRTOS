/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Error translation at the oveRTOS/LXP boundary. oveRTOS services report ove_err_t; the
 * LXP provider and host contracts speak lxp_err_t. The two enums share their values
 * today, but they are separate contracts and LXP may drop codes it never interprets, so
 * every error that crosses the boundary goes through these switches instead of relying
 * on the coincidence. An oveRTOS code LXP has no counterpart for folds into the LXP code
 * that reports it; any other code with no counterpart becomes an I/O error.
 */
#ifndef LXP_OVE_ERR_H
#define LXP_OVE_ERR_H

#include "lxp/lxp_types.h"
#include "ove/types.h"

/* The error pairs, one per row: X(oveRTOS code, LXP code). */
#define LXP_OVE_ERR_PAIRS(X)                                              \
	X(OVE_OK, LXP_OK)                                                 \
	X(OVE_ERR_NOT_REGISTERED, LXP_ERR_NOT_REGISTERED)                 \
	X(OVE_ERR_INVALID_PARAM, LXP_ERR_INVALID_PARAM)                   \
	X(OVE_ERR_NO_MEMORY, LXP_ERR_NO_MEMORY)                           \
	X(OVE_ERR_TIMEOUT, LXP_ERR_TIMEOUT)                               \
	X(OVE_ERR_NOT_SUPPORTED, LXP_ERR_NOT_SUPPORTED)                   \
	X(OVE_ERR_QUEUE_FULL, LXP_ERR_QUEUE_FULL)                         \
	X(OVE_ERR_NET_REFUSED, LXP_ERR_NET_REFUSED)                       \
	X(OVE_ERR_NET_UNREACHABLE, LXP_ERR_NET_UNREACHABLE)               \
	X(OVE_ERR_NET_ADDR_IN_USE, LXP_ERR_NET_ADDR_IN_USE)               \
	X(OVE_ERR_NET_RESET, LXP_ERR_NET_RESET)                           \
	X(OVE_ERR_NET_DNS_FAIL, LXP_ERR_NET_DNS_FAIL)                     \
	X(OVE_ERR_NET_CLOSED, LXP_ERR_NET_CLOSED)                         \
	X(OVE_ERR_WOULD_BLOCK, LXP_ERR_WOULD_BLOCK)                       \
	X(OVE_ERR_EOF, LXP_ERR_EOF)                                       \
	X(OVE_ERR_NOT_FOUND, LXP_ERR_NOT_FOUND)                           \
	X(OVE_ERR_NET_ADDR_NOT_AVAILABLE, LXP_ERR_NET_ADDR_NOT_AVAILABLE) \
	X(OVE_ERR_ALREADY_EXISTS, LXP_ERR_ALREADY_EXISTS)                 \
	X(OVE_ERR_NO_SPACE, LXP_ERR_NO_SPACE)                             \
	X(OVE_ERR_NOT_DIR, LXP_ERR_NOT_DIR)                               \
	X(OVE_ERR_IS_DIR, LXP_ERR_IS_DIR)                                 \
	X(OVE_ERR_NOT_EMPTY, LXP_ERR_NOT_EMPTY)                           \
	X(OVE_ERR_READ_ONLY, LXP_ERR_READ_ONLY)                           \
	X(OVE_ERR_IO, LXP_ERR_IO)                                         \
	X(OVE_ERR_BUSY, LXP_ERR_BUSY)                                     \
	X(OVE_ERR_NAME_TOO_LONG, LXP_ERR_NAME_TOO_LONG)                   \
	X(OVE_ERR_BAD_HANDLE, LXP_ERR_BAD_HANDLE)                         \
	X(OVE_ERR_PERMISSION, LXP_ERR_PERMISSION)                         \
	X(OVE_ERR_CROSS_DEVICE, LXP_ERR_CROSS_DEVICE)

/* oveRTOS codes LXP has no counterpart for, one per row: X(oveRTOS code, the LXP code
 * that reports it). They cross into LXP only. */
#define LXP_OVE_ERR_FOLDS(X)                        \
	X(OVE_ERR_INVAL, LXP_ERR_INVALID_PARAM)     \
	X(OVE_ERR_QUEUE_EMPTY, LXP_ERR_WOULD_BLOCK) \
	X(OVE_ERR_ML_FAILED, LXP_ERR_IO)            \
	X(OVE_ERR_BUS_NACK, LXP_ERR_IO)             \
	X(OVE_ERR_BUS_BUSY, LXP_ERR_IO)             \
	X(OVE_ERR_BUS_ERROR, LXP_ERR_IO)

/* An oveRTOS result as the LXP contracts report it. */
static inline int lxp_err_from_ove(int err)
{
	switch (err) {
#define LXP_OVE_ERR_TO_LXP(ove, lxp) \
	case ove:                    \
		return lxp;
		LXP_OVE_ERR_PAIRS(LXP_OVE_ERR_TO_LXP)
		LXP_OVE_ERR_FOLDS(LXP_OVE_ERR_TO_LXP)
#undef LXP_OVE_ERR_TO_LXP
	default:
		return LXP_ERR_IO;
	}
}

/* An LXP result as the oveRTOS host API reports it. */
static inline int ove_err_from_lxp(int err)
{
	switch (err) {
#define LXP_OVE_ERR_TO_OVE(ove, lxp) \
	case lxp:                    \
		return ove;
		LXP_OVE_ERR_PAIRS(LXP_OVE_ERR_TO_OVE)
#undef LXP_OVE_ERR_TO_OVE
	default:
		return OVE_ERR_IO;
	}
}

#endif /* LXP_OVE_ERR_H */
