# External Applications

oveRTOS applications can live outside the oveRTOS source tree. An **external app** is a standalone directory with its own `app.yaml`, `Makefile`, and optionally RTOS patches and config overlays. The build system discovers it via environment variable and builds it exactly like an in-tree app.

## Directory Layout

```
my_app/
├── Makefile                        # Build entry point (3 lines)
├── app.yaml                        # Application manifest
├── include/                        # Header files
│   └── app_conf.h
├── src/                            # Source files
│   ├── app.c
│   └── ...
├── nuttx/                          # NuttX RTOS overlay (optional)
│   └── <board-name>_defconfig
├── patches/                        # RTOS source patches (optional)
│   ├── freertos/
│   │   └── 0001-fix-something.patch
│   ├── nuttx/
│   │   └── 0001-fix-something.patch
│   └── zephyr/
│       └── 0001-fix-something.patch
└── output/                         # Build artifacts (generated)
```

## The App Makefile

The app Makefile is the build entry point. It is just three lines:

```makefile
APP_DIR := $(CURDIR)
OVE_DIR ?= /path/to/oveRTOS
include $(OVE_DIR)/config/make/ove_app.mk
```

- `APP_DIR` must point to the app directory (always `$(CURDIR)`).
- `OVE_DIR` must resolve to the oveRTOS source tree. Set it to an absolute path or a `$(realpath ...)` expression matching your directory layout. Use `?=` so it can be overridden from the environment (e.g. `make OVE_DIR=/opt/oveRTOS`).
- The `include` line pulls in `ove_app.mk`, which provides all build targets.

`ove_app.mk` does the following:

1. Sets `OVE_EXTERNAL_APPS=$(APP_DIR)` so the build system discovers the app.
2. Bootstraps the Python venv and `ove` CLI if not already set up.
3. Exposes these Make targets:

| Target | Description |
|--------|-------------|
| `make` (default) | Full pipeline: download + configure + build |
| `make <board>.<rtos>.<app>` | Load a configuration via fragment composition |
| `make menuconfig` | Interactive Kconfig TUI |
| `make savedefconfig` | Save current config as minimal defconfig |
| `make nuttx-menuconfig` | NuttX native menuconfig (layered) |
| `make zephyr-menuconfig` | Zephyr native menuconfig (layered) |
| `make download` | Fetch RTOS sources |
| `make configure` | Generate build files from `.config` |
| `make all` | Download + configure + build |
| `make run` | Run firmware (QEMU or host) |
| `make flash` | Flash firmware to hardware |
| `make alldefconfigs` | Build every configuration (all board/RTOS/app combinations) |
| `make clean` | Remove `output/` |
| `make help` | List targets and available configurations |

All output goes into the app's own `output/` directory, not into the oveRTOS tree.

## app.yaml

The `app.yaml` manifest declares the application language, sources, and optional Kconfig parameters.

### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `lang` | string | `c`, `cpp`, `rust`, or `zig` |
| `description` | string | Shown in the Kconfig application menu |

### Language-Specific Fields

**C / C++** (require `sources`):

```yaml
lang: c
description: "My App"
sources:
  - src/app.c
  - src/util.c
includes:
  - include
```

**Rust** (requires `rust.lib_name`):

```yaml
lang: rust
description: "Rust App"
rust:
  lib_name: my_app
  crate_dir: "."        # optional, default: "."
```

**Zig** (requires `zig.lib_name`):

```yaml
lang: zig
description: "Zig App"
zig:
  lib_name: my_app
  src_dir: src           # optional, default: "src"
```

### Platform and Board Source Overrides

Use `platform_sources` to compile different files depending on the RTOS, and `board_sources` to override per board. Board-specific sources take priority over platform-specific.

```yaml
lang: c
description: "My DSP App"
sources:
  - src/app.c
  - src/ui.c
platform_sources:
  default:
    - src/dsp_stub.c       # fallback for all RTOSes
  freertos:
    - src/dsp_freertos.c   # FreeRTOS-specific DSP
board_sources:
  stm32f746g-discovery:
    - src/dsp_cmsis.c      # hardware-accelerated DSP for this board
includes:
  - include
```

Resolution order:

1. If the active board is listed in `board_sources`, use those files.
2. Otherwise, if the active RTOS is listed in `platform_sources`, use those files.
3. Otherwise, if `platform_sources.default` exists, use that.
4. Otherwise, no extra sources are added.

The resolved extra sources are appended to the base `sources` list.

### App-Specific Kconfig

The `kconfig` field injects raw Kconfig content into the app's configuration menu. These options appear under the Application menu in `make menuconfig` and are available as `CONFIG_*` macros in generated headers.

```yaml
kconfig: |
  config DSP_BUFFER_SIZE
  	int "DSP buffer size (samples)"
  	default 512

  config DSP_RATE
  	int "Sample rate (Hz)"
  	default 44100
```

Access these in C via the generated `ove_config.h`:

```c
#include "generated/ove_config.h"

static int16_t buf[CONFIG_DSP_BUFFER_SIZE];
```

## Configuration

External apps use the same fragment-based configuration system as in-tree apps. The `app.yaml` `config_name` and `defconfig` fields declare the app's config identity and required modules. The build system composes the final `.config` from global, board, RTOS, and app fragments automatically.

Load a configuration using dot-syntax:

```bash
make stm32f746.freertos.myapp
```

The `defconfig` field in `app.yaml` lists the Kconfig symbols the app requires:

```yaml
defconfig:
  - CONFIG_OVE_AUDIO=y
  - CONFIG_OVE_FS=y
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_LOG_LEVEL_INF=y
```

Save the current config as a minimal defconfig:

```bash
make savedefconfig
```

## Configuration Layering

For NuttX and Zephyr, the RTOS has its own Kconfig system separate from oveRTOS. The build system merges multiple configuration layers at build time, with higher layers overriding lower ones:

### NuttX Config Layers

```
Layer 1: oveRTOS template base        generated/nuttx_defconfig
                                       (auto-generated from oveRTOS .config)
         ↓ overridden by
Layer 2: Board overlay                 boards/<board>/nuttx/ove_board_defconfig
                                       (board-specific NuttX settings)
         ↓ overridden by
Layer 3: App overlay                   <app>/nuttx/<board-name>_defconfig
                                       (app-specific NuttX settings)
         ↓ overridden by
Layer 4: User customizations           rtos.config
                                       (saved by `make nuttx-menuconfig`)
```

**Layer 1** is generated from oveRTOS Kconfig options using a Jinja2 template. It sets base NuttX requirements like the entry point, watchdog, stack coloration, TLS, and LVGL support.

**Layer 2** is a file in the oveRTOS board directory (`boards/<board>/nuttx/ove_board_defconfig`). It configures board-specific NuttX peripherals like SDMMC, LTDC, DTCM, and UART baud rate. This file is maintained in the oveRTOS tree and shared by all apps targeting that board.

**Layer 3** is provided by the app. Place a file named `<board-name>_defconfig` (e.g. `stm32f746g-discovery_defconfig`) in a `nuttx/` directory at the app root. This is where the app enables NuttX drivers and subsystems it needs. For example, an audio app would enable SAI, I2S, codec drivers, and FAT filesystem here:

```
# nuttx/stm32f746g-discovery_defconfig
CONFIG_INIT_STACKSIZE=8192
CONFIG_STM32F7_SDMMC1=y
CONFIG_FS_FAT=y
CONFIG_DRIVERS_AUDIO=y
CONFIG_AUDIO_WM8994=y
CONFIG_STM32F7_SAI2=y
```

If no board-specific overlay exists, the build system also checks for a generic `<app>/nuttx_defconfig`.

**Layer 4** is created automatically when you run `make nuttx-menuconfig`. The NuttX native menuconfig presents the merged result of layers 1-3 as a baseline, then saves only the delta (your changes) to `rtos.config` in the workspace. This file should not be edited by hand.

### Zephyr Config Layers

```
Layer 1: oveRTOS template base        generated/prj.conf
         ↓ appended by
Layer 3: App overlay                   <app>/prj.conf
         ↓ appended by
Layer 4: User customizations           rtos.config
                                       (saved by `make zephyr-menuconfig`)
```

Layer 2 (board overlay) is skipped for Zephyr because Zephyr's CMake system auto-detects board-specific `boards/*.conf` files.

### FreeRTOS

FreeRTOS configuration is handled entirely through the oveRTOS Kconfig system. The generated `FreeRTOSConfig.h` is derived from oveRTOS `.config` options. There are no additional layers.

## Patches

External apps can carry patches for RTOS source trees. Place `.patch` files in:

```
patches/
├── freertos/
│   └── 0001-fix-something.patch
├── nuttx/
│   └── 0001-add-codec-driver.patch
└── zephyr/
    └── 0001-fix-board-dts.patch
```

Patches are applied during the build, after board patches and before compilation. The application order is:

1. **Board patches** from `boards/<board>/<rtos>/patches/` (maintained in oveRTOS)
2. **App patches** from `<app>/patches/<rtos>/` (maintained in the external app)

App patches are applied on top of board patches so they can depend on board-level fixes. Patches use `git apply` format and are applied idempotently (a stamp file tracks which patches have already been applied). The downloaded RTOS sources in `dl/` are never modified; patches are applied to build-tree copies.

To create a patch:

```bash
cd output/<board>/<rtos>/<app>/build/nuttx   # or the relevant RTOS build dir
# Make your changes
git diff > /path/to/my_app/patches/nuttx/0001-description.patch
```

## Build Workflow

### Quick Start

```bash
cd my_app/

# 1. Load a configuration
make stm32f746.freertos.myapp

# 2. Build (downloads RTOS sources, generates configs, compiles)
make

# 3. Run on QEMU or flash to hardware
make run
# or
make flash
```

### Step by Step

```bash
# Load config
make stm32f746.nuttx.myapp

# Download RTOS sources
make download

# Generate build files
make configure

# Build
make all

# (Optional) Tune NuttX options
make nuttx-menuconfig

# Rebuild
make
```

### Interactive Configuration

```bash
# oveRTOS-level config (app, board, RTOS, modules)
make menuconfig

# NuttX-native config (drivers, stack sizes, peripherals)
make nuttx-menuconfig

# Zephyr-native config
make zephyr-menuconfig
```

### Building All Configurations

```bash
make alldefconfigs
```

This iterates over all board/RTOS/app combinations, composes each configuration from fragments, and runs the full build pipeline. A summary is printed at the end. This is useful for CI.

## Workspace Layout

Each configuration creates an isolated workspace:

```
output/
└── <board>/
    └── <rtos>/
        └── <app>/
            ├── .config           # Expanded Kconfig
            ├── build/            # RTOS build tree
            ├── generated/        # Generated headers and build fragments
            │   ├── ove_config.h
            │   ├── ove_config.cmake
            │   ├── app_sources.cmake
            │   ├── board_desc.h
            │   └── nuttx_defconfig (or prj.conf, FreeRTOSConfig.h)
            ├── images/           # Firmware binaries
            │   ├── firmware.elf
            │   └── firmware.bin
            └── dl/               # Downloaded RTOS sources
```

Multiple workspaces can coexist. Switching between them is done by loading a different configuration.

## Quick start: a new external app

The fastest path is the CLI generator (which stamps the same layout the reference content above describes):

```bash
ove app new --lang cpp --name my_app
cd my_app
make qemu.freertos.my_app && make && make run
```

If you want to lay it out by hand, the minimum is **three** files in your app directory:

- `Makefile` — three lines:
    ```makefile
    APP_DIR := $(CURDIR)
    OVE_DIR ?= /path/to/oveRTOS
    include $(OVE_DIR)/config/make/ove_app.mk
    ```
- `app.yaml` — declare language, sources, required Kconfig, and the dot-syntax target name. See [The App Makefile](#the-app-makefile) and [app.yaml](#appyaml) above.
- `src/app.c` (or `app.cpp` / `lib.rs` / `main.zig`) — define `ove_main()`.

Optional add-ons (each documented in the reference sections above): NuttX `defconfig` overlays under `nuttx/`, app-level patches under `patches/<rtos>/`, multi-app workspaces.
