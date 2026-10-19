# Run

## Running the Firmware

After a successful build, launch the firmware with:

```bash
make run
```

The behaviour depends on the selected board:

| Board | Behaviour |
|---|---|
| `qemu` (`qemu-mps2-an500`) | Launches QEMU emulating an ARM MPS2-AN500 (Cortex-M7); the console is the board's UART0 on the terminal |
| `stm32f746` | `make run` is not used; use `make flash` to program hardware over ST-LINK |
| `web` / `web-an521` (browser variants of `qemu-mps2-an500` / `qemu-mps2-an521`) | Serves the browser bundle over HTTP (COOP/COEP enabled) and opens it; the firmware runs in QEMU compiled to WebAssembly |

### QEMU Emulated Targets

When the board is `qemu` (`qemu-mps2-an500`), `make run` invokes QEMU with the generated `.elf` image. The console is the emulated CMSDK UART0, connected to the terminal: output appears there and typed input reaches `ove_console`. With the dashboard (the default, not `--headless`), its console box feeds the same UART, as it does in the browser bundle. No physical hardware is required.

To run without an interactive display (useful in CI):

```bash
make run HEADLESS=1
```

### In a Web Browser

`make web.<rtos>.<app>` configures the QEMU board's `web` variant. The build then also writes `serve/` — the dashboard, the firmware and QEMU compiled to WebAssembly — and `make run` serves it locally:

```bash
make web.freertos.example_c
make
make run   # http://localhost:8080
```

A Linux personality app gets a terminal on its console instead of the dashboard; Zephyr's runs on the an521, so it is `make web-an521.zephyr.<app>`.

The emulator is built on first use (`ove ensure-toolchain qemu-wasm`, needs Docker or Podman). It runs QEMU's interpreter, several times slower than the native run above, so this variant builds the optimised firmware. See [Board Configurations](../build-system/boards.md) for what the bundle contains and how to host it.

## Flashing to Hardware

To flash firmware onto a physical board (e.g., STM32F746G-Discovery):

```bash
make flash
```

This invokes the `ove` CLI, which calls OpenOCD with the configured board file (`OVE_OPENOCD_CFG`, default: `board/stm32f7discovery.cfg`). Connect the board via ST-LINK USB before running.

Ensure OpenOCD is installed:

```bash
# Debian/Ubuntu
sudo apt install openocd

# Fedora
sudo dnf install openocd
```

## Debugging

### QEMU (in-dashboard)

When running a `qemu` board under `make run`, the browser dashboard has a debugger: run control in its status bar (pause, continue, step over / into / out, reset) and a Debug panel (Monaco source view, breakpoints, call stack, registers). Halting the target switches the dashboard to its Debug workspace. It drives `arm-none-eabi-gdb` attached to QEMU's GDB stub via `config/scripts/ove-dashboard-bridge.py`. F6 / F10 / F11 / Shift+F11 map to pause / step over / step into / step out; F5 continues while the Debug panel is shown.

### Browser bundle

The dashboard in a `web` bundle has no debugger: the firmware is the optimised build and the emulator runs inside the page, where there is no GDB to attach. Debug the same app on the native `qemu` board instead.

## Testing

Run the full test suite:

```bash
make test
```

Individual test targets:

```bash
make test-stub             # Stub backend unit tests
make test-cpp              # C++ binding tests
make test-rust             # Rust binding tests
make test-zig              # Zig binding tests

make test-qemu             # All QEMU ARM tests
make test-qemu-freertos    # FreeRTOS on QEMU
make test-qemu-nuttx       # NuttX on QEMU
make test-qemu-zephyr      # Zephyr on QEMU

make test-all              # All tests (simulation + QEMU)
```

Zero-heap variants are also available for QEMU tests:

```bash
make test-qemu-freertos-zeroheap
make test-qemu-nuttx-zeroheap
make test-qemu-zephyr-zeroheap
```
