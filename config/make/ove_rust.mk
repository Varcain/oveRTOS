# oveRTOS Rust (Cargo) NuttX Make Integration
#
# Expects the app's sources.mk to define:
#   APP_RUST_CRATE_DIR  — path to Cargo.toml directory
#   APP_RUST_LIB_NAME   — crate name (lib<name>.a)
#   APP_RUST_TARGET     — Rust target triple (default: thumbv7em-none-eabihf)

# Select Rust target based on FPU config — must match NuttX ABI
ifeq ($(CONFIG_ARCH_FPU),y)
APP_RUST_TARGET ?= thumbv7em-none-eabihf
else
APP_RUST_TARGET ?= thumbv7em-none-eabi
endif

# Resolve cargo path
ifdef OVE_RUST_TOOLCHAIN_PATH
  CARGO := $(OVE_RUST_TOOLCHAIN_PATH)/cargo
else
  CARGO := cargo
endif

# Place Rust build artifacts in the NuttX build workspace, not next to sources
CARGO_TARGET_DIR := $(APPDIR)/external/ove_app/rust_target
RUST_LIB_DIR := $(CARGO_TARGET_DIR)/$(APP_RUST_TARGET)/release
RUST_LIB := $(RUST_LIB_DIR)/lib$(APP_RUST_LIB_NAME).a

# LVGL include path for bindgen
LVGL_INCLUDE_PATH ?= $(OVE_DIR)/dl/lvgl
LVGL_PARENT_PATH ?= $(OVE_DIR)/dl

# CMSIS-DSP include paths for bindgen (if available)
_CMSIS_DSP_INC := $(OVE_DIR)/dl/CMSIS-DSP/Include
_CMSIS_CORE_INC := $(OVE_DIR)/dl/CMSIS_5/CMSIS/Core/Include
ifneq ($(wildcard $(_CMSIS_DSP_INC)/arm_math.h),)
  CARGO_CMSIS_VARS = CMSIS_DSP_INCLUDE=$(_CMSIS_DSP_INC) CMSIS_CORE_INCLUDE=$(_CMSIS_CORE_INC)
endif

# Board-specific lv_conf.h directory for bindgen
# OVE_BOARD_DIR_BASE is exported by the top-level Makefile
LV_CONF_PATH ?= $(OVE_BOARD_DIR_BASE)/nuttx

# Cross-compilation: resolve ARM sysroot and linker for bindgen
CROSSDEV ?= arm-none-eabi-
_RUST_CC := $(CROSSDEV)gcc
_TOOLCHAIN_BIN_DIR := $(dir $(shell which $(_RUST_CC) 2>/dev/null))
ifneq ($(_TOOLCHAIN_BIN_DIR),)
  _TOOLCHAIN_ROOT := $(realpath $(_TOOLCHAIN_BIN_DIR)/..)
  ARM_SYSROOT_INCLUDE := $(_TOOLCHAIN_ROOT)/arm-none-eabi/include
endif

# Cargo target env var for linker
_RUST_TARGET_UPPER := $(subst -,_,$(APP_RUST_TARGET))
_RUST_TARGET_UPPER := $(shell echo "$(_RUST_TARGET_UPPER)" | tr '[:lower:]' '[:upper:]')

# ── Generate storage type sizes for Rust bindings ──────────────────
# Compile a C file with sizeof/alignof arrays using NuttX's include
# paths, then extract sizes for cargo.
SIZES_C := $(APPDIR)/external/ove_app/ove_storage_sizes.c
SIZES_O := $(APPDIR)/external/ove_app/ove_storage_sizes.o
SIZES_ENV := $(APPDIR)/external/ove_app/ove_storage_sizes.env

# Hook into NuttX build: build Rust crate during context phase
context:: rust_build

.PHONY: rust_build
rust_build: $(SIZES_ENV)
	OVE_DIR=$(OVE_DIR) \
	OVE_GEN_DIR=$(OVE_GEN_DIR) \
	OVE_STORAGE_SIZES=$(SIZES_ENV) \
	LVGL_INCLUDE_PATH=$(LVGL_INCLUDE_PATH) \
	LVGL_PARENT_PATH=$(LVGL_PARENT_PATH) \
	LV_CONF_PATH=$(LV_CONF_PATH) \
	ARM_SYSROOT_INCLUDE=$(ARM_SYSROOT_INCLUDE) \
	CARGO_TARGET_DIR=$(CARGO_TARGET_DIR) \
	CARGO_TARGET_$(_RUST_TARGET_UPPER)_LINKER=$(_RUST_CC) \
	$(CARGO_CMSIS_VARS) \
	$(CARGO) build \
		--target $(APP_RUST_TARGET) \
		--release \
		--manifest-path $(APP_RUST_CRATE_DIR)/Cargo.toml
	$(Q) mkdir -p $(APPDIR)/staging
	$(Q) cp -f $(RUST_LIB) $(APPDIR)/staging/librust_crate$(LIBEXT)

$(SIZES_ENV): $(SIZES_O)
	$(Q) python3 $(OVE_DIR)/config/scripts/extract_storage_sizes.py $(SIZES_O) $(SIZES_ENV)

$(SIZES_O): $(SIZES_C)
	$(Q) $(CC) $(CFLAGS) -w -c -o $@ $<

$(SIZES_C):
	$(Q) printf '%s\n' \
		'#include "ove/storage.h"' \
		'#include <stddef.h>' \
		'#define S(type) unsigned char _sizeof_##type[sizeof(type)];' \
		'#define A(type) unsigned char _alignof_##type[_Alignof(type)];' \
		'S(ove_thread_storage_t) A(ove_thread_storage_t)' \
		'S(ove_queue_storage_t) A(ove_queue_storage_t)' \
		'S(ove_timer_storage_t) A(ove_timer_storage_t)' \
		'S(ove_mutex_storage_t) A(ove_mutex_storage_t)' \
		'S(ove_sem_storage_t) A(ove_sem_storage_t)' \
		'S(ove_event_storage_t) A(ove_event_storage_t)' \
		'S(ove_condvar_storage_t) A(ove_condvar_storage_t)' \
		'S(ove_eventgroup_storage_t) A(ove_eventgroup_storage_t)' \
		'S(ove_workqueue_storage_t) A(ove_workqueue_storage_t)' \
		'S(ove_work_storage_t) A(ove_work_storage_t)' \
		'S(ove_stream_storage_t) A(ove_stream_storage_t)' \
		'S(ove_watchdog_storage_t) A(ove_watchdog_storage_t)' \
		'S(ove_file_storage_t) A(ove_file_storage_t)' \
		'S(ove_dir_storage_t) A(ove_dir_storage_t)' > $@
	$(Q) if grep -q 'CONFIG_OVE_NET 1' $(OVE_GEN_DIR)/ove_config.h 2>/dev/null; then \
		printf '%s\n' \
			'S(ove_socket_storage_t) A(ove_socket_storage_t)' \
			'S(ove_netif_storage_t) A(ove_netif_storage_t)' >> $@; fi
	$(Q) if grep -q 'CONFIG_OVE_NET_HTTP 1' $(OVE_GEN_DIR)/ove_config.h 2>/dev/null; then \
		printf '%s\n' \
			'S(ove_http_client_storage_t) A(ove_http_client_storage_t)' >> $@; fi
	$(Q) if grep -q 'CONFIG_OVE_NET_MQTT 1' $(OVE_GEN_DIR)/ove_config.h 2>/dev/null; then \
		printf '%s\n' \
			'S(ove_mqtt_client_storage_t) A(ove_mqtt_client_storage_t)' >> $@; fi
	$(Q) if grep -q 'CONFIG_OVE_NET_TLS 1' $(OVE_GEN_DIR)/ove_config.h 2>/dev/null; then \
		printf '%s\n' \
			'S(ove_tls_storage_t) A(ove_tls_storage_t)' >> $@; fi
