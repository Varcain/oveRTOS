# Board Configurations

oveRTOS ships with four reference board definitions. Each board lives under `boards/<name>/` and supplies:

- `board.yaml` — machine-readable hardware description (clocks, memory, peripherals)
- A CMakeLists.txt (or equivalent) that selects the correct backend and passes hardware parameters to the build
- Board-specific source files (clock init, peripheral configuration)

The active board is selected by adding its subdirectory in the parent project's CMakeLists.

---

## STM32F746G-DISCO (`stm32f746g-discovery`)

The ST STM32F746G-DISCO discovery kit is the primary hardware target and the reference board for FreeRTOS integration.

**Hardware summary**

| Parameter | Value |
|-----------|-------|
| MCU family | STM32F7 (Cortex-M7) |
| MCU | STM32F746xx |
| CPU frequency | 216 MHz |
| Flash | 1024 KB |
| Internal RAM | 320 KB + 64 KB DTCM |
| External SDRAM | 8192 KB @ 0xC0000000 |
| Display | 480 × 272 px, RGB565 |
| Audio codec | WM8994 via I2C @ 0x1A, 44100 Hz, 16-bit stereo |
| Console UART | 115200 baud, TX=PA9 / RX=PB7 |
| SD card | SDMMC1 |
| GPIO ports | 9 ports, 16 pins each |
| LEDs | 1 (port 8, pin 1, active-high) |
| Watchdog timeout | 5000 ms |

**Backends available**

The board directory contains subdirectories for `freertos`, `nuttx`, and `zephyr`. The FreeRTOS backend is the primary integration target and the one used in the reference examples.

**Clock tree**

HSE crystal at 25 MHz is used as the PLL source. The PLL output drives SYSCLK at 216 MHz, AHB at 216 MHz (divider 1), APB1 at 54 MHz (divider 4), and APB2 at 108 MHz (divider 2).

---

## QEMU MPS2 AN500 (`qemu-mps2-an500`)

The MPS2 AN500 model emulates a Cortex-M7 SoC on the QEMU machine. It is the primary emulation target for running oveRTOS without physical hardware.

**Hardware summary**

| Parameter | Value |
|-----------|-------|
| MCU family | ARM (emulated) |
| MCU model | CMSDK_CM7 |
| CPU frequency | 25 MHz |
| Flash | 4096 KB |
| RAM | 4096 KB |
| Display | 480 × 272 px, RGB565 |
| Console | Semihosting or UART (no physical TX/RX pins) |
| GPIO ports | 8 ports, 16 pins each |
| LEDs | None |
| Watchdog timeout | 5000 ms |

**Backends available**

The board directory contains subdirectories for `freertos`, `nuttx`, and `zephyr`.

**Running under QEMU**

The board directory includes a `qemu-run.sh` helper that launches the correct QEMU invocation with the MPS2 machine type, memory map, and semihosting for console output. A `qemu_board.c` file provides the board-specific QEMU peripheral shims.

```bash
# After building:
boards/qemu-mps2-an500/qemu-run.sh ./firmware.elf
```

The `cmake/` subdirectory contains toolchain files and CMake helpers for cross-compilation targeting this board.

---

## Host PC (`host-pc`)

The host-pc board uses the POSIX backend and compiles oveRTOS as a native Linux or macOS process. It is intended for rapid development iteration and unit testing on a desktop machine.

**Hardware summary**

| Parameter | Value |
|-----------|-------|
| MCU family | POSIX |
| Architecture | x86_64 |
| Flash / RAM | Host OS process memory |
| Display | 480 × 272 px, RGB565 (rendered via sim dashboard in browser) |
| Audio | 44100 Hz, 16-bit, 1 channel (512-sample buffers) |
| Console | Standard I/O |
| GPIO ports | 8 virtual ports, 16 pins each |
| LEDs | 8 virtual LEDs (port 0, pins 0–7, active-high) |

**Backend**

The board directory contains a single `posix` subdirectory. The POSIX backend maps oveRTOS threads to `pthread_t`, mutexes to `pthread_mutex_t`, semaphores to POSIX semaphores, and timers to `timer_create` or equivalent.

**Usage**

Building for the host-pc board produces a native executable that can be run directly:

```bash
cmake -B build -DOVE_BOARD=host-pc
cmake --build build
./build/firmware
```

This makes it straightforward to run oveRTOS applications in CI pipelines or debuggers without embedded hardware.

---

## WASM (`wasm`)

The WASM board compiles oveRTOS to WebAssembly via Emscripten and runs the firmware entirely in a web browser. LVGL renders into an HTML5 `<canvas>` element, and audio streams through the browser's Web Audio API.

**Hardware summary**

| Parameter | Value |
|-----------|-------|
| MCU family | POSIX (wasm32) |
| Architecture | WebAssembly |
| Flash / RAM | Browser process memory |
| Display | 480 × 272 px, XRGB8888 (rendered in HTML canvas via sim dashboard) |
| Audio | 44100 Hz, 16-bit, 1 channel (512-sample buffers) |
| Console | Browser console |
| GPIO ports | 8 virtual ports, 16 pins each |
| LEDs | 4 virtual LEDs (port 0, pins 0–3, active-high) |

**Backend**

The board uses the WASM backend (`backends/wasm/`), compiled with Emscripten (version 3.1.51). Threads use Emscripten's `SharedArrayBuffer`-based pthread emulation. Bus drivers (UART, SPI, I2C, I2S) are stubbed. HTTP and SNTP use Emscripten's native fetch support.

**Usage**

The WASM board produces an HTML page (`shell.html`) that loads the compiled WebAssembly module. The sim dashboard provides display, audio, LED, and GPIO visualisation in the browser.

**Requirements**

- Emscripten toolchain (3.1.51+)
- Browser with SharedArrayBuffer support (requires HTTPS or localhost with COOP/COEP headers)
