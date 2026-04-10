# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

############################################################################
# boards/stm32f746g-discovery/nuttx/Makefile
#
# NuttX application Makefile for oveRTOS on STM32F746G-Discovery.
# When building via the oveRTOS build system, this directory is copied into
# the NuttX apps tree at $(APPDIR)/external/ove_app/.
############################################################################

include $(APPDIR)/Make.defs

# Application info (read from NuttX Kconfig)
PROGNAME  = $(CONFIG_EXTERNAL_OVE_APP_PROGNAME)
PRIORITY  = $(CONFIG_EXTERNAL_OVE_APP_PRIORITY)
STACKSIZE = $(CONFIG_EXTERNAL_OVE_APP_STACKSIZE)
MODULE    = $(CONFIG_EXTERNAL_OVE_APP)

# Resolve directories — .ove_env written by build system, fallback to env
-include .ove_env
OVE_DIR   ?= $(error OVE_DIR must be set)

# Resolve OVE_APP_DIR from .config / app_paths.json
include $(OVE_DIR)/config/make/ove_app_resolve.mk

# Entry point
MAINSRC = main.c

# Shared oveRTOS NuttX boilerplate — populates CSRCS / CFLAGS / CXXFLAGS
# from the generated config.
include $(OVE_DIR)/config/make/ove_nuttx_common.mk

# ── STM32 MCU flags ────────────────────────────────────────────────
# Ensure hard-float even when the base NuttX config doesn't set it.
ifeq ($(CONFIG_ARCH_FPU),y)
CFLAGS   += -mfloat-abi=hard -mfpu=fpv5-sp-d16
CXXFLAGS += -mfloat-abi=hard -mfpu=fpv5-sp-d16
endif

# CMSIS-DSP build flags (must be defined before -O3 override)
CFLAGS   += -DARM_MATH_CM7 -D__FPU_PRESENT=1 -O3
CXXFLAGS += -DARM_MATH_CM7 -D__FPU_PRESENT=1 -O3

# ── CMSIS-DSP library ──────────────────────────────────────────────
CMSIS_DSP_DIR  = $(OVE_DIR)/dl/CMSIS-DSP
CMSIS_CORE_DIR = $(OVE_DIR)/dl/CMSIS_5/CMSIS/Core

CSRCS += $(CMSIS_DSP_DIR)/Source/TransformFunctions/TransformFunctions.c
CSRCS += $(CMSIS_DSP_DIR)/Source/ComplexMathFunctions/ComplexMathFunctions.c
CSRCS += $(CMSIS_DSP_DIR)/Source/BasicMathFunctions/BasicMathFunctions.c
CSRCS += $(CMSIS_DSP_DIR)/Source/CommonTables/CommonTables.c
CSRCS += $(CMSIS_DSP_DIR)/Source/SupportFunctions/SupportFunctions.c

CFLAGS   += -I$(CMSIS_DSP_DIR)/Include
CFLAGS   += -I$(CMSIS_DSP_DIR)/PrivateInclude
CFLAGS   += -I$(CMSIS_CORE_DIR)/Include
CXXFLAGS += -I$(CMSIS_DSP_DIR)/Include
CXXFLAGS += -I$(CMSIS_DSP_DIR)/PrivateInclude
CXXFLAGS += -I$(CMSIS_CORE_DIR)/Include

include $(APPDIR)/Application.mk
