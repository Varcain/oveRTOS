/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Build configuration of the NuttX scheduler-note regression test
 * (tests/obs/nuttx_note/).  It mirrors what the NuttX firmware build uses
 * for the code under test (see output/qemu/nuttx/example_c/generated/
 * ove_config.h): the trace stream and the scheduler-driven state stats are
 * on, and the NuttX backend's storage header is the one that defines
 * `struct ove_thread`.
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_TRACE_STREAM 1
#define CONFIG_OVE_TRACE_MARKERS 1
#define CONFIG_OVE_THREAD_STATE_STATS 1
#define CONFIG_OVE_RTOS_NUTTX 1
#define CONFIG_OVE_TRACE_RING_SIZE 4096

/* The NuttX build always applies config/patches/nuttx/, which adds the
 * `ready` note op and the macro that announces it (see the patch and
 * dl/nuttx-24f210ef/include/nuttx/note/note_driver.h).  The stand-in in
 * nuttx_note_shim.h mirrors that patched layout, so the ready op is
 * compiled here exactly as it is in the firmware. */
#define OVE_NUTTX_HAVE_SCHED_NOTE_READY 1

#endif /* OVE_CONFIG_H */
