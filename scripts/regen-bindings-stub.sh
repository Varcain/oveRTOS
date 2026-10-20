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
#   --check     Compare with fresh bindgen output, ignoring doc attributes.
#               Fail on drift or generation failure. Skip if LVGL or the
#               rust_stub storage probe is missing.
set -euo pipefail

MODE="regen"
if [ "${1:-}" = "--check" ]; then
    MODE="check"
    shift
fi

OVE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LVGL_DIR="$OVE_DIR/dl/lvgl"
if [ ! -f "$LVGL_DIR/lvgl.h" ]; then
    if [ "$MODE" = "check" ]; then
        echo "warning: dl/lvgl not present — skipping bindings_stub.rs drift check" >&2
        exit 0
    fi
    echo "Fetch the manifest-pinned LVGL into dl/lvgl before regenerating bindings_stub.rs" >&2
    exit 1
fi

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
#define CONFIG_OVE_ASYNC 1
#define CONFIG_OVE_TRACE_STREAM 1
#define CONFIG_OVE_TRACE_MARKERS 1
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
LV_CONF_PATH="$OVE_DIR/tests/host" \
LVGL_INCLUDE_PATH="$LVGL_DIR" \
LVGL_PARENT_PATH="$OVE_DIR/dl" \
CARGO_TARGET_DIR="$TMP/target" \
cargo build --manifest-path "$OVE_DIR/bindings/rust/ove/Cargo.toml" \
    --features std --locked >"$TMP/cargo.log" 2>&1 || true

BINDINGS=""
if [ -d "$TMP/target" ]; then
    BINDINGS=$(find "$TMP/target" -name ove_bindings.rs -print -quit)
fi
if [ -z "$BINDINGS" ]; then
    if [ "$MODE" = "check" ]; then
        echo "error: bindgen never ran" >&2
        tail -n 30 "$TMP/cargo.log" >&2
        exit 1
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
# Remove legacy layout-assertion forms; structured assertions remain below.
text = re.sub(
    r"#\[allow\(clippy::unnecessary_operation, clippy::identity_op\)\]\s*\n"
    r"const _: \(\) = \{[^;]*?\};",
    "", text, flags=re.DOTALL)
text = re.sub(r"const _: \(\) = \{[^;]*?\};", "", text, flags=re.DOTALL)
# core::ffi for no_std stub
text = re.sub(r"::std::os::raw::", "core::ffi::", text)
text = re.sub(r"::(?:std|core)::option::Option", "Option", text)
text = re.sub(r"::(?:std|core)::mem::", "core::mem::", text)

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
//! `cargo doc` can build without a real C toolchain or LVGL headers.
//!
//! Regenerate after changing the C FFI surface, LVGL pin or host LVGL config:
//!     scripts/regen-bindings-stub.sh
//! `make lint` compares this stub with fresh bindgen output.

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

# Format the output so `cargo fmt --check` and the on-disk file stay in
# sync. Without this, `--check` here diffs against the unformatted bindgen
# output and reports false drift after any `cargo fmt` pass.
if command -v rustfmt >/dev/null 2>&1; then
    rustfmt --edition 2024 "$OUT" 2>/dev/null || true
fi

if [ "$MODE" = "check" ]; then
    sed '/^[[:space:]]*#\[doc = /d' "$REAL" > "$TMP/stub.nodoc"
    sed '/^[[:space:]]*#\[doc = /d' "$OUT" > "$TMP/generated.nodoc"
    if ! diff -U2 --label 'bindings_stub.rs (in tree)' --label 'bindings_stub.rs (generated)' \
        "$TMP/stub.nodoc" "$TMP/generated.nodoc" > "$TMP/stub.diff"; then
        echo "error: bindings_stub.rs is out of sync with the bindgen output." >&2
        echo "         Run \`scripts/regen-bindings-stub.sh\` and commit the diff." >&2
        head -n 40 "$TMP/stub.diff" >&2
        exit 1
    fi
    echo "bindings_stub.rs: up to date with bindgen output"
    exit 0
fi

echo "Regenerated $OUT ($(wc -l < "$OUT") lines)"
