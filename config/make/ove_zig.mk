# oveRTOS Zig (zig build-lib) NuttX Make Integration
#
# Expects the app's sources.mk to define:
#   APP_ZIG_SRC_DIR   — path to directory containing main.zig
#   APP_ZIG_LIB_NAME  — output library name (lib<name>.a)

# Resolve zig binary: custom path > toolchain dir > PATH
ifdef OVE_ZIG_PATH
  ZIG := $(OVE_ZIG_PATH)
else
  # Look for downloaded zig in the toolchains directory
  _TC_PARENT := $(realpath $(OVE_DIR)/output/toolchains)
  _ZIG_BIN := $(firstword $(wildcard $(_TC_PARENT)/zig-*/zig))
  ifneq ($(_ZIG_BIN),)
    ZIG := $(_ZIG_BIN)
  else
    ZIG := zig
  endif
endif

# Zig target for cross-compilation
ifeq ($(CONFIG_ARCH_FPU),y)
ZIG_TARGET ?= thumb-freestanding-eabihf
else
ZIG_TARGET ?= thumb-freestanding-eabi
endif

# MCU flag
ZIG_CPU ?= cortex_m7

# Output
ZIG_OUTPUT_DIR := $(APPDIR)/external/ove_app/zig_output
ZIG_LIB := $(ZIG_OUTPUT_DIR)/lib$(APP_ZIG_LIB_NAME).a

# oveRTOS Zig bindings
OVE_ZIG_BINDINGS := $(OVE_DIR)/bindings/zig/ove

# NuttX build tree root (apps live under APPDIR, nuttx is a sibling)
NUTTX_BUILD_DIR := $(realpath $(APPDIR)/../nuttx)

# LVGL include paths
LVGL_INCLUDE_PATH ?= $(OVE_DIR)/dl/lvgl
LVGL_PARENT_PATH ?= $(OVE_DIR)/dl

# Include flags
ZIG_INCLUDE_ARGS := \
	-I$(OVE_DIR)/include \
	-I$(OVE_GEN_DIR) \
	-I$(OVE_DIR)/backends/nuttx/include \
	-I$(NUTTX_BUILD_DIR)/include \
	-I$(LVGL_INCLUDE_PATH) \
	-I$(LVGL_PARENT_PATH)

# ── Generate storage sizes for zero-heap mode ────────────────────────
# When CONFIG_OVE_ZERO_HEAP is enabled, compile a C probe to measure
# storage type sizes and generate a header for Zig's @cImport.
ZIG_SIZES_DIR := $(ZIG_OUTPUT_DIR)/zig_sizes_include
ZIG_SIZES_HDR := $(ZIG_SIZES_DIR)/zig_storage_sizes.h

# Detect zero-heap from ove_config.h
_ZIG_ZERO_HEAP := $(shell grep -c 'CONFIG_OVE_ZERO_HEAP' $(OVE_GEN_DIR)/ove_config.h 2>/dev/null)
ifneq ($(_ZIG_ZERO_HEAP),0)
ZIG_INCLUDE_ARGS += -I$(ZIG_SIZES_DIR)
endif

# Hook into NuttX build
context:: zig_build

ifneq ($(_ZIG_ZERO_HEAP),0)
.PHONY: zig_storage_sizes
zig_storage_sizes:
	$(Q) mkdir -p $(ZIG_SIZES_DIR)
	$(Q) echo '#include "ove/ove.h"' > $(ZIG_OUTPUT_DIR)/_zig_sizes.c
	$(Q) echo '#include "ove/storage.h"' >> $(ZIG_OUTPUT_DIR)/_zig_sizes.c
	$(Q) echo '#include <stddef.h>' >> $(ZIG_OUTPUT_DIR)/_zig_sizes.c
	$(Q) echo '#define S(t) unsigned char _sizeof_##t[sizeof(t)];' >> $(ZIG_OUTPUT_DIR)/_zig_sizes.c
	$(Q) echo '#define A(t) unsigned char _alignof_##t[_Alignof(t)];' >> $(ZIG_OUTPUT_DIR)/_zig_sizes.c
	$(Q) for t in ove_thread_storage_t ove_queue_storage_t ove_timer_storage_t \
		ove_mutex_storage_t ove_sem_storage_t ove_event_storage_t \
		ove_condvar_storage_t ove_eventgroup_storage_t ove_workqueue_storage_t \
		ove_work_storage_t ove_stream_storage_t ove_watchdog_storage_t \
		ove_file_storage_t ove_dir_storage_t; do \
		echo "S($$t) A($$t)" >> $(ZIG_OUTPUT_DIR)/_zig_sizes.c; \
	done
	$(Q) $(CC) -c -w \
		-I$(OVE_DIR)/include \
		-I$(OVE_DIR)/backends/nuttx/include \
		-I$(OVE_GEN_DIR) \
		-I$(NUTTX_BUILD_DIR)/include \
		-o $(ZIG_OUTPUT_DIR)/_zig_sizes.o \
		$(ZIG_OUTPUT_DIR)/_zig_sizes.c
	$(Q) python3 $(OVE_DIR)/config/scripts/extract_storage_sizes.py \
		$(ZIG_OUTPUT_DIR)/_zig_sizes.o $(ZIG_OUTPUT_DIR)/_zig_sizes.env
	$(Q) echo '/* Auto-generated storage sizes for Zig zero-heap builds. */' > $(ZIG_SIZES_HDR)
	$(Q) while IFS='=' read key val; do \
		echo "#define OVE_$$key $$val" >> $(ZIG_SIZES_HDR); \
	done < $(ZIG_OUTPUT_DIR)/_zig_sizes.env
endif

.PHONY: zig_build
zig_build: $(if $(filter-out 0,$(_ZIG_ZERO_HEAP)),zig_storage_sizes)
	$(Q) mkdir -p $(ZIG_OUTPUT_DIR)
	$(ZIG) build-lib \
		-target $(ZIG_TARGET) \
		-mcpu=$(ZIG_CPU) \
		-OReleaseSafe \
		-fllvm -flld \
		--name $(APP_ZIG_LIB_NAME) \
		-femit-bin=$(ZIG_LIB) \
		--dep ove \
		-Mroot=$(APP_ZIG_SRC_DIR)/main.zig \
		$(ZIG_INCLUDE_ARGS) \
		-Move=$(OVE_ZIG_BINDINGS)/src/root.zig
	$(Q) mkdir -p $(APPDIR)/staging
	$(Q) cp -f $(ZIG_LIB) $(APPDIR)/staging/libzig_crate$(LIBEXT)
