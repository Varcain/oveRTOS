# Configure

oveRTOS uses [Kconfig](https://www.kernel.org/doc/html/latest/kbuild/kconfig-language.html) for build configuration, processed by the `ove` CLI. All configuration is stored in `.config` in the project root.

## Interactive Configuration

```bash
make menuconfig
```

This opens the Kconfig TUI. Use arrow keys to navigate, space to toggle options, and `?` to read help text. Save with `S` and exit with `Q`.

The top-level menu is organised into:

- **RTOS Selection** — choose the backend
- **Hardware Target** — choose the board
- **Toolchain** — ARM cross-compiler source and settings
- **oveRTOS Modules** — enable or disable individual API modules
- **Debug Options** — log level, debug builds, stack canaries

### Selecting the RTOS Backend

Under `RTOS Selection > RTOS backend`, choose one of:

| Option | Backend |
|---|---|
| `FreeRTOS` | FreeRTOS via STM32CubeF7 SDK |
| `Zephyr RTOS` | Zephyr Project via West |
| `Apache NuttX` | Apache NuttX RTOS |
| `POSIX (native host)` | pthreads + sim dashboard, no cross-compilation |

### Enabling and Disabling Modules

Under `oveRTOS Modules`, each subsystem can be toggled independently. Two modules are always enabled (thread management and application lifecycle). The remaining modules are optional:

| Module | Config symbol | Kconfig default |
|---|---|---|
| Synchronization primitives | `OVE_SYNC` | disabled |
| Audio engine | `OVE_AUDIO` | disabled |
| Filesystem | `OVE_FS` | disabled |
| Console I/O | `OVE_CONSOLE` | disabled |
| Logging | `OVE_LOG` | disabled |
| Time and delays | `OVE_TIME` | disabled |
| Board descriptor | `OVE_BOARD` | disabled |
| GPIO | `OVE_GPIO` | disabled |
| LED control | `OVE_LED` | disabled |
| BSP compatibility shim | `OVE_BSP` | disabled |
| LVGL UI framework | `OVE_LVGL` | disabled |
| Message queues | `OVE_QUEUE` | disabled |
| Software timers | `OVE_TIMER` | disabled |
| Event groups | `OVE_EVENTGROUP` | disabled |
| Interactive shell | `OVE_SHELL` | disabled |
| Non-volatile storage | `OVE_NVS` | disabled |
| Watchdog timer | `OVE_WATCHDOG` | disabled |
| Work queues | `OVE_WORKQUEUE` | disabled |
| Stream I/O | `OVE_STREAM` | disabled |
| Power management | `OVE_PM` | disabled |

Defconfig files enable commonly used modules for each board/RTOS combination. When starting from scratch with `make menuconfig`, all optional modules default to disabled.

### Zero-Heap Mode

At the bottom of `oveRTOS Modules`, enable `Zero-heap build` (`OVE_ZERO_HEAP`) to switch `_create()`/`_destroy()` from heap-backed functions to GCC statement-expression macros that auto-generate per-call-site static storage. Application code using `_create()`/`_destroy()` continues to work unchanged. Use `_init()`/`_deinit()` when you need explicit storage control (arrays, loops, structs). In zero-heap mode, size parameters must be compile-time constants and each `_create()` call site produces one static object.

### Backend-Specific Submenus

After selecting a backend, a dedicated menu appears with source method and kernel configuration options. For advanced NuttX or Zephyr kernel tuning, use the dedicated targets:

```bash
make nuttx-menuconfig   # NuttX native kernel config TUI
make zephyr-menuconfig  # Zephyr native kernel config TUI
```

## Loading a Configuration

Configurations use dot-separated `<board>.<rtos>.<app>` syntax. The build system composes the final `.config` from config fragments (global, board, RTOS, app) defined in `config/fragments/`, `board.yaml`, and `app.yaml`:

```bash
make <board>.<rtos>.<app>
```

For example:

```bash
make qemu.freertos.example_c
make stm32f746.zephyr.example_cpp
make host.posix.example_rust
```

For zero-heap variants, append `ZEROHEAP=1`:

```bash
make host.posix.example_c ZEROHEAP=1
```

### Available Boards, RTOSes, and Apps

**Boards:**

| Short name | Board |
|---|---|
| `host` | Host PC (POSIX native) |
| `qemu` | QEMU MPS2-AN500 (Cortex-M7) |
| `stm32f746` | STM32F746G-Discovery |
| `wasm` | WebAssembly (Emscripten) |

**RTOSes:**

| Name | Boards |
|---|---|
| `posix` | host, wasm |
| `freertos` | qemu, stm32f746 |
| `nuttx` | qemu, stm32f746 |
| `zephyr` | qemu, stm32f746 |

**Apps** (from `app.yaml` `config_name` fields):

| App | Languages |
|---|---|
| `example_c`, `example_cpp`, `example_rust`, `example_zig` | Basic example |
| `benchmark`, `benchmark_cpp`, `benchmark_rust`, `benchmark_zig` | Latency/throughput benchmark |
| `example_net`, `example_net_cpp`, `example_net_rust`, `example_net_zig` | Networking |
| `example_pm_c`, `example_pm_cpp`, `example_pm_rust`, `example_pm_zig` | Power management |
| `example_keyword_live`, `example_keyword_live_cpp`, `example_keyword_live_rust`, `example_keyword_live_zig` | Keyword detection |

### Building All Configurations

Build all apps for a specific board/RTOS pair:

```bash
make allconfigs-host.posix
make allconfigs-qemu.freertos
make allconfigs-stm32f746.zephyr
```

Build every configuration across all boards and RTOSes:

```bash
make alldefconfigs
```

Run `make help` to see available targets at any time.

### How Fragment Composition Works

The `ove` CLI composes the final `.config` by layering these fragments in order:

1. **Global fragment** (`config/fragments/global.defconfig`) — common modules
2. **Board selection** + `board.yaml` defconfig entries — board-specific symbols
3. **RTOS fragment** (`config/fragments/rtos/<rtos>.defconfig`) — RTOS selection and settings
4. **Board+RTOS overrides** from `board.yaml` `rtos_defconfig.<rtos>` — per-RTOS board tuning
5. **App selection** + `app.yaml` defconfig entries — app language and required modules
6. **Variant fragment** (`config/fragments/variant/zeroheap.defconfig`) — applied when `ZEROHEAP=1`

Each layer supplements the previous without overwriting, so earlier settings are preserved unless explicitly overridden.

## Saving a Modified Configuration

After customising settings in `menuconfig`, save the minimal diff as a defconfig:

```bash
make savedefconfig
```
