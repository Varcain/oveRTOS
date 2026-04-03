/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Export compile-time config flags to JS so the dashboard can
 * show/hide panels based on which modules are enabled.
 */

#ifdef __EMSCRIPTEN__

#include "ove_config.h"
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
int ove_wasm_has_audio(void)
{
#ifdef CONFIG_OVE_AUDIO
	return 1;
#else
	return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE
int ove_wasm_has_lvgl(void)
{
#ifdef CONFIG_OVE_LVGL
	return 1;
#else
	return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE
int ove_wasm_has_shell(void)
{
#ifdef CONFIG_OVE_SHELL
	return 1;
#else
	return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE
int ove_wasm_has_net(void)
{
#ifdef CONFIG_OVE_NET
	return 1;
#else
	return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE
int ove_wasm_has_infer(void)
{
#ifdef CONFIG_OVE_INFER
	return 1;
#else
	return 0;
#endif
}

#endif /* __EMSCRIPTEN__ */
