# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

############################################################################
# tests/sim/nuttx-qemu-zeroheap/nuttx_app/Makefile
#
# NuttX Application.mk for QEMU test runner (zero-heap mode).
# Builds and links:
#   - NuttX backend modules (pure POSIX, provided by NuttX kernel)
#   - Stub backends for hardware-dependent modules
#   - CMocka test framework
#   - All 20 test suites
############################################################################

include $(APPDIR)/Make.defs

# Application info

PROGNAME  = $(CONFIG_EXTERNAL_OVE_TEST_PROGNAME)
PRIORITY  = $(CONFIG_EXTERNAL_OVE_TEST_PRIORITY)
STACKSIZE = $(CONFIG_EXTERNAL_OVE_TEST_STACKSIZE)
MODULE    = $(CONFIG_EXTERNAL_OVE_TEST)

# Resolve OVE_DIR
OVE_DIR ?= $(error OVE_DIR must be set)
TESTS_ROOT  := $(OVE_DIR)/tests
STUB_DIR    := $(TESTS_ROOT)/backends/stub
SUITE_DIR   := $(TESTS_ROOT)/suites
NUTTX_BE    := $(OVE_DIR)/backends/nuttx
POSIX_BE    := $(OVE_DIR)/backends/posix

# Entry point
MAINSRC = main.c

# NuttX backend modules (10, use POSIX APIs provided by NuttX kernel)
CSRCS += $(NUTTX_BE)/nuttx_thread.c
CSRCS += $(NUTTX_BE)/nuttx_sync.c
CSRCS += $(NUTTX_BE)/nuttx_queue.c
CSRCS += $(NUTTX_BE)/nuttx_timer.c
CSRCS += $(NUTTX_BE)/nuttx_time.c
CSRCS += $(NUTTX_BE)/nuttx_eventgroup.c
CSRCS += $(NUTTX_BE)/nuttx_workqueue.c
CSRCS += $(NUTTX_BE)/nuttx_stream.c
CSRCS += $(NUTTX_BE)/nuttx_console.c
CSRCS += $(NUTTX_BE)/nuttx_shell.c

# Core modules (board/GPIO/LED dispatcher)
CSRCS += $(OVE_DIR)/src/ove_board.c
CSRCS += $(OVE_DIR)/src/ove_gpio.c
CSRCS += $(OVE_DIR)/src/ove_led.c
CSRCS += $(OVE_DIR)/src/ove_app.c

# Stub backends for hardware-dependent modules
CSRCS += $(STUB_DIR)/stub_audio.c
CSRCS += $(STUB_DIR)/stub_bsp.c
CSRCS += $(STUB_DIR)/stub_lvgl.c
CSRCS += $(STUB_DIR)/stub_gpio.c
CSRCS += $(STUB_DIR)/stub_board.c
CSRCS += $(STUB_DIR)/stub_watchdog.c
CSRCS += $(STUB_DIR)/stub_nvs.c
CSRCS += $(STUB_DIR)/stub_fs.c

# Test suites (23)
CSRCS += $(SUITE_DIR)/test_thread.c
CSRCS += $(SUITE_DIR)/test_sync_mutex.c
CSRCS += $(SUITE_DIR)/test_sync_sem.c
CSRCS += $(SUITE_DIR)/test_sync_event.c
CSRCS += $(SUITE_DIR)/test_sync_condvar.c
CSRCS += $(SUITE_DIR)/test_sync_recursive.c
CSRCS += $(SUITE_DIR)/test_queue.c
CSRCS += $(SUITE_DIR)/test_timer.c
CSRCS += $(SUITE_DIR)/test_time.c
CSRCS += $(SUITE_DIR)/test_eventgroup.c
CSRCS += $(SUITE_DIR)/test_workqueue.c
CSRCS += $(SUITE_DIR)/test_stream.c
CSRCS += $(SUITE_DIR)/test_console.c
CSRCS += $(SUITE_DIR)/test_watchdog.c
CSRCS += $(SUITE_DIR)/test_nvs.c
CSRCS += $(SUITE_DIR)/test_shell.c
CSRCS += $(SUITE_DIR)/test_audio.c
CSRCS += $(SUITE_DIR)/test_bsp.c
CSRCS += $(SUITE_DIR)/test_board.c
CSRCS += $(SUITE_DIR)/test_gpio.c
CSRCS += $(SUITE_DIR)/test_led.c
CSRCS += $(SUITE_DIR)/test_fs.c
CSRCS += $(SUITE_DIR)/test_lvgl.c
CSRCS += $(SUITE_DIR)/test_app.c

# CMocka (built as part of this app)
CMOCKA_DIR := $(OVE_DIR)/dl/cmocka
CSRCS += $(CMOCKA_DIR)/src/cmocka.c

# Include paths (zeroheap config must come first to override ove_config.h)
CFLAGS += -I$(OVE_DIR)/tests/sim/nuttx-qemu-zeroheap
CFLAGS += -I$(OVE_DIR)/include
CFLAGS += -I$(NUTTX_BE)/include
CFLAGS += -I$(OVE_DIR)/backends/common
CFLAGS += -I$(TESTS_ROOT)
CFLAGS += -I$(CMOCKA_DIR)/include
CFLAGS += -I$(CMOCKA_DIR)/src
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers
CFLAGS += -DCONFIG_OVE_ZERO_HEAP=1

include $(APPDIR)/Application.mk
