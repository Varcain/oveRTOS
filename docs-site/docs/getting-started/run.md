# Run

## Running the Firmware

After a successful build, launch the firmware with:

```bash
make run
```

The behaviour depends on the selected board:

| Board | Behaviour |
|---|---|
| `qemu` (`qemu-mps2-an500`) | Launches QEMU emulating an ARM MPS2-AN500 (Cortex-M7); semihosting provides console I/O |
| `host` | Executes the POSIX binary directly on the host; browser dashboard opens for display and audio |
| `stm32f746` | `make run` is not used; use `make flash` to program hardware over ST-LINK |
| `wasm` | Produces an HTML/WASM bundle; serve it over HTTP (COOP/COEP enabled) and open in a browser |

### QEMU Emulated Targets

When the board is `qemu` (`qemu-mps2-an500`), `make run` invokes QEMU with the generated `.elf` image. Console output is routed via ARM semihosting and appears in the terminal. No physical hardware is required.

To run without an interactive display (useful in CI):

```bash
make run HEADLESS=1
```

### POSIX Native Targets

When the board is `host`, the compiled binary runs directly as a Linux or macOS process. The sim framework launches a browser-based dashboard that visualises the board display, LEDs, GPIO, and audio. Audio output plays through the browser's Web Audio API.

```bash
make run   # starts the browser dashboard
```

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
