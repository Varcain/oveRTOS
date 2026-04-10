# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# ============================================================================
# oveRTOS Build System — Thin Makefile delegating to ove CLI
# ============================================================================
#
# Usage:
#   make menuconfig                       # Configure via TUI
#   make stm32f746_freertos_example_c_defconfig  # Load predefined config
#   make                                  # Full build
#   make flash                            # Flash to target
#   make test                             # Run sim tests
#
# All commands delegate to the 'ove' CLI installed in .venv.

OVE_DIR := $(CURDIR)
VENV_DIR    := $(OVE_DIR)/.venv
VENV_PYTHON := $(VENV_DIR)/bin/python
VENV_PIP    := $(VENV_DIR)/bin/pip
VENV_STAMP  := $(VENV_DIR)/.stamp
OVE     := $(VENV_DIR)/bin/ove
REQUIREMENTS := $(OVE_DIR)/config/requirements.txt
CLI_DIR     := $(OVE_DIR)/config/ove-cli

export PATH := $(OVE_DIR)/config/scripts:$(VENV_DIR)/bin:$(PATH)

# ── Python venv + CLI installation ─────────────────────────────────────────

$(VENV_STAMP): $(REQUIREMENTS) $(CLI_DIR)/pyproject.toml
	@command -v python3 >/dev/null 2>&1 || { \
		echo ""; \
		echo "ERROR: python3 not found."; \
		echo ""; \
		echo "  Ubuntu/Debian:  sudo apt install python3 python3-venv"; \
		echo "  Fedora:         sudo dnf install python3"; \
		echo "  Arch:           sudo pacman -S python"; \
		echo ""; \
		exit 1; \
	}
	@python3 -c "import venv" 2>/dev/null || { \
		echo ""; \
		echo "ERROR: python3-venv module not available."; \
		echo ""; \
		echo "  Ubuntu/Debian:  sudo apt install python3-venv"; \
		echo "  Fedora:         sudo dnf install python3-libs"; \
		echo ""; \
		exit 1; \
	}
	@echo "=== Setting up Python virtual environment ==="
	@python3 -m venv $(VENV_DIR)
	@$(VENV_PIP) install --quiet --upgrade pip
	@$(VENV_PIP) install --quiet -r $(REQUIREMENTS)
	@$(VENV_PIP) install --quiet -e $(CLI_DIR)
	@touch $@

.PHONY: setup
setup:
	@rm -f $(VENV_STAMP)
	@$(MAKE) $(VENV_STAMP)

# ── Configuration ──────────────────────────────────────────────────────────

.PHONY: menuconfig
menuconfig: $(VENV_STAMP)
	@$(OVE) menuconfig

%_defconfig: $(VENV_STAMP)
	@$(OVE) defconfig $@

# Fragment-based configuration: make <board>.<rtos>.<app> [ZEROHEAP=1]
# Examples:
#   make qemu.freertos.example_c
#   make stm32f746.zephyr.benchmark_rust
#   make host.posix.example_net_cpp ZEROHEAP=1
#
# Detect dot-separated targets in MAKECMDGOALS and generate rules for them.
# Exclude allconfigs-* and alldefconfigs (handled by their own pattern rules).
_DOT_TARGETS := $(foreach t,$(MAKECMDGOALS),$(if $(findstring .,$(t)),$(if $(filter allconfigs-% alldefconfigs,$(t)),,$(t))))
ifneq ($(_DOT_TARGETS),)
.PHONY: $(_DOT_TARGETS)
$(_DOT_TARGETS): $(VENV_STAMP)
	@$(OVE) defconfig-fragments "$@" $(if $(ZEROHEAP),--zeroheap)
endif

.PHONY: savedefconfig
savedefconfig: $(VENV_STAMP)
	@$(OVE) savedefconfig

.PHONY: nuttx-menuconfig
nuttx-menuconfig: $(VENV_STAMP)
	@$(OVE) rtos-menuconfig nuttx

.PHONY: zephyr-menuconfig
zephyr-menuconfig: $(VENV_STAMP)
	@$(OVE) rtos-menuconfig zephyr

# ── Build pipeline ─────────────────────────────────────────────────────────

.PHONY: download
download: $(VENV_STAMP)
	@$(OVE) download

.PHONY: configure
configure: $(VENV_STAMP)
	@$(OVE) configure

.PHONY: all
all: $(VENV_STAMP)
	@$(OVE) download
	@$(OVE) configure
	@$(OVE) build

# Build all app configurations for a given board.rtos pair.
# Usage: make allconfigs-host.posix
#        make allconfigs-qemu.freertos
#        make allconfigs-stm32f746.zephyr
.PHONY: allconfigs-%
allconfigs-%: $(VENV_STAMP)
	@BOARD_RTOS="$*"; \
	BOARD=$$(echo "$$BOARD_RTOS" | cut -d. -f1); \
	RTOS=$$(echo "$$BOARD_RTOS" | cut -d. -f2); \
	if [ -z "$$BOARD" ] || [ -z "$$RTOS" ]; then \
		echo "ERROR: usage: make allconfigs-<board>.<rtos>"; exit 1; \
	fi; \
	APPS=$$(grep -rh 'config_name:' $(OVE_DIR)/apps/*/app.yaml $(OVE_DIR)/apps/*/*/app.yaml 2>/dev/null \
		| sed 's/.*config_name: *//' | sort -u); \
	TOTAL=$$(echo "$$APPS" | wc -w); \
	CURRENT=0; \
	FAILED=""; \
	for app in $$APPS; do \
		CURRENT=$$((CURRENT + 1)); \
		echo ""; \
		echo "============================================================"; \
		echo "[$$CURRENT/$$TOTAL] Building $$BOARD.$$RTOS.$$app"; \
		echo "============================================================"; \
		if $(MAKE) "$$BOARD.$$RTOS.$$app" && $(MAKE); then \
			echo "[$$CURRENT/$$TOTAL] $$BOARD.$$RTOS.$$app: OK"; \
		else \
			echo "[$$CURRENT/$$TOTAL] $$BOARD.$$RTOS.$$app: FAILED"; \
			FAILED="$$FAILED $$app"; \
		fi; \
	done; \
	echo ""; \
	echo "============================================================"; \
	echo "allconfigs-$$BOARD.$$RTOS: $$TOTAL configurations processed"; \
	if [ -n "$$FAILED" ]; then \
		echo "FAILED:$$FAILED"; \
		exit 1; \
	else \
		echo "All configurations built successfully"; \
	fi

.PHONY: alldefconfigs
alldefconfigs: $(VENV_STAMP)
	@BOARDS=$$(find $(OVE_DIR)/boards -maxdepth 1 -mindepth 1 -type d -exec basename {} \; | sort); \
	RTOSES="freertos nuttx zephyr posix"; \
	for board in $$BOARDS; do \
		for rtos in $$RTOSES; do \
			echo ""; \
			echo "############################################################"; \
			echo "# allconfigs-$$board.$$rtos"; \
			echo "############################################################"; \
			$(MAKE) "allconfigs-$$board.$$rtos" || true; \
		done; \
	done

# ── Run / Flash / Debug ───────────────────────────────────────────────────

.PHONY: run
run: $(VENV_STAMP)
	@$(OVE) run $(if $(filter 1,$(HEADLESS)),--headless)

.PHONY: flash
flash: $(VENV_STAMP)
	@$(OVE) flash

# ── Tests ──────────────────────────────────────────────────────────────────

.PHONY: test
test: $(VENV_STAMP)
	@$(OVE) test

.PHONY: test-stub test-cpp test-rust test-zig test-nuttx test-zephyr
test-stub: $(VENV_STAMP)
	@$(OVE) test stub

test-cpp: $(VENV_STAMP)
	@$(OVE) test cpp

test-rust: $(VENV_STAMP)
	@$(OVE) test rust

test-zig: $(VENV_STAMP)
	@$(OVE) test zig

test-nuttx: $(VENV_STAMP)
	@$(OVE) test nuttx

test-zephyr: $(VENV_STAMP)
	@$(OVE) test zephyr

.PHONY: test-qemu test-qemu-freertos test-qemu-freertos-zeroheap test-qemu-nuttx test-qemu-nuttx-zeroheap test-qemu-zephyr test-qemu-zephyr-zeroheap
test-qemu: $(VENV_STAMP)
	@$(OVE) test qemu

test-qemu-freertos: $(VENV_STAMP)
	@$(OVE) test qemu-freertos

test-qemu-freertos-zeroheap: $(VENV_STAMP)
	@$(OVE) test qemu-freertos-zeroheap

test-qemu-nuttx: $(VENV_STAMP)
	@$(OVE) test qemu-nuttx

test-qemu-nuttx-zeroheap: $(VENV_STAMP)
	@$(OVE) test qemu-nuttx-zeroheap

test-qemu-zephyr: $(VENV_STAMP)
	@$(OVE) test qemu-zephyr

test-qemu-zephyr-zeroheap: $(VENV_STAMP)
	@$(OVE) test qemu-zephyr-zeroheap

.PHONY: test-all
test-all: $(VENV_STAMP)
	@$(OVE) test all

# ── Documentation ────────────────────────────────────────────────────────────

.PHONY: docs docs-serve docs-clean

docs: $(VENV_STAMP) ## Build complete documentation site
	@echo "==> Generating C API docs (Doxygen)..."
	@mkdir -p output/docs/doxygen
	doxygen Doxyfile
	@echo "==> Generating C++ API docs (Doxygen)..."
	@mkdir -p output/docs/doxygen-cpp
	doxygen Doxyfile.cpp
	@echo "==> Generating Rust API docs..."
	DOCS_RS=1 RUSTDOCFLAGS="--cfg docsrs" cargo doc --no-deps --features std \
		--manifest-path bindings/rust/ove/Cargo.toml
	@echo "==> Generating Zig API docs (autodoc)..."
	@mkdir -p output/docs/zig-staging
	@cp bindings/zig/ove/ove_config_docs.h output/docs/zig-staging/ove_config.h
	@ZIG=$$(find $(OVE_DIR)/output/toolchains -maxdepth 2 -name zig -type f 2>/dev/null | head -1); \
	if [ -z "$$ZIG" ]; then \
		echo "  Zig not found in toolchains, downloading..."; \
		$(VENV_PYTHON) -c "from ove.download import download_zig_toolchain; \
			from ove.manifest import load_manifest; \
			download_zig_toolchain({}, '$(OVE_DIR)/dl', '$(OVE_DIR)/output/toolchains', \
			manifest=load_manifest('$(OVE_DIR)'))"; \
		ZIG=$$(find $(OVE_DIR)/output/toolchains -maxdepth 2 -name zig -type f 2>/dev/null | head -1); \
	fi; \
	if [ -z "$$ZIG" ]; then ZIG=$$(command -v zig 2>/dev/null); fi; \
	if [ -z "$$ZIG" ]; then echo "ERROR: zig not found"; exit 1; fi; \
	echo "  Using: $$ZIG"; \
	$$ZIG build-lib bindings/zig/ove/src/root.zig \
		-femit-docs=output/docs/zig -fno-emit-bin \
		-isystem include -isystem output/docs/zig-staging
	@echo "==> Extracting Kconfig reference..."
	python3 scripts/kconfig-doc-extract.py
	@echo "==> Building MkDocs site..."
	cd docs-site && mkdocs build
	@echo "==> Copying C API (Doxygen) into site..."
	cp -r output/docs/doxygen/html docs-site/site/api/c
	@echo "==> Copying C++ API (Doxygen) into site..."
	cp -r output/docs/doxygen-cpp/html docs-site/site/api/cpp
	@echo "==> Copying rustdoc HTML into site..."
	cp -r bindings/rust/ove/target/doc docs-site/site/api/rust
	@echo "==> Copying Zig autodoc HTML into site..."
	cp -r output/docs/zig docs-site/site/api/zig
	@echo "==> Documentation built: docs-site/site/"

docs-serve: docs ## Build docs and start local preview server
	@echo "==> Serving docs at http://localhost:8000"
	cd docs-site/site && python3 -m http.server 8000

docs-clean: ## Remove generated documentation
	rm -rf output/docs docs-site/site docs-site/docs/build-system/kconfig.md

# ── Board tools ────────────────────────────────────────────────────────────

.PHONY: board-import-zephyr
board-import-zephyr: $(VENV_STAMP)
	@if [ -z "$(BOARD)" ]; then \
		echo "Error: BOARD not set. Usage: make board-import-zephyr BOARD=<name>"; \
		exit 1; \
	fi
	@$(OVE) board import --rtos zephyr --name $(BOARD) \
		$(if $(OVE_NAME),--ove-name $(OVE_NAME))

.PHONY: board-import-nuttx
board-import-nuttx: $(VENV_STAMP)
	@if [ -z "$(BOARD)" ]; then \
		echo "Error: BOARD not set. Usage: make board-import-nuttx BOARD=<name>"; \
		exit 1; \
	fi
	@$(OVE) board import --rtos nuttx --name $(BOARD) \
		$(if $(OVE_NAME),--ove-name $(OVE_NAME))

.PHONY: board-sync
board-sync: $(VENV_STAMP)
	@$(OVE) board sync $(if $(BOARD),--name $(BOARD)) $(if $(RTOS),--rtos $(RTOS))

.PHONY: board-register
board-register: $(VENV_STAMP)
	@if [ -z "$(BOARD)" ]; then \
		echo "Error: BOARD not set. Usage: make board-register BOARD=<name>"; \
		exit 1; \
	fi
	@$(OVE) board register --name $(BOARD)

.PHONY: board-list
board-list: $(VENV_STAMP)
	@$(OVE) board list

# ── Clean ──────────────────────────────────────────────────────────────────

.PHONY: clean
clean:
	@$(OVE) clean 2>/dev/null || { \
		echo "Cleaning active workspace build artifacts..."; \
		rm -rf output/*/build output/*/generated output/*/images; \
	}

.PHONY: clean-all
clean-all:
	@$(OVE) clean --all 2>/dev/null || { \
		echo "Cleaning all workspaces..."; \
		rm -rf output .config; \
	}

.PHONY: distclean
distclean:
	@echo "Cleaning everything (output, downloads, venv, config)..."
	@rm -rf output dl .venv
	@rm -f .config .config.old

# ── Help ───────────────────────────────────────────────────────────────────

.PHONY: help
help:
	@echo ""
	@echo "oveRTOS Build System"
	@echo "===================="
	@echo ""
	@echo "Configuration:  make <board>.<rtos>.<app> [ZEROHEAP=1]"
	@echo ""
	@echo "  Boards:"
	@for d in $$(find $(OVE_DIR)/boards -maxdepth 1 -mindepth 1 -type d | sort); do \
		name=$$(basename $$d); \
		rtoses=$$(find $$d -maxdepth 1 -mindepth 1 -type d -not -name src -not -name cmake | \
			  sort | xargs -I{} basename {} | tr '\n' ' '); \
		printf "    %-28s  [%s]\n" "$$name" "$$rtoses"; \
	done
	@echo ""
	@echo "  Apps:"
	@for lang in c cpp rust zig; do \
		langdir="$(OVE_DIR)/apps/$$lang"; \
		[ -d "$$langdir" ] || continue; \
		printf "    [%s]\n" "$$lang"; \
		for f in $$(find "$$langdir" -name "app.yaml" -type f | sort); do \
			cname=$$(grep 'config_name:' $$f | head -1 | sed 's/.*config_name: *//'); \
			desc=$$(grep 'description:' $$f | head -1 | sed 's/.*description: *"*//;s/"*$$//'); \
			if [ -n "$$cname" ]; then printf "      %-26s  %s\n" "$$cname" "$$desc"; fi; \
		done; \
	done
	@echo ""
	@echo "  Examples:"
	@echo "    make qemu.freertos.example_c"
	@echo "    make stm32f746.zephyr.benchmark_rust"
	@echo "    make host.posix.example_net ZEROHEAP=1"
	@echo ""
	@echo "  Other:"
	@echo "    menuconfig              - Interactive configuration (TUI)"
	@echo "    savedefconfig           - Save current config as minimal defconfig"
	@echo "    nuttx-menuconfig        - NuttX native kernel menuconfig"
	@echo "    zephyr-menuconfig       - Zephyr native kernel menuconfig"
	@echo ""
	@echo "Build:"
	@echo "  all (default)           - Full pipeline: download, configure, build"
	@echo "  download                - Download RTOS sources to dl/"
	@echo "  configure               - Generate config files from .config"
	@echo "  allconfigs-<board>.<rtos> - Build all apps for a board/RTOS pair"
	@echo "  alldefconfigs           - Build every configuration (all boards/RTOSes)"
	@echo "  setup                   - (Re)create Python venv and install CLI"
	@echo "  flash                   - Flash firmware to target board"
	@echo "  run                     - Run firmware (QEMU or POSIX)"
	@echo "  run HEADLESS=1          - Run firmware without display viewer"
	@echo ""
	@echo "Tests:"
	@echo "  test                    - Run sim tests (stub, cpp, rust, zig, nuttx, zephyr)"
	@echo "  test-stub               - Stub backend tests"
	@echo "  test-cpp                - C++ binding tests"
	@echo "  test-rust               - Rust binding tests"
	@echo "  test-zig                - Zig binding tests"
	@echo "  test-nuttx              - NuttX simulator tests"
	@echo "  test-zephyr             - Zephyr native_sim tests"
	@echo "  test-qemu               - All QEMU ARM tests"
	@echo "  test-qemu-freertos      - FreeRTOS QEMU ARM tests"
	@echo "  test-qemu-freertos-zeroheap - FreeRTOS QEMU ARM tests (zero-heap)"
	@echo "  test-qemu-nuttx         - NuttX QEMU ARM tests"
	@echo "  test-qemu-nuttx-zeroheap  - NuttX QEMU ARM tests (zero-heap)"
	@echo "  test-qemu-zephyr        - Zephyr QEMU ARM tests"
	@echo "  test-qemu-zephyr-zeroheap - Zephyr QEMU ARM tests (zero-heap)"
	@echo "  test-all                - All tests (sim + QEMU)"
	@echo ""
	@echo "Documentation:"
	@echo "  docs                    - Build complete documentation site"
	@echo "  docs-serve              - Build docs and start local preview server"
	@echo "  docs-clean              - Remove generated documentation"
	@echo ""
	@echo "Board tools:"
	@echo "  board-import-zephyr BOARD=<name>  - Import board from Zephyr"
	@echo "  board-import-nuttx  BOARD=<name>  - Import board from NuttX"
	@echo "  board-register BOARD=<name>       - Re-run post-import registration"
	@echo "  board-sync [BOARD=<name>]         - Sync board.yaml to RTOS configs"
	@echo "  board-list                        - List all boards"
	@echo ""
	@echo "Clean:"
	@echo "  clean                   - Clean active workspace"
	@echo "  clean-all               - Remove all workspaces (output/)"
	@echo "  distclean               - Full reset (output/, dl/, .venv, .config)"
	@echo ""
	@echo "CLI:"
	@echo "  ove <command>       - Direct CLI usage (after 'make setup')"
	@echo ""

# Default target
.DEFAULT_GOAL := all
