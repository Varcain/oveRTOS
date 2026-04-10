# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

############################################################################
# boards/qemu-mps2-an500/nuttx/Makefile
#
# NuttX application Makefile for oveRTOS on QEMU MPS2-AN500.
# Sim framework replaces the NuttX display/audio backends — QEMU uses
# semihosting transport, never native drivers.
############################################################################

ifneq ($(APPDIR),)
include $(APPDIR)/Make.defs
endif

# Application info (read from NuttX Kconfig)
PROGNAME  = $(CONFIG_EXTERNAL_OVE_APP_PROGNAME)
PRIORITY  = $(CONFIG_EXTERNAL_OVE_APP_PRIORITY)
STACKSIZE = $(CONFIG_EXTERNAL_OVE_APP_STACKSIZE)
MODULE    = $(CONFIG_EXTERNAL_OVE_APP)

# Resolve directories — .ove_env is written by the build system.
-include .ove_env
OVE_DIR     ?= $(error OVE_DIR must be set — run ove build)
OVE_GEN_DIR ?= $(error OVE_GEN_DIR must be set — run ove build)

# Resolve OVE_APP_DIR from .config / app_paths.json
include $(OVE_DIR)/config/make/ove_app_resolve.mk

# Entry point
MAINSRC = main.c

# Shared oveRTOS NuttX boilerplate — populates CSRCS / CFLAGS / CXXFLAGS
# from the generated config.
include $(OVE_DIR)/config/make/ove_nuttx_common.mk

# ── QEMU sim framework ─────────────────────────────────────────────
# Replace NuttX lvgl/audio backends with the sim framework HAL.
SIM_DIR      := $(OVE_DIR)/sim
QEMU_SRC_DIR := $(OVE_DIR)/boards/qemu-mps2-an500/src

CSRCS := $(filter-out %nuttx_lvgl.c,$(CSRCS))
CSRCS := $(filter-out %nuttx_audio.c,$(CSRCS))

ifneq ($(wildcard $(LVGL_INCLUDE_PATH)/lvgl.h),)
CSRCS += $(SIM_DIR)/hal/sim_lvgl.c
endif
CSRCS += $(SIM_DIR)/hal/sim_audio.c
CSRCS += $(SIM_DIR)/src/ove_sim_registry.c
CSRCS += $(SIM_DIR)/src/ove_sim_input.c
CSRCS += $(SIM_DIR)/src/ove_sim_transport_shm_guest.c
CSRCS += $(SIM_DIR)/hal/sim_board.c
CSRCS += $(SIM_DIR)/hal/sim_display.c

CFLAGS += -I$(OVE_DIR)/sim/include
CFLAGS += -I$(OVE_DIR)/sim/src
CFLAGS += -I$(QEMU_SRC_DIR)
ifneq ($(wildcard $(LVGL_INCLUDE_PATH)/lvgl.h),)
CFLAGS += -I$(LVGL_INCLUDE_PATH)
CFLAGS += -I$(LVGL_PARENT_PATH)
endif

ifneq ($(APPDIR),)
include $(APPDIR)/Application.mk
endif
