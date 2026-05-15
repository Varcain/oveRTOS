# Doctor — environment health check

`make doctor` (or `ove doctor` directly) probes the host environment and reports the status of every tool oveRTOS uses. Run it after a fresh checkout, when something breaks, or before opening a bug report — its output is the first thing anyone helping you will want to see.

## Running it

```bash
make doctor
```

For CI / scripting:

```bash
.venv/bin/ove doctor --json
```

Exit code is `0` on green/yellow (`OK` and `WARN`), `1` on red (`FAIL`).

## What it checks

The checks fall into five groups. A `FAIL` blocks building; a `WARN` is a tool you may need *if* you target the matching path (e.g., no ARM toolchain is fine for a POSIX-only workflow).

### Python

| Check | Required | What it verifies |
|---|---|---|
| `python` | Yes | Interpreter version ≥ 3.9 |
| `py:jinja2` | Yes | Template engine used to generate `FreeRTOSConfig.h`, `prj.conf`, etc. |
| `py:yaml` | Yes | `app.yaml` / `board.yaml` / `manifest.yaml` parsing |
| `py:jsonschema` | No | Validates `app.yaml` against the schema |
| `py:serial` | No | Used by hardware-in-the-loop tests only |

If any required Python package fails, recreate the venv:

```bash
rm -rf .venv && make setup
```

### Core build tools

| Check | Required | Notes |
|---|---|---|
| `cmake` | Yes | 3.20+ |
| `ninja` | No | CMake falls back to `make` if Ninja is missing |
| `make` | Yes | GNU Make |
| `git` | Yes | Source checkout + manifest-driven downloads |
| `ccache` | No | Optional compilation cache |

### Compilers

| Check | Required for | Notes |
|---|---|---|
| `gcc` | POSIX backend | Or `clang` |
| `clang` | POSIX backend | Or `gcc` |
| `arm-none-eabi-gcc` | Embedded targets | PATH binary *or* downloaded — see below |

`make doctor` also probes the **downloaded** ARM toolchain:

```
arm-gnu (downloaded)  ✓  output/toolchains/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-gcc
```

This is fetched lazily on the first embedded build (`make download` resolves it). If you see `WARN: not found`, run `make download` once.

### Language bindings

| Check | Required for | Minimum |
|---|---|---|
| `cargo` | Rust apps | 1.85 (recorded in `rust-toolchain.toml`) |
| `rustc` | Rust apps | 1.85 |
| `zig` | Zig apps | 0.15.2 |

These are `WARN` when missing because they are only needed for their respective language path. If `ove doctor` reports the wrong Zig version, run `make ensure-toolchain-zig` to download the supported one into `output/toolchains/`.

### RTOS / emulation tools

| Check | Required for | Notes |
|---|---|---|
| `qemu-system-arm` | QEMU runs | Install via your distro |
| `qemu-system-xtensa` | (future) | Reserved |
| `west` | Zephyr | Optional — Zephyr's own bootstrap covers it for most cases |
| `kconfig-mconf` | None | Optional system Kconfig binary; the venv installs a Python fallback |
| `openocd` | Hardware flashing | Required only for `make flash` to STM32 |

### Lint and format tools

| Check | Required for | Notes |
|---|---|---|
| `clang-format` | `make format` / `make lint` | Skipped silently if missing |
| `clang-tidy` | `make lint` (C/C++ semantic checks) | |
| `ruff` | `make lint` (Python) | |
| `lcov` | `make coverage` | |

All optional. `make lint` skips any tool it cannot find rather than failing.

### Downloaded tools (under `output/`)

| Check | Notes |
|---|---|
| `arm-gnu (downloaded)` | Fetched by `make download` for embedded builds |
| `zig (downloaded)` | Fetched by `make ensure-toolchain-zig` |
| `renode (downloaded)` | Fetched by `make ensure-toolchain-renode` for `make test-renode-*` |

### Manifest source trees (under `dl/`)

| Check | Required for | Notes |
|---|---|---|
| `dl:FreeRTOS-Kernel` | FreeRTOS backend | |
| `dl:STM32CubeF7` | FreeRTOS + STM32F7 | |
| `dl:lvgl` | `CONFIG_OVE_LVGL=y` apps | |
| `dl:CMSIS-DSP` | Audio + inference | |
| `dl:mbedtls` | `ove_net_tls` | |
| `dl:lwip` | FreeRTOS networking | |

All resolved by `make download`. Whether each is required depends on your selected config.

### Workspace state

```
workspace  ✓  ove_dir=/path/to/oveRTOS  venv=true  config=true
```

If `config=false`, no `.config` is loaded — pick one via `make <board>.<rtos>.<app>` first.

## Reading the output

```
oveRTOS environment check
========================================
  [✓] python              3.11.6
  [✓] py:jinja2
  [!] py:serial           missing — re-run 'make .venv'
  [✓] cmake               cmake version 3.28.1
  [!] arm-gnu (downloaded) run 'ove download' to fetch the pinned toolchain
  [✓] workspace           ove_dir=/home/me/oveRTOS  venv=True  config=True
========================================
OK with 2 optional tool(s) missing — some RTOS / language paths will be unavailable.
```

| Marker | Status | Meaning |
|---|---|---|
| `✓` | `OK` | Tool present and runnable |
| `!` | `WARN` | Optional tool missing — some path unavailable, build still works for other paths |
| `✗` | `FAIL` | Required tool missing — fix before building |

## JSON mode

For CI gates or editor integrations:

```bash
.venv/bin/ove doctor --json
```

Output shape:

```json
{
  "results": [
    {"name": "python", "status": "OK", "version": "3.11.6"},
    {"name": "arm-gnu (downloaded)", "status": "WARN", "detail": "not found at …"}
  ],
  "failed": [],
  "warned": ["arm-gnu (downloaded)"]
}
```

`failed` is the list to gate on. `warned` is informational.

## See also

- [Troubleshooting](troubleshooting.md) — concrete fixes per failure
- [Setup](setup.md) — what to install up front
