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

# Fragment-based configuration: make <board>.<rtos>.<app>
# Examples:
#   make qemu.freertos.example_c
#   make stm32f746.zephyr.benchmark_rust
#   make host.posix.example_net_cpp_zh
#
# Detect dot-separated targets in MAKECMDGOALS and generate rules for them.
# Exclude paths (slashes) — `setup`'s recursive make passes the absolute
# $(VENV_STAMP) path through MAKECMDGOALS, which would otherwise match here
# and shadow the venv-build rule. Also exclude allconfigs-* / alldefconfigs
# which have their own pattern rules.
_DOT_TARGETS := $(foreach t,$(MAKECMDGOALS),$(if $(findstring /,$(t)),,$(if $(findstring .,$(t)),$(if $(filter allconfigs-% alldefconfigs,$(t)),,$(t)))))
ifneq ($(_DOT_TARGETS),)
# Each goal calls `ove defconfig-fragments`, which rewrites the single shared
# .config. Running multiple goals in parallel would race on that file, so
# serialize when we see more than one dot-target on the command line.
.NOTPARALLEL:
.PHONY: $(_DOT_TARGETS)
$(_DOT_TARGETS): $(VENV_STAMP)
	@$(OVE) defconfig-fragments "$@"
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
	@$(OVE) download $(if $(filter 1,$(DRYRUN)),--dry-run)

.PHONY: configure
configure: $(VENV_STAMP)
	@$(OVE) configure $(if $(filter 1,$(DRYRUN)),--dry-run)

.PHONY: build
build: $(VENV_STAMP)
	@$(OVE) build $(if $(filter 1,$(JSON)),--json) $(if $(filter 1,$(DRYRUN)),--dry-run)

.PHONY: all
all: $(VENV_STAMP)
	@$(OVE) download
	@$(OVE) configure
	@$(OVE) build $(if $(filter 1,$(JSON)),--json) $(if $(filter 1,$(DRYRUN)),--dry-run)

# Build all app configurations for a given board.rtos pair.
# Usage: make allconfigs-host.posix
#        make allconfigs-qemu.freertos
#        make allconfigs-stm32f746.zephyr
.PHONY: allconfigs-%
allconfigs-%: $(VENV_STAMP)
	@$(OVE) allconfigs "$*"

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
	@$(OVE) run $(if $(filter 1,$(HEADLESS)),--headless) $(EXTRA)

.PHONY: flash
flash: $(VENV_STAMP)
	@$(OVE) flash

# ── Tests ──────────────────────────────────────────────────────────────────

.PHONY: test
test: $(VENV_STAMP)
	@$(OVE) test $(if $(filter 1,$(JSON)),--json)

# ── Benchmarks ─────────────────────────────────────────────────────────────
# `make benchmarks-<platform>` — build all 4 bindings (c, cpp, rust, zig)
# of the benchmark app for the given platform, run each, and emit a
# cross-binding comparison report.  See `ove benchmarks --help`.
#
#   posix                  — runs locally, writes report to
#                            output/host/posix/_benchmarks/report.md
#   stm32f746g-discovery   — flashes via openocd and tails the
#                            picocom-written serial log
#                            ($OVE_SERIAL_LOG, default /tmp/serial.log).
#                            Report at
#                            output/stm32f746/freertos/_benchmarks/report.md
#
# Use SKIPBUILD=1 to skip the per-binding builds (re-flash + re-run only).
# Use ZEROHEAP=1 to build and run the *_zh variants and write the
# generated report to docs-site/docs/benchmarks/<rtos>-zeroheap.md.
.PHONY: benchmarks-%
benchmarks-%: $(VENV_STAMP)
	@$(OVE) benchmarks \
		$(if $(filter 1,$(SKIPBUILD)),--skip-build) \
		$(if $(filter 1,$(ZEROHEAP)),--zeroheap) \
		$(foreach b,$(BINDING),--binding $(b)) \
		"$*"

# Single source of truth for `test-<name>` recipes — each delegates to
# `ove test <name>`. Add new suites here, not as separate targets.
TEST_NAMES := stub stub-sanitize stub-sanitize-zh stub-tsan stub-msan \
              cpp cpp-sanitize cpp-sanitize-zh cpp-tsan rust rust-zeroheap \
              zig zig-zeroheap zig-debug lvgl-compile nuttx zephyr \
              qemu qemu-freertos qemu-freertos-zeroheap \
              qemu-nuttx qemu-nuttx-zeroheap \
              qemu-zephyr qemu-zephyr-zeroheap all \
              renode renode-stm32f746-freertos \
              renode-stm32f746-freertos-zeroheap \
              renode-stm32f746-zephyr \
              renode-stm32f746-zephyr-zeroheap \
              renode-stm32f746-nuttx \
              renode-stm32f746-nuttx-zeroheap \
              hw hw-stm32f746-freertos \
              hw-stm32f746-freertos-zeroheap \
              hw-stm32f746-zephyr \
              hw-stm32f746-zephyr-zeroheap \
              hw-stm32f746-nuttx \
              hw-stm32f746-nuttx-zeroheap \
              rust-coverage zig-coverage nuttx-coverage zephyr-coverage \
              qemu-freertos-coverage qemu-nuttx-coverage qemu-zephyr-coverage
.PHONY: $(addprefix test-,$(TEST_NAMES))
$(addprefix test-,$(TEST_NAMES)): test-%: $(VENV_STAMP)
	@$(OVE) test $* $(if $(filter 1,$(JSON)),--json)

ASAN_BUILD_DIR := $(OVE_DIR)/output/tests/stub_asan

.PHONY: asan
asan: $(VENV_STAMP)
	@cmake -S $(OVE_DIR)/tests -B $(ASAN_BUILD_DIR)
	@cmake --build $(ASAN_BUILD_DIR) --target ove_test_stub_asan -j$$(nproc)
	@$(ASAN_BUILD_DIR)/ove_test_stub_asan

COVERAGE_OUTPUT_DIR         := $(OVE_DIR)/output/tests/coverage
COVERAGE_STUB_DIR           := $(OVE_DIR)/output/tests/stub_coverage
COVERAGE_CPP_DIR            := $(OVE_DIR)/output/tests/cpp_coverage
COVERAGE_RUST_DIR           := $(OVE_DIR)/output/tests/rust_coverage
COVERAGE_ZEPHYR_DIR         := $(OVE_DIR)/output/tests/zephyr_coverage
COVERAGE_NUTTX_DIR          := $(OVE_DIR)/output/tests/nuttx_coverage
COVERAGE_ZIG_DIR            := $(OVE_DIR)/output/tests/zig_coverage
COVERAGE_QEMU_FREERTOS_DIR  := $(OVE_DIR)/output/tests/qemu_freertos_coverage
COVERAGE_QEMU_NUTTX_DIR     := $(OVE_DIR)/output/tests/qemu_nuttx_coverage
COVERAGE_QEMU_ZEPHYR_DIR    := $(OVE_DIR)/output/tests/qemu_zephyr_coverage

# Each backend emits a filtered lcov tracefile; the top-level `coverage`
# target merges them into one combined HTML report so the headline number
# reflects every test target, not just the C stub suites.
#
#   `make coverage`                                        — everything instrumented (default)
#   `make coverage WITH_QEMU_FREERTOS=0 WITH_QEMU_NUTTX=0 \
#     WITH_QEMU_ZEPHYR=0 WITH_ZEPHYR=0 WITH_NUTTX=0 \
#     WITH_ZIG=0`                                          — host-only (stub + cpp + rust, fastest)
#
# Backends already wired:
#   - stub (C / gcc / gcov)             via tests/CMakeLists.txt
#   - cpp  (C++ / g++ / gcov)           via tests/cpp/CMakeLists.txt
#   - rust (LLVM src-based)             via `ove test rust-coverage`
#   - zephyr native_sim (gcov)          via `ove test zephyr-coverage`        (opt-in)
#   - nuttx sim (gcov)                  via `ove test nuttx-coverage`         (opt-in)
#   - zig   (kcov/DWARF)                via `ove test zig-coverage`           (opt-in;
#             kcov built locally from manifest, see _ensure_kcov in test.py)
#   - freertos QEMU (arm-none-eabi-gcov) via `ove test qemu-freertos-coverage` (opt-in)
#   - nuttx    QEMU (arm-none-eabi-gcov) via `ove test qemu-nuttx-coverage`    (opt-in)
#   - zephyr   QEMU (arm-zephyr-eabi-gcov, from Zephyr SDK) via
#             `ove test qemu-zephyr-coverage` (opt-in)
# Every instrumented backend runs by default so the headline coverage
# number reflects the full test matrix. Opt out per-backend with e.g.
# `make coverage WITH_QEMU_FREERTOS=0` when you need a faster pass.
WITH_ZEPHYR         ?= 1
WITH_NUTTX          ?= 1
WITH_ZIG            ?= 1
WITH_QEMU_FREERTOS  ?= 1
WITH_QEMU_NUTTX     ?= 1
WITH_QEMU_ZEPHYR    ?= 1

# Coverage entry points.
#
# `make test-stub-coverage` / `make test-cpp-coverage` wrap the cmake
# invocations the CI workflow used to inline.  They sit outside the
# auto-generated `test-<name>` pattern (which delegates to `$(OVE) test
# <name>`) because the stub/cpp coverage builds are plain CMake projects
# that do not need the `ove` RTOS-orchestration layer.  The rest of the
# coverage backends (rust/zephyr/nuttx/zig/qemu-*) DO need that layer
# and continue to flow through `$(OVE) test <name>-coverage` via the
# pattern rule above.
.PHONY: test-stub-coverage
test-stub-coverage:
	@cmake -S $(OVE_DIR)/tests -B $(COVERAGE_STUB_DIR) -DOVE_TEST_BUILD_COVERAGE=ON
	@cmake --build $(COVERAGE_STUB_DIR) --target coverage -j$$(nproc)

.PHONY: test-cpp-coverage
test-cpp-coverage:
	@cmake -S $(OVE_DIR)/tests/cpp -B $(COVERAGE_CPP_DIR) -DOVE_TEST_BUILD_COVERAGE=ON
	@cmake --build $(COVERAGE_CPP_DIR) --target coverage -j$$(nproc)

.PHONY: coverage
coverage: $(VENV_STAMP)
	@command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1 || { \
		echo ""; \
		echo "ERROR: lcov/genhtml not found."; \
		echo ""; \
		echo "  Ubuntu/Debian:  sudo apt install lcov"; \
		echo ""; \
		exit 1; \
	}
	@$(MAKE) --no-print-directory test-stub-coverage
	@$(MAKE) --no-print-directory test-cpp-coverage
	@$(OVE) test rust-coverage
	@if [ "$(WITH_ZEPHYR)" = "1" ];        then $(OVE) test zephyr-coverage;        fi
	@if [ "$(WITH_NUTTX)" = "1" ];         then $(OVE) test nuttx-coverage;         fi
	@if [ "$(WITH_ZIG)" = "1" ];           then $(OVE) test zig-coverage;           fi
	@if [ "$(WITH_QEMU_FREERTOS)" = "1" ]; then $(OVE) test qemu-freertos-coverage; fi
	@if [ "$(WITH_QEMU_NUTTX)" = "1" ];    then $(OVE) test qemu-nuttx-coverage;    fi
	@if [ "$(WITH_QEMU_ZEPHYR)" = "1" ];   then $(OVE) test qemu-zephyr-coverage;   fi
	@$(MAKE) --no-print-directory coverage-merge

# Final lcov-merge + genhtml + summary, factored out so CI can invoke it
# after downloading per-backend .filtered.info artifacts. Skips any
# tracefile that isn't present — lets the merge produce a partial report
# when a backend job fails upstream.
.PHONY: coverage-merge
coverage-merge:
	@command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1 || { \
		echo "ERROR: lcov/genhtml not found (Ubuntu/Debian: sudo apt install lcov)"; \
		exit 1; \
	}
	@mkdir -p $(COVERAGE_OUTPUT_DIR)
	@lcov \
		$(if $(wildcard $(COVERAGE_STUB_DIR)/coverage/coverage.filtered.info),--add-tracefile $(COVERAGE_STUB_DIR)/coverage/coverage.filtered.info) \
		$(if $(wildcard $(COVERAGE_CPP_DIR)/coverage/coverage.filtered.info),--add-tracefile $(COVERAGE_CPP_DIR)/coverage/coverage.filtered.info) \
		$(if $(wildcard $(COVERAGE_RUST_DIR)/coverage.filtered.info),--add-tracefile $(COVERAGE_RUST_DIR)/coverage.filtered.info) \
		$(if $(filter 1,$(WITH_ZEPHYR)),$(if $(wildcard $(COVERAGE_ZEPHYR_DIR)/coverage.filtered.info),--add-tracefile $(COVERAGE_ZEPHYR_DIR)/coverage.filtered.info)) \
		$(if $(filter 1,$(WITH_NUTTX)),$(if $(wildcard $(COVERAGE_NUTTX_DIR)/coverage.filtered.info),--add-tracefile $(COVERAGE_NUTTX_DIR)/coverage.filtered.info)) \
		$(if $(filter 1,$(WITH_ZIG)),$(if $(wildcard $(COVERAGE_ZIG_DIR)/coverage.filtered.info),--add-tracefile $(COVERAGE_ZIG_DIR)/coverage.filtered.info)) \
		$(if $(filter 1,$(WITH_QEMU_FREERTOS)),$(if $(wildcard $(COVERAGE_QEMU_FREERTOS_DIR)/coverage.filtered.info),--add-tracefile $(COVERAGE_QEMU_FREERTOS_DIR)/coverage.filtered.info)) \
		$(if $(filter 1,$(WITH_QEMU_NUTTX)),$(if $(wildcard $(COVERAGE_QEMU_NUTTX_DIR)/coverage.filtered.info),--add-tracefile $(COVERAGE_QEMU_NUTTX_DIR)/coverage.filtered.info)) \
		$(if $(filter 1,$(WITH_QEMU_ZEPHYR)),$(if $(wildcard $(COVERAGE_QEMU_ZEPHYR_DIR)/coverage.filtered.info),--add-tracefile $(COVERAGE_QEMU_ZEPHYR_DIR)/coverage.filtered.info)) \
		--rc branch_coverage=1 \
		--output-file   $(COVERAGE_OUTPUT_DIR)/coverage.info \
		--ignore-errors inconsistent,format,empty
	@genhtml $(COVERAGE_OUTPUT_DIR)/coverage.info \
		--branch-coverage \
		--output-directory $(COVERAGE_OUTPUT_DIR)/html \
		--ignore-errors source,mismatch
	@lcov --summary $(COVERAGE_OUTPUT_DIR)/coverage.info \
		--rc branch_coverage=1 \
		--ignore-errors inconsistent,format,empty || true
	@echo ""
	@echo "Combined coverage report: $(COVERAGE_OUTPUT_DIR)/html/index.html"

# ── Quality / CI ──────────────────────────────────────────────────────────

.PHONY: doctor
doctor: $(VENV_STAMP)
	@$(OVE) doctor $(if $(filter 1,$(JSON)),--json)

.PHONY: lint
lint: $(VENV_STAMP)
	@$(OVE) ensure-toolchain zig
	@$(OVE) lint
	@# Non-blocking proactive drift check: warns if bindings_stub.rs is
	@# stale relative to a fresh bindgen run.  The script always exits 0
	@# in --check mode, but `|| true` is belt-and-braces.
	@$(OVE_DIR)/scripts/regen-bindings-stub.sh --check || true
	@# LVGL cross-binding parity — GATING.  The whole backlog has been
	@# triaged: intentional/idiomatic gaps are whitelisted with a rationale
	@# in tests/audit/lvgl_parity_whitelist.txt, and the checker's staleness
	@# guard fails if a whitelist line stops matching a real gap.  A NEW
	@# cross-binding drift (or a stale whitelist entry) now fails lint here.
	@# Genuine drops still pending a build-verified binding fix are tracked
	@# under the whitelist's "GENUINE DROPS" section.  `... --verbose` lists
	@# every gap; remove a line as you close its gap.
	@python3 $(OVE_DIR)/scripts/lvgl_parity_check.py --strict

# Run clang-tidy specifically against cross-compile backend code.  Reuses
# any existing output/<board>/<rtos>/<app>/build/firmware/compile_commands.json
# (or output/tests/{qemu,renode}-<rtos>*/build/compile_commands.json) and
# scopes to backends/<rtos>/*.c.  SKIPs cleanly if no firmware build is
# present — bootstrap one first via e.g. `make stm32f746.freertos.benchmark_c`.
.PHONY: lint-backends
lint-backends: $(VENV_STAMP)
	@$(OVE) lint --only clang-tidy-backends

.PHONY: format
format: $(VENV_STAMP)
	@$(OVE) format

.PHONY: ci
ci: $(VENV_STAMP)
	@$(OVE) ci $(if $(filter 1,$(KEEPGOING)),--keep-going)

# Run Miri (Rust UB detector) over the binding's pure-Rust unit tests.
# Uses DOCS_RS=1 to take build.rs's stub-bindings path so we don't need a
# configured C workspace; FFI-calling tests live in tests/rust and are
# unreachable under Miri by design (Miri can't execute extern "C" calls
# into the stub library).  Requires nightly + the `miri` component.
.PHONY: miri
miri:
	@DOCS_RS=1 cargo +nightly miri test \
		--manifest-path bindings/rust/ove/Cargo.toml \
		--features std

# Run GCC -fanalyzer over the C source tree.  Closest C analog of Rust's
# Miri / C++'s UBSan+ASan job: catches null deref, use-after-free,
# double-free, leak-on-realloc, uninitialized-read defects via abstract
# interpretation rather than runtime checks.  Output is filtered to
# ove-only paths because the third-party cmocka harness has known
# analyzer false-positives that aren't actionable here.  Run on demand;
# not gated in CI to avoid noise from any future toolchain bump.
.PHONY: c-analyze
c-analyze:
	@cmake -B output/tests/c_analyzer -S tests \
		-DCMAKE_C_COMPILER=gcc \
		-DCMAKE_C_FLAGS="-fanalyzer -Wno-analyzer-too-complex" \
		-DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -5
	@cmake --build output/tests/c_analyzer -j$$(nproc) 2>&1 \
		| grep -E "(include/ove|backends|tests/include|tests/src).*analyzer-" \
		| (grep . && echo "FAIL: see analyzer findings above" && exit 1) \
		|| echo "OK: no -fanalyzer findings in ove sources"

.PHONY: manifest
manifest: $(VENV_STAMP)
	@$(OVE) manifest $(if $(filter 1,$(CHECK)),--check)

# Shell completion scripts — `make completion-bash > ~/.bash_completion.d/ove`.
.PHONY: completion-%
completion-%: $(VENV_STAMP)
	@$(OVE) completion $*

# Workspace-independent toolchain fetch — `make ensure-toolchain-zig`.
.PHONY: ensure-toolchain-%
ensure-toolchain-%: $(VENV_STAMP)
	@$(OVE) ensure-toolchain $*

# ── Documentation ────────────────────────────────────────────────────────────

.PHONY: docs docs-serve docs-clean

docs: $(VENV_STAMP) ## Build complete documentation site
	@echo "==> Generating Doxyfile.predefined from Kconfig..."
	python3 scripts/kconfig_doxyfile_gen.py
	@echo "==> Generating C API docs (Doxygen)..."
	@mkdir -p output/docs/doxygen
	doxygen Doxyfile
	@echo "==> Generating C++ API docs (Doxygen)..."
	@mkdir -p output/docs/doxygen-cpp
	doxygen Doxyfile.cpp
	@echo "==> Generating Rust API docs..."
	# DOCS_RS=1 sets the `docsrs` cfg via the ove crate's build.rs (it
	# emits `cargo:rustc-cfg=docsrs` itself). We deliberately do NOT
	# pass `--cfg docsrs` in RUSTDOCFLAGS — that would propagate to
	# every transitive dep, and some (e.g. proc-macro2) gate
	# `#![feature(doc_cfg)]` on `cfg(docsrs)` which needs nightly.
	DOCS_RS=1 cargo doc \
		--features std,async,async-net,async-net-qemu-shm,async-net-stm32f7-eth,embedded-hal,embedded-hal-async,embedded-io,embedded-io-async,fugit,portable-atomic-arc \
		--manifest-path bindings/rust/ove/Cargo.toml
	@echo "==> Generating Zig API docs (autodoc)..."
	@mkdir -p output/docs/zig-staging
	@cp bindings/zig/ove/ove_config_docs.h output/docs/zig-staging/ove_config.h
	@$(MAKE) ensure-toolchain-zig
	@ZIG=$$(find $(OVE_DIR)/output/toolchains -maxdepth 2 -name zig -type f 2>/dev/null | head -1); \
	if [ -z "$$ZIG" ]; then ZIG=$$(command -v zig 2>/dev/null); fi; \
	if [ -z "$$ZIG" ]; then echo "ERROR: zig not found"; exit 1; fi; \
	echo "  Using: $$ZIG"; \
	$$ZIG build-lib bindings/zig/ove/src/root.zig \
		-femit-docs=output/docs/zig -fno-emit-bin \
		-isystem include -isystem output/docs/zig-staging
	@echo "==> Extracting Kconfig reference..."
	python3 scripts/kconfig_doc_extract.py
	@echo "==> Building MkDocs site..."
	cd docs-site && mkdocs build
	@echo "==> Copying C API (Doxygen) into site..."
	cp -r output/docs/doxygen/html docs-site/site/api/c
	@echo "==> Copying C++ API (Doxygen) into site..."
	cp -r output/docs/doxygen-cpp/html docs-site/site/api/cpp
	@echo "==> Copying rustdoc HTML into site..."
	cp -r output/cargo/doc docs-site/site/api/rust
	@echo "==> Copying Zig autodoc HTML into site..."
	cp -r output/docs/zig docs-site/site/api/zig
	@echo "==> Documentation built: docs-site/site/"

docs-serve: docs ## Build docs and start local preview server
	@echo "==> Serving docs at http://localhost:8000"
	cd docs-site/site && python3 -m http.server 8000

docs-clean: ## Remove generated documentation
	rm -rf output/docs docs-site/site docs-site/docs/build-system/kconfig.md

# ── IDE integration ────────────────────────────────────────────────────────

.PHONY: vscode
vscode: $(VENV_STAMP)
	@$(OVE) vscode $(if $(filter 1,$(NO_OPEN)),--no-open)

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
	@$(OVE) clean --dist 2>/dev/null || { \
		echo "Cleaning everything (output, downloads, venv, config)..."; \
		rm -rf output dl .venv; \
		rm -f .config .config.old; \
	}

# ── Help ───────────────────────────────────────────────────────────────────

.PHONY: help
help:
	@echo ""
	@echo "oveRTOS Build System"
	@echo "===================="
	@echo ""
	@echo "Configuration:  make <board>.<rtos>.<app>"
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
	@echo "    make host.posix.example_net_zh"
	@echo ""
	@echo "  Other:"
	@echo "    menuconfig              - Interactive configuration (TUI)"
	@echo "    savedefconfig           - Save current config as minimal defconfig"
	@echo "    nuttx-menuconfig        - NuttX native kernel menuconfig"
	@echo "    zephyr-menuconfig       - Zephyr native kernel menuconfig"
	@echo ""
	@echo "Build:"
	@echo "  all (default)           - Full pipeline: download, configure, build"
	@echo "  build                   - Build firmware only (skip download/configure)"
	@echo "  download                - Download RTOS sources to dl/"
	@echo "  configure               - Generate config files from .config"
	@echo "  allconfigs-<board>.<rtos> - Build all apps for a board/RTOS pair"
	@echo "  alldefconfigs           - Build every configuration (all boards/RTOSes)"
	@echo "  setup                   - (Re)create Python venv and install CLI"
	@echo "  flash                   - Flash firmware to target board"
	@echo "  run                     - Run firmware (QEMU or POSIX)"
	@echo "  run HEADLESS=1          - Run firmware without display viewer"
	@echo "  run EXTRA=\"<args>\"      - Forward extra args to runner (e.g. QEMU flags)"
	@echo ""
	@echo "  Build flags:"
	@echo "    DRYRUN=1              - Dry-run for download/configure/build"
	@echo "    JSON=1                - Emit JSON summary for build/test/doctor"
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
	@echo "  test-renode             - All Renode STM32F746 targets (FreeRTOS / Zephyr / NuttX)"
	@echo "  test-hw-stm32f746-<rtos>{,-zeroheap} - Manual HIL on real Discovery board."
	@echo "                            Requires OVE_HW_SERIAL_PORT=/dev/ttyACMx and"
	@echo "                            an installed openocd; never run by test-all/CI."
	@echo "  test-all                - All tests (sim + QEMU + Renode; excludes HW)"
	@echo "  asan                    - Build and run stub with AddressSanitizer + UBSan"
	@echo "  coverage                - Combined HTML coverage (stub + cpp + rust; needs lcov)"
	@echo "  coverage WITH_ZEPHYR=1  - Also include Zephyr native_sim (slow)"
	@echo "  coverage WITH_NUTTX=1   - Also include NuttX sim (slow)"
	@echo "  coverage WITH_ZIG=1     - Also include Zig (builds kcov locally)"
	@echo ""
	@echo "Quality / CI:"
	@echo "  doctor                  - Check host environment (toolchains, deps, venv)"
	@echo "  doctor JSON=1           - Emit JSON diagnostic report"
	@echo "  lint                    - Formatters (check) + correctness linters"
	@echo "                            (clang-format -n, clang-tidy, cargo fmt --check,"
	@echo "                             cargo clippy, zig fmt --check, zig ast-check,"
	@echo "                             ruff, backend-struct guard)"
	@echo "  format                  - Apply formatters in place (no linters)"
	@echo "  ci                      - Run pre-merge gates (doctor + lint + test all)"
	@echo "  ci KEEPGOING=1          - Keep running CI stages after a failure"
	@echo "  manifest                - Show manifest versions and integrity status"
	@echo "  manifest CHECK=1        - Exit non-zero if manifest has uncommitted changes"
	@echo "  completion-bash|zsh|fish  - Emit shell completion script on stdout"
	@echo "  ensure-toolchain-zig    - Install the zig toolchain"
	@echo ""
	@echo "Documentation:"
	@echo "  docs                    - Build complete documentation site"
	@echo "  docs-serve              - Build docs and start local preview server"
	@echo "  docs-clean              - Remove generated documentation"
	@echo ""
	@echo "IDE:"
	@echo "  vscode                  - Generate .vscode/ for the active workspace and launch VSCode"
	@echo "  vscode NO_OPEN=1        - Generate .vscode/ only; do not launch 'code'"
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
	@echo "Scaffolding:"
	@echo "  ove app new --lang {c,cpp,rust,zig} --name <name>"
	@echo "                          - Stamp a new external app from the bundled template"
	@echo ""
	@echo "CLI:"
	@echo "  ove <command>       - Direct CLI usage (after 'make setup')"
	@echo ""
	@echo "New to oveRTOS?  Visit the quickstart:"
	@echo "  https://varcain.github.io/oveRTOS/getting-started/quickstart/"
	@echo "Hit a wall?  Run 'make doctor' or browse:"
	@echo "  https://varcain.github.io/oveRTOS/getting-started/troubleshooting/"
	@echo ""

# Default target
.DEFAULT_GOAL := all
