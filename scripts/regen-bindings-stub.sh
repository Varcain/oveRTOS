#!/usr/bin/env bash
# Regenerate bindings/rust/ove/src/bindings_stub.rs from a comprehensive
# bindgen run (all CONFIG_OVE_* enabled). The stub ships in-tree so docs.rs
# builds — and any consumer setting `cfg(docsrs)` — can compile the binding
# crate without running bindgen against a real C toolchain + LVGL workspace.
#
# Two modes:
#   (no args)   Regenerate bindings_stub.rs in place from a fresh bindgen
#               run.  Run after adding/changing a C FFI symbol; commit
#               the diff.
#   --check     Run bindgen and compare against the in-tree stub.  Prints
#               a warning on drift but ALWAYS exits 0 — wired into
#               `make lint` to surface stale stubs without blocking the
#               lint pass.
#
# A second drift catch is the cfg(docsrs) clippy run inside `make lint`,
# which fails-fast on symbols the Rust binding references but the stub
# lacks.  `--check` catches the inverse direction too: bindgen surface
# present in real builds but missing from the stub (e.g. a new C
# function added without yet writing the Rust wrapper for it).
set -euo pipefail

MODE="regen"
if [ "${1:-}" = "--check" ]; then
    MODE="check"
    shift
fi

OVE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# All-modules-on config — fed only to bindgen, never compiled into a real
# build, so disabled-by-default modules (NET, INFER, UART, …) all reach
# the stub. Storage-type sizes get filled with placeholders since their
# real values are platform-dependent and only matter for the in-tree build.
cat > "$TMP/ove_config.h" <<'CFG'
#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H
#define CONFIG_OVE_RTOS_POSIX 1
#define CONFIG_OVE_APP 1
#define CONFIG_OVE_THREAD 1
#define CONFIG_OVE_SYNC 1
#define CONFIG_OVE_QUEUE 1
#define CONFIG_OVE_TIMER 1
#define CONFIG_OVE_EVENTGROUP 1
#define CONFIG_OVE_LOG 1
#define CONFIG_OVE_AUDIO 1
#define CONFIG_OVE_FS 1
#define CONFIG_OVE_LVGL 1
#define CONFIG_OVE_NVS 1
#define CONFIG_OVE_SHELL 1
#define CONFIG_OVE_WATCHDOG 1
#define CONFIG_OVE_BSP 1
#define CONFIG_OVE_BOARD 1
#define CONFIG_OVE_GPIO 1
#define CONFIG_OVE_LED 1
#define CONFIG_OVE_TIME 1
#define CONFIG_OVE_CONSOLE 1
#define CONFIG_OVE_STREAM 1
#define CONFIG_OVE_WORKQUEUE 1
#define CONFIG_OVE_INFER 1
#define CONFIG_OVE_NET 1
#define CONFIG_OVE_NET_TLS 1
#define CONFIG_OVE_NET_HTTP 1
#define CONFIG_OVE_NET_MQTT 1
#define CONFIG_OVE_NET_HTTPD 1
#define CONFIG_OVE_NET_SNTP 1
#define CONFIG_OVE_NET_HTTPD_WS 1
#define CONFIG_OVE_UART 1
#define CONFIG_OVE_SPI 1
#define CONFIG_OVE_I2C 1
#define CONFIG_OVE_I2S 1
#define CONFIG_OVE_PM 1
#define CONFIG_OVE_PM_MAX_WAKE_SOURCES 8
#define CONFIG_OVE_PM_MAX_NOTIFIERS 4
#define CONFIG_OVE_APP_NAME "stub"
#define CONFIG_OVE_APP_VERSION "0.0.0"
#define OVE_LOG_LEVEL 0
#endif
CFG

# Storage sizes the build script consumes. Existing tests-stub values plus
# placeholders for net/infer/uart/spi/i2c/i2s storage types absent from
# the in-tree probe (those modules are off in tests/ove_config.h).
STUB_SIZES="$OVE_DIR/output/tests/rust_stub/ove_storage_sizes.env"
if [ ! -f "$STUB_SIZES" ]; then
    if [ "$MODE" = "check" ]; then
        echo "warning: rust_stub build not present at $STUB_SIZES — skipping bindings_stub.rs drift check" >&2
        exit 0
    fi
    echo "Need rust_stub built first: run \`ove test rust\` or \`make lint\`" >&2
    exit 1
fi
cp "$STUB_SIZES" "$TMP/sizes.env"
cat >> "$TMP/sizes.env" <<'SIZES'
ALIGNOF_OVE_HTTP_CLIENT_STORAGE=8
ALIGNOF_OVE_I2C_STORAGE=8
ALIGNOF_OVE_I2S_STORAGE=8
ALIGNOF_OVE_MODEL_STORAGE=8
ALIGNOF_OVE_MQTT_CLIENT_STORAGE=8
ALIGNOF_OVE_NETIF_STORAGE=8
ALIGNOF_OVE_SOCKET_STORAGE=8
ALIGNOF_OVE_SPI_STORAGE=8
ALIGNOF_OVE_TLS_STORAGE=8
ALIGNOF_OVE_UART_STORAGE=8
SIZEOF_OVE_HTTP_CLIENT_STORAGE=64
SIZEOF_OVE_I2C_STORAGE=64
SIZEOF_OVE_I2S_STORAGE=64
SIZEOF_OVE_MODEL_STORAGE=64
SIZEOF_OVE_MQTT_CLIENT_STORAGE=128
SIZEOF_OVE_NETIF_STORAGE=64
SIZEOF_OVE_SOCKET_STORAGE=64
SIZEOF_OVE_SPI_STORAGE=64
SIZEOF_OVE_TLS_STORAGE=128
SIZEOF_OVE_UART_STORAGE=64
SIZES

# `cargo build` will fail at lib compile time (the lib has unsafe-block
# diffs against the comprehensive bindings) — that's fine; we just need
# build.rs to emit ove_bindings.rs.
OVE_DIR="$OVE_DIR" \
OVE_GEN_DIR="$TMP" \
RUST_IS_NATIVE=1 \
OVE_STORAGE_SIZES="$TMP/sizes.env" \
LV_CONF_PATH="$OVE_DIR/boards/host/posix" \
LVGL_INCLUDE_PATH="$OVE_DIR/tests/backends/stub/lvgl" \
LVGL_PARENT_PATH="$OVE_DIR/tests/backends/stub" \
CARGO_TARGET_DIR="$TMP/target" \
cargo build --manifest-path "$OVE_DIR/bindings/rust/ove/Cargo.toml" \
    --features std --locked >/dev/null 2>&1 || true

BINDINGS=$(find "$TMP/target" -name ove_bindings.rs | head -1)
if [ -z "$BINDINGS" ]; then
    if [ "$MODE" = "check" ]; then
        echo "warning: bindgen never ran (build.rs failed) — skipping bindings_stub.rs drift check" >&2
        exit 0
    fi
    echo "bindgen never ran — check build.rs failed earlier" >&2
    exit 1
fi

REAL="$OVE_DIR/bindings/rust/ove/src/bindings_stub.rs"
if [ "$MODE" = "check" ]; then
    OUT="$TMP/bindings_stub.new.rs"
else
    OUT="$REAL"
fi
python3 - "$BINDINGS" "$OUT" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
# Drop layout assertions (need real ::std::mem and platform-specific sizes)
text = re.sub(
    r"#\[allow\(clippy::unnecessary_operation, clippy::identity_op\)\]\s*\n"
    r"const _: \(\) = \{[^;]*?\};",
    "", text, flags=re.DOTALL)
text = re.sub(r"const _: \(\) = \{[^;]*?\};", "", text, flags=re.DOTALL)
# core::ffi for no_std stub
text = re.sub(r"::std::os::raw::", "core::ffi::", text)
text = re.sub(r"::std::option::Option", "Option", text)
text = re.sub(r"::std::mem::", "core::mem::", text)

header = '''// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Stub FFI bindings for docs.rs builds.
//!
//! Used in place of the bindgen-generated `ove_bindings.rs` when
//! building documentation on docs.rs (detected via `DOCS_RS` env var
//! and the `docsrs` cfg flag). Mirrors what bindgen would emit so
//! `cargo doc` (and `clippy --cfg docsrs`) compile without a real C
//! toolchain or LVGL headers.
//!
//! Regenerate after touching the C FFI surface:
//!     scripts/regen-bindings-stub.sh
//! `make lint` then runs clippy with cfg(docsrs) to catch drift.

#![allow(
    non_upper_case_globals,
    non_camel_case_types,
    non_snake_case,
    dead_code
)]
#![allow(clippy::unreadable_literal, clippy::pub_underscore_fields)]

'''
open(sys.argv[2], "w").write(header + text)
PY

if [ "$MODE" = "check" ]; then
    if ! diff -q "$OUT" "$REAL" >/dev/null 2>&1; then
        echo "warning: bindings_stub.rs is out of sync with the bindgen output." >&2
        echo "         Run \`scripts/regen-bindings-stub.sh\` and commit the diff." >&2
    else
        echo "bindings_stub.rs: up to date with bindgen output"
    fi
    exit 0
fi

echo "Regenerated $OUT ($(wc -l < "$OUT") lines)"
