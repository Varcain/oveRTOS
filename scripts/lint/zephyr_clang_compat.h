/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Lint-time shim included via `--extra-arg=-include` when clang-tidy
 * runs against the Zephyr backend's cross-compile compile_commands.json
 * (see config/ove-cli/ove/lint.py:_clang_tidy_backends).  Lives under
 * scripts/lint/ so the firmware build never sees it; the path is
 * outside every Zephyr include directory by design.
 *
 * Why this exists: the Zephyr SDK (arm-zephyr-eabi 12.2.0) ships
 * gcc's arm_acle.h, which defines wrapper inlines around 18
 * coprocessor builtins (`__builtin_arm_cdp`, `__builtin_arm_ldc`,
 * `__builtin_arm_mcr`, etc.).  Clang's signature for those builtins
 * requires constant arguments; gcc allows runtime values via
 * constant-propagation after inlining.  The wrappers pass function
 * parameters through, so even with `-isystem` demotion clang's
 * parser fails to AST-build any TU that transitively includes
 * arm_acle.h — and almost every Zephyr backend TU does, via
 * <zephyr/kernel.h> -> <zephyr/arch/arm/...>.
 *
 * The cheap fix: pre-set arm_acle.h's own include guard so the
 * header body is skipped entirely.  backends/zephyr (in source) does
 * not use the coprocessor intrinsics directly, and the Zephyr inlines
 * that might use them (memory barriers, etc.) come in via Zephyr's
 * own asm_inline_gcc.h with separate `__asm__ volatile` definitions,
 * not via arm_acle.h.  Verified by running clang-tidy across all 11
 * backends/zephyr (.c files): with this guard pre-set, zero parse
 * errors remain.
 *
 * Maintenance: if the Zephyr SDK is bumped and the include guard
 * macro changes, re-check with
 *   head -30 ${ZEPHYR_SDK}/arm-zephyr-eabi/lib/gcc/arm-zephyr-eabi/${VER}/include/arm_acle.h
 * and update the macro name below.  If new parse errors surface in
 * other vendor headers, extend this shim with the corresponding
 * guard or stub — keep additions minimal so the lint surface stays
 * close to what the firmware build actually compiles.
 */

#ifndef OVE_LINT_ZEPHYR_CLANG_COMPAT_H
#define OVE_LINT_ZEPHYR_CLANG_COMPAT_H

#ifndef _GCC_ARM_ACLE_H
#define _GCC_ARM_ACLE_H 1
#endif

#endif /* OVE_LINT_ZEPHYR_CLANG_COMPAT_H */
