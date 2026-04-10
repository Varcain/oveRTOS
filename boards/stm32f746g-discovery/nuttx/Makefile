# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

############################################################################
# boards/<board>/nuttx/Makefile
#
# NuttX application Makefile for oveRTOS
# Integrates oveRTOS framework + app code into the NuttX build system.
#
# When building via the oveRTOS build system, this directory is copied into
# the NuttX apps tree at $(APPDIR)/external/ove_app/.
#
# OVE_DIR and OVE_APP_DIR are passed from the top-level Makefile.
############################################################################

include $(APPDIR)/Make.defs

# Application info

PROGNAME  = $(CONFIG_EXTERNAL_OVE_APP_PROGNAME)
PRIORITY  = $(CONFIG_EXTERNAL_OVE_APP_PRIORITY)
STACKSIZE = $(CONFIG_EXTERNAL_OVE_APP_STACKSIZE)
MODULE    = $(CONFIG_EXTERNAL_OVE_APP)

# Resolve directories — .ove_env written by build system, fallback to env
-include .ove_env
OVE_DIR   ?= $(error OVE_DIR must be set)
ifeq ($(OVE_APP_DIR),)
    OVE_APP_NAME := $(shell grep '^CONFIG_OVE_APP_NAME=' \
        $(OVE_DIR)/.config 2>/dev/null | cut -d'"' -f2)
    ifneq ($(OVE_APP_NAME),)
        # Resolve via app_paths.json (supports two-level apps/<lang>/<app> layout)
        OVE_APP_DIR := $(shell python3 -c "import json,sys; d=json.load(open('$(OVE_DIR)/output/kconfig/app_paths.json')); print(d.get('$(OVE_APP_NAME)',''))" 2>/dev/null)
        ifeq ($(OVE_APP_DIR),)
            OVE_APP_DIR := $(OVE_DIR)/apps/$(OVE_APP_NAME)
        endif
    endif
endif

# Entry point

MAINSRC = main.c

# oveRTOS framework sources (generated from .config — sets VPATH + basenames)
OVE_GEN_DIR ?= $(OVE_DIR)/output/generated
-include $(OVE_GEN_DIR)/ove_sources.mk
CSRCS = $(OVE_BACKEND_SRCS)

# Application sources (generated from app.yaml by 'ove configure')
-include $(OVE_GEN_DIR)/app_sources.mk
# Legacy fallback
ifeq ($(APP_LANG),)
-include $(OVE_APP_DIR)/sources.mk
endif
-include $(OVE_DIR)/config/make/ove_app_lang.mk

# CMSIS-DSP (downloaded to dl/)
CMSIS_DSP_DIR  = $(OVE_DIR)/dl/CMSIS-DSP
CMSIS_CORE_DIR = $(OVE_DIR)/dl/CMSIS_5/CMSIS/Core

CSRCS += $(CMSIS_DSP_DIR)/Source/TransformFunctions/TransformFunctions.c
CSRCS += $(CMSIS_DSP_DIR)/Source/ComplexMathFunctions/ComplexMathFunctions.c
CSRCS += $(CMSIS_DSP_DIR)/Source/BasicMathFunctions/BasicMathFunctions.c
CSRCS += $(CMSIS_DSP_DIR)/Source/CommonTables/CommonTables.c
CSRCS += $(CMSIS_DSP_DIR)/Source/SupportFunctions/SupportFunctions.c

# Include paths
CFLAGS += -I$(CMSIS_DSP_DIR)/Include
CFLAGS += -I$(CMSIS_DSP_DIR)/PrivateInclude
CFLAGS += -I$(CMSIS_CORE_DIR)/Include
CFLAGS += $(APP_INCLUDES)
CFLAGS += -I$(OVE_DIR)/include
CFLAGS += -I$(OVE_DIR)/backends/nuttx/include
CFLAGS += -I$(OVE_DIR)/backends/common
CSRCS += $(OVE_MODEL_SRCS)
CFLAGS += -I$(OVE_GEN_DIR)
CFLAGS += -I$(OVE_GEN_DIR)/generated_models
CXXFLAGS += -I$(OVE_GEN_DIR)/generated_models

# TFLM sources (compiled as C++ within NuttX build system)
-include $(OVE_GEN_DIR)/ove_tflm_sources.mk

# FPU flags (ensure hard-float even when base NuttX config lacks it)
ifeq ($(CONFIG_ARCH_FPU),y)
CFLAGS += -mfloat-abi=hard -mfpu=fpv5-sp-d16
CXXFLAGS += -mfloat-abi=hard -mfpu=fpv5-sp-d16
endif

# CMSIS-DSP compile flags
CFLAGS += -DARM_MATH_CM7 -D__FPU_PRESENT=1 -O3

# Propagate paths and defines to C++ sources
CXXFLAGS += -I$(CMSIS_DSP_DIR)/Include
CXXFLAGS += -I$(CMSIS_DSP_DIR)/PrivateInclude
CXXFLAGS += -I$(CMSIS_CORE_DIR)/Include
CXXFLAGS += $(APP_INCLUDES)
CXXFLAGS += -I$(OVE_DIR)/include
CXXFLAGS += -I$(OVE_DIR)/backends/nuttx/include
CXXFLAGS += -I$(OVE_DIR)/backends/common
CXXFLAGS += -I$(OVE_GEN_DIR)
CXXFLAGS += -DARM_MATH_CM7 -D__FPU_PRESENT=1 -O3

include $(APPDIR)/Application.mk
