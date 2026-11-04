// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
namespace tflite
{
inline constexpr size_t MicroArenaBufferAlignment()
{
	return 16;
}
} // namespace tflite
