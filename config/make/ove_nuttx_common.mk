# ============================================================================
# ove_nuttx_common.mk — Shared boilerplate for NuttX board Makefiles
# ============================================================================
#
# Included from per-board boards/<board>/nuttx/Makefile.  Must be included
# AFTER:
#   - $(APPDIR)/Make.defs is included
#   - OVE_DIR, OVE_APP_DIR are resolved (via ove_app_resolve.mk)
#   - PROGNAME / PRIORITY / STACKSIZE / MODULE are set
#   - MAINSRC is set
#
# Must be included BEFORE:
#   - $(APPDIR)/Application.mk
#
# Boards may add their own sources to CSRCS (with += or :=), extend CFLAGS
# and CXXFLAGS, or rewrite CSRCS via filter-out after this include runs.

# Generated config directory (defaults to shared workspace generated/ if not set)
OVE_GEN_DIR ?= $(OVE_DIR)/output/generated

# oveRTOS framework backend sources (generated from .config)
-include $(OVE_GEN_DIR)/ove_sources.mk
CSRCS += $(OVE_BACKEND_SRCS)

# Application sources (generated from app.yaml by 'ove configure')
-include $(OVE_GEN_DIR)/app_sources.mk
# Legacy fallback for apps still using a hand-written sources.mk
ifeq ($(APP_LANG),)
-include $(OVE_APP_DIR)/sources.mk
endif
# Language dispatch (C / C++ / Rust / Zig)
-include $(OVE_DIR)/config/make/ove_app_lang.mk

# Model data generated from models/*.tflite
CSRCS += $(OVE_MODEL_SRCS)

# LVGL sources (external — compiled within NuttX build, not bundled nuttx-apps)
-include $(OVE_GEN_DIR)/ove_lvgl_sources.mk

# TFLM sources (compiled as C++ within the NuttX build system)
-include $(OVE_GEN_DIR)/ove_tflm_sources.mk

# ── Common include paths ────────────────────────────────────────────
CFLAGS += $(APP_INCLUDES)
CFLAGS += -I$(OVE_DIR)/include
CFLAGS += -I$(OVE_DIR)/backends/nuttx/include
CFLAGS += -I$(OVE_DIR)/backends/common
CFLAGS += -I$(OVE_GEN_DIR)
CFLAGS += -I$(OVE_GEN_DIR)/generated_models

CXXFLAGS += $(APP_INCLUDES)
CXXFLAGS += -I$(OVE_DIR)/include
CXXFLAGS += -I$(OVE_DIR)/backends/nuttx/include
CXXFLAGS += -I$(OVE_DIR)/backends/common
CXXFLAGS += -I$(OVE_GEN_DIR)
CXXFLAGS += -I$(OVE_GEN_DIR)/generated_models
