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

## Debugging

### QEMU (in-dashboard)

When running a `qemu` board under `make run`, the browser dashboard exposes a Debug window (Monaco source view, breakpoints, step controls, call stack, registers). It drives `arm-none-eabi-gdb` attached to QEMU's GDB stub via `config/scripts/ove-dashboard-bridge.py`. F5 / F6 / F10 map to continue / pause / step-over.

### WASM (Chrome DevTools)

WASM debugging uses the browser's own debugger, not the dashboard. One-time setup:

1. Build with debug symbols:

   ```bash
   emcmake cmake -DOVE_DEBUG=ON -B build boards/wasm/posix
   emmake make -C build
   ```

   This adds `-g -gsource-map` on the Emscripten link line, embedding DWARF in `ove_wasm.wasm` and emitting `ove_wasm.wasm.map`.

2. (Optional) Enable the runtime safety net — high-ROI for hunting memory bugs:

   ```bash
   emcmake cmake -DOVE_DEBUG=ON -DOVE_WASM_SAFE=ON -B build boards/wasm/posix
   ```

   `OVE_WASM_SAFE` turns on Emscripten's `SAFE_HEAP=1`, `ASSERTIONS=2`, and `STACK_OVERFLOW_CHECK=2`. Pays a significant runtime cost, so leave off for performance testing.

3. Install the **[C/C++ DevTools Support (DWARF)](https://chromewebstore.google.com/detail/cc-devtools-support-dwarf/pdcpmagijalfljmkmjngeonclgbbannb)** Chrome extension once. Without it, DevTools still steps through the `.wasm` byte-by-byte, but locals appear as raw wasm indices rather than named C variables.

4. Open the dashboard in Chrome/Chromium/Edge, press **F12**, switch to the **Sources** panel. Your project's `.c` files appear under the wasm module. Click a line to set a breakpoint, reload to hit it. Scope view shows named C locals with the extension installed.

Firefox has a built-in WASM debugger that picks up `-g` automatically; named-local support is more limited than Chrome's.

**In-dashboard controls on WASM**: the Debug window still shows the source browser and thread list, but the step/pause/breakpoint buttons are greyed out with a tooltip pointing to DevTools. The dashboard does not host its own debugger for WASM — the browser's is strictly more capable.

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
