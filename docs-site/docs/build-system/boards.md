# Board Configurations

oveRTOS ships with two reference board definitions, `stm32f746g-discovery` and `qemu-mps2-an500`, plus `qemu-mps2-an521`, the Cortex-M33 board that Zephyr's Linux personality runs on. Each board lives under `boards/<name>/` and supplies:

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
| Console | CMSDK UART0 on QEMU's stdio (the Linux personality guest uses UART1) |
| GPIO ports | 8 ports, 16 pins each |
| LEDs | None |
| Watchdog timeout | 5000 ms |

**Backends available**

The board directory contains subdirectories for `freertos`, `nuttx`, and `zephyr`.

**Running under QEMU**

The board directory includes a `qemu-run.sh` helper that launches the correct QEMU invocation with the MPS2 machine type and memory map; the console UART is on its stdio, and the sim display, audio and input files go over semihosting.

**What the emulated machine provides**

QEMU emulates no AN500 GPIO, storage or STM32 peripherals, so the board supplies its own drivers where the RTOS backend expects that hardware:

| Module | FreeRTOS | NuttX |
|--------|----------|-------|
| Console | `freertos_console.c` over the board's UART0 driver (`qemu_serial.c`) | NuttX `/dev/console` (UART0) |
| Time | `freertos_time_systick.c` (QEMU has no DWT cycle counter) | NuttX clock |
| GPIO | `qemu_gpio.c`: a RAM bank of the 8 × 16 pins in `board.yaml`; pins read back what was written, and no pin interrupt ever fires | same |
| NVS | `qemu_nvs.c`: kept in RAM until QEMU exits | NuttX backend (needs a mounted filesystem) |
| Filesystem | not available: `OVE_FS` cannot be enabled, and `ove_fs_*` return `OVE_ERR_NOT_SUPPORTED` | NuttX backend |
| Watchdog | `qemu_watchdog.c`: every call returns `OVE_ERR_NOT_SUPPORTED` | NuttX backend (`OVE_ERR_NOT_SUPPORTED` without `/dev/watchdog0`) |

```bash
# After building:
boards/qemu-mps2-an500/qemu-run.sh ./firmware.elf
```

The `cmake/` subdirectory contains toolchain files and CMake helpers for cross-compilation targeting this board.

**In a web browser (`web` variant)**

The same firmware also runs in a browser: `web` is a variant of this board that packages the image together with QEMU compiled to WebAssembly.

```bash
make web.freertos.example_c
make
make run        # serves the bundle and opens it
```

The build writes `output/web/<rtos>/<app>/serve/`: the sim dashboard, the firmware, and a link to the emulator. The directory is self-contained — copy it with the link dereferenced to put it on any static host. The public demos on the docs site are built this way: `make docs-demos` builds every app's bundle and adds those that start to the built site.

- The emulator is built once, on first use, by `ove ensure-toolchain qemu-wasm` into `output/tools/qemu-wasm/`. It is the upstream QEMU release pinned in `manifest.yaml`, built with upstream's own Emscripten container recipe (so it needs Docker or Podman), plus the small patches in `config/qemu-wasm/patches/`. Each patch says why it is there; so far they keep guests that reprogram the MPU often, such as the Linux personality, from running several times slower in the browser. Changing a patch builds a new emulator on the next build, and the bundle's `qemu/SOURCE.txt` carries the exact source, recipe and patches.
- QEMU has no JIT for a WebAssembly host and runs its interpreter instead, several times slower than the native `qemu-system-arm`. The variant therefore builds the optimised firmware rather than the debug build plain `qemu.<rtos>.<app>` uses. Simple UIs run at interactive rates; full-screen animation does not.
- The page needs `SharedArrayBuffer`, so it must be cross-origin isolated. `serve/run.sh` sends the COOP/COEP headers; on a static host such as GitHub Pages the bundled `coi-serviceworker.js` supplies them.
- The dashboard shows the display, console, audio, trace and thread panels and takes pointer, console and audio input. The profiler and the debugger are not available in the browser, and there is no network peer; use the native QEMU run for those.

A variant is declared under `variants:` in `board.yaml`: a name that stands in for the board in `<board>.<rtos>.<app>` and adds Kconfig lines (`CONFIG_OVE_SIM_WEB=y` here).

