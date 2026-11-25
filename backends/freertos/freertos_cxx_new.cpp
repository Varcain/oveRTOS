/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * FreeRTOS heap builds route malloc and free to the kernel heap but not
 * libc's memalign, which libstdc++'s aligned operator new uses. Aligned new
 * therefore over-allocates through the plain operator new and keeps the
 * original pointer just below the aligned block. NuttX and Zephyr allocate
 * aligned blocks from the same heap as free and need no replacement.
 *
 * The other aligned forms (array, nothrow, sized) forward to these two.
 * Both are weak so an application's own replacement still links.
 */

#include <cstddef>
#include <cstdint>
#include <new>

__attribute__((weak)) void *operator new(std::size_t size, std::align_val_t al)
{
	const auto align = static_cast<std::size_t>(al);
	if (size > SIZE_MAX - align - sizeof(void *)) {
		throw std::bad_alloc();
	}
	auto *raw = static_cast<unsigned char *>(::operator new(size + align + sizeof(void *)));
	unsigned char *p = raw + sizeof(void *);
	p += -reinterpret_cast<std::uintptr_t>(p) & (align - 1);
	reinterpret_cast<void **>(p)[-1] = raw;
	return p;
}

__attribute__((weak)) void operator delete(void *p, std::align_val_t) noexcept
{
	if (p != nullptr) {
		::operator delete(static_cast<void **>(p)[-1]);
	}
}
