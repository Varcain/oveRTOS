/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* Internal annotations for Clang's thread-safety analysis. */
#ifndef OVE_THREAD_SAFETY_H
#define OVE_THREAD_SAFETY_H

#if defined(__clang__)
#define OVE_TSA_(x) __attribute__((x))
#else
#define OVE_TSA_(x)
#endif

#define OVE_CAPABILITY(x) OVE_TSA_(capability(x))
#define OVE_GUARDED_BY(x) OVE_TSA_(guarded_by(x))
#define OVE_REQUIRES(...) OVE_TSA_(requires_capability(__VA_ARGS__))
#define OVE_TRY_ACQUIRE(...) OVE_TSA_(try_acquire_capability(__VA_ARGS__))
#define OVE_RELEASE(...) OVE_TSA_(release_capability(__VA_ARGS__))
#define OVE_NO_THREAD_SAFETY_ANALYSIS OVE_TSA_(no_thread_safety_analysis)

#endif
