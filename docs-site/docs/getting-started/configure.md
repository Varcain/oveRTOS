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
| `POSIX/SDL2 (native host)` | pthreads + SDL2, no cross-compilation |

### Enabling and Disabling Modules

Under `oveRTOS Modules`, each subsystem can be toggled independently. Two modules are always enabled (thread management and application lifecycle). The remaining modules are optional:

| Module | Config symbol | Default |
|---|---|---|
| Synchronization primitives | `OVE_SYNC` | enabled |
| Audio streaming | `OVE_AUDIO` | enabled |
| Filesystem | `OVE_FS` | enabled |
| Console I/O | `OVE_CONSOLE` | enabled |
| Logging | `OVE_LOG` | enabled |
| Time and delays | `OVE_TIME` | enabled |
| Board descriptor | `OVE_BOARD` | enabled |
| GPIO | `OVE_GPIO` | enabled |
| LED control | `OVE_LED` | enabled |
| BSP compatibility shim | `OVE_BSP` | enabled |
| LVGL UI framework | `OVE_LVGL` | enabled |
| Message queues | `OVE_QUEUE` | enabled |
| Software timers | `OVE_TIMER` | enabled |
| Event groups | `OVE_EVENTGROUP` | enabled |
| Interactive shell | `OVE_SHELL` | enabled |
| Non-volatile storage | `OVE_NVS` | enabled |
| Watchdog timer | `OVE_WATCHDOG` | enabled |
| Work queues | `OVE_WORKQUEUE` | disabled |
| Stream I/O | `OVE_STREAM` | disabled |

### Zero-Heap Mode

At the bottom of `oveRTOS Modules`, enable `Zero-heap build` (`OVE_ZERO_HEAP`) to strip all dynamic `_create()`/`_destroy()` APIs from every module. Use `_init()`/`_deinit()` with caller-supplied static buffers instead.

### Backend-Specific Submenus

After selecting a backend, a dedicated menu appears with source method and kernel configuration options. For advanced NuttX or Zephyr kernel tuning, use the dedicated targets:

```bash
make nuttx-menuconfig   # NuttX native kernel config TUI
make zephyr-menuconfig  # Zephyr native kernel config TUI
```

## Loading a Defconfig

Defconfigs are pre-made configurations for specific board and RTOS combinations. Load one to get a working starting point:

```bash
make <name>_defconfig
```

For example:

```bash
make qemu_freertos_example_c_defconfig
make stm32f746_zephyr_example_cpp_defconfig
make host_posix_example_rust_defconfig
```

### Available Defconfigs

**host (POSIX native)**

| Defconfig | Description |
|---|---|
| `host_posix_example_c_defconfig` | C example, POSIX backend |
| `host_posix_example_c_zeroheap_defconfig` | C example, POSIX, zero-heap |
| `host_posix_example_cpp_defconfig` | C++ example, POSIX backend |
| `host_posix_example_cpp_zeroheap_defconfig` | C++ example, POSIX, zero-heap |
| `host_posix_example_rust_defconfig` | Rust example, POSIX backend |
| `host_posix_example_rust_zeroheap_defconfig` | Rust example, POSIX, zero-heap |
| `host_posix_example_zig_defconfig` | Zig example, POSIX backend |
| `host_posix_example_zig_zeroheap_defconfig` | Zig example, POSIX, zero-heap |
| `host_posix_benchmark_defconfig` | Benchmark, POSIX backend |
| `host_posix_benchmark_zeroheap_defconfig` | Benchmark, POSIX, zero-heap |

**qemu (QEMU MPS2-AN500 Cortex-M7)**

| Defconfig | Description |
|---|---|
| `qemu_freertos_example_c_defconfig` | C example, FreeRTOS |
| `qemu_freertos_example_c_zeroheap_defconfig` | C example, FreeRTOS, zero-heap |
| `qemu_freertos_example_cpp_defconfig` | C++ example, FreeRTOS |
| `qemu_freertos_example_cpp_zeroheap_defconfig` | C++ example, FreeRTOS, zero-heap |
| `qemu_freertos_example_rust_defconfig` | Rust example, FreeRTOS |
| `qemu_freertos_example_rust_zeroheap_defconfig` | Rust example, FreeRTOS, zero-heap |
| `qemu_freertos_example_zig_defconfig` | Zig example, FreeRTOS |
| `qemu_freertos_example_zig_zeroheap_defconfig` | Zig example, FreeRTOS, zero-heap |
| `qemu_freertos_benchmark_defconfig` | Benchmark, FreeRTOS |
| `qemu_freertos_benchmark_zeroheap_defconfig` | Benchmark, FreeRTOS, zero-heap |
| `qemu_nuttx_example_c_defconfig` | C example, NuttX |
| `qemu_nuttx_example_c_zeroheap_defconfig` | C example, NuttX, zero-heap |
| `qemu_nuttx_example_cpp_defconfig` | C++ example, NuttX |
| `qemu_nuttx_example_cpp_zeroheap_defconfig` | C++ example, NuttX, zero-heap |
| `qemu_nuttx_example_rust_defconfig` | Rust example, NuttX |
| `qemu_nuttx_example_rust_zeroheap_defconfig` | Rust example, NuttX, zero-heap |
| `qemu_nuttx_example_zig_defconfig` | Zig example, NuttX |
| `qemu_nuttx_example_zig_zeroheap_defconfig` | Zig example, NuttX, zero-heap |
| `qemu_nuttx_benchmark_defconfig` | Benchmark, NuttX |
| `qemu_nuttx_benchmark_zeroheap_defconfig` | Benchmark, NuttX, zero-heap |
| `qemu_zephyr_example_c_defconfig` | C example, Zephyr |
| `qemu_zephyr_example_c_zeroheap_defconfig` | C example, Zephyr, zero-heap |
| `qemu_zephyr_example_cpp_defconfig` | C++ example, Zephyr |
| `qemu_zephyr_example_cpp_zeroheap_defconfig` | C++ example, Zephyr, zero-heap |
| `qemu_zephyr_example_rust_defconfig` | Rust example, Zephyr |
| `qemu_zephyr_example_rust_zeroheap_defconfig` | Rust example, Zephyr, zero-heap |
| `qemu_zephyr_example_zig_defconfig` | Zig example, Zephyr |
| `qemu_zephyr_example_zig_zeroheap_defconfig` | Zig example, Zephyr, zero-heap |
| `qemu_zephyr_benchmark_defconfig` | Benchmark, Zephyr |
| `qemu_zephyr_benchmark_zeroheap_defconfig` | Benchmark, Zephyr, zero-heap |

**stm32f746 (STM32F746G-Discovery)**

| Defconfig | Description |
|---|---|
| `stm32f746_freertos_example_c_defconfig` | C example, FreeRTOS |
| `stm32f746_freertos_example_c_zeroheap_defconfig` | C example, FreeRTOS, zero-heap |
| `stm32f746_freertos_example_cpp_defconfig` | C++ example, FreeRTOS |
| `stm32f746_freertos_example_cpp_zeroheap_defconfig` | C++ example, FreeRTOS, zero-heap |
| `stm32f746_freertos_example_rust_defconfig` | Rust example, FreeRTOS |
| `stm32f746_freertos_example_rust_zeroheap_defconfig` | Rust example, FreeRTOS, zero-heap |
| `stm32f746_freertos_example_zig_defconfig` | Zig example, FreeRTOS |
| `stm32f746_freertos_example_zig_zeroheap_defconfig` | Zig example, FreeRTOS, zero-heap |
| `stm32f746_freertos_benchmark_defconfig` | Benchmark, FreeRTOS |
| `stm32f746_freertos_benchmark_zeroheap_defconfig` | Benchmark, FreeRTOS, zero-heap |
| `stm32f746_nuttx_example_c_defconfig` | C example, NuttX |
| `stm32f746_nuttx_example_c_zeroheap_defconfig` | C example, NuttX, zero-heap |
| `stm32f746_nuttx_example_cpp_defconfig` | C++ example, NuttX |
| `stm32f746_nuttx_example_cpp_zeroheap_defconfig` | C++ example, NuttX, zero-heap |
| `stm32f746_nuttx_example_rust_defconfig` | Rust example, NuttX |
| `stm32f746_nuttx_example_rust_zeroheap_defconfig` | Rust example, NuttX, zero-heap |
| `stm32f746_nuttx_example_zig_defconfig` | Zig example, NuttX |
| `stm32f746_nuttx_example_zig_zeroheap_defconfig` | Zig example, NuttX, zero-heap |
| `stm32f746_nuttx_benchmark_defconfig` | Benchmark, NuttX |
| `stm32f746_zephyr_example_c_defconfig` | C example, Zephyr |
| `stm32f746_zephyr_example_c_zeroheap_defconfig` | C example, Zephyr, zero-heap |
| `stm32f746_zephyr_example_cpp_defconfig` | C++ example, Zephyr |
| `stm32f746_zephyr_example_cpp_zeroheap_defconfig` | C++ example, Zephyr, zero-heap |
| `stm32f746_zephyr_example_rust_defconfig` | Rust example, Zephyr |
| `stm32f746_zephyr_example_rust_zeroheap_defconfig` | Rust example, Zephyr, zero-heap |
| `stm32f746_zephyr_example_zig_defconfig` | Zig example, Zephyr |
| `stm32f746_zephyr_example_zig_zeroheap_defconfig` | Zig example, Zephyr, zero-heap |
| `stm32f746_zephyr_benchmark_defconfig` | Benchmark, Zephyr |
| `stm32f746_zephyr_benchmark_zeroheap_defconfig` | Benchmark, Zephyr, zero-heap |

Run `make help` to see the list at any time.

## Saving a Modified Configuration

After customising settings in `menuconfig`, save the minimal diff as a new defconfig:

```bash
make savedefconfig
```
