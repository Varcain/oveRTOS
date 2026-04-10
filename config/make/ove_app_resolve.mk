# ============================================================================
# ove_app_resolve.mk — Resolve OVE_APP_DIR from the active configuration
# ============================================================================
#
# Included from per-board NuttX Makefiles before the app sources are needed.
# Assumes OVE_DIR has already been set (via .ove_env or command line).
#
# Resolution order:
#   1. OVE_APP_DIR set by caller — used as-is
#   2. CONFIG_OVE_APP_NAME in $(OVE_DIR)/.config + lookup via
#      output/kconfig/app_paths.json (supports apps/<lang>/<app> layout)
#   3. Fallback to flat layout $(OVE_DIR)/apps/<app>

ifeq ($(OVE_APP_DIR),)
  OVE_APP_NAME := $(shell grep '^CONFIG_OVE_APP_NAME=' \
      $(OVE_DIR)/.config 2>/dev/null | cut -d'"' -f2)
  ifneq ($(OVE_APP_NAME),)
    OVE_APP_DIR := $(shell python3 -c "import json; \
        d=json.load(open('$(OVE_DIR)/output/kconfig/app_paths.json')); \
        print(d.get('$(OVE_APP_NAME)',''))" 2>/dev/null)
    ifeq ($(OVE_APP_DIR),)
      OVE_APP_DIR := $(OVE_DIR)/apps/$(OVE_APP_NAME)
    endif
  endif
endif
