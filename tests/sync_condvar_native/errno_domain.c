/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#if OVE_ERRNO_MODE == 0
#undef EDQUOT
#elif OVE_ERRNO_MODE == 1
#undef EDQUOT
#define EDQUOT ENOSPC
#endif

/* Zero-heap allocation discards its size argument in this shared header. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include OVE_ERRNO_HELPER
#pragma GCC diagnostic pop

int compile_errno_domain(int error)
{
	return ove_errno_to_ove(error);
}
