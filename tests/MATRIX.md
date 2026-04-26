# oveRTOS Test Matrix

Which suite runs on which backend. Sources of truth:

- C suite list: `tests/cmake/OveTest.cmake` and `tests/framework/suites.inc`
- Per-variant runner: `tests/sim/*/sim_main.c` (or `src/main.c`)
- Per-variant build inputs: `tests/sim/*/CMakeLists.txt` (or `nuttx_app/Makefile`)

## Suite × backend

Legend: `✅` runs, `❌` skipped (intentionally), `—` not yet enabled.

| Suite              | stub | nuttx (native) | nuttx-qemu | freertos-qemu | zephyr | zephyr-qemu | Notes |
|--------------------|:----:|:--------------:|:----------:|:-------------:|:------:|:-----------:|-------|
| storage_bounds     | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | Canary red-zones; active only in `CONFIG_OVE_ZERO_HEAP` builds |
| thread             | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| sync_mutex         | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| sync_sem           | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| sync_event         | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| sync_condvar       | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| sync_recursive     | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| queue              | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| timer              | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| time               | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| eventgroup         | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| workqueue          | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| stream             | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| console            | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| watchdog           | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| nvs                | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| shell              | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| audio              | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| bsp                | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| board              | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| gpio               | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| led                | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| lvgl               | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| app                | ✅   | ✅             | ✅         | ✅            | ✅     | ✅          | |
| fs                 | ✅   | ✅             | ❌         | ❌            | ❌     | ❌          | No FS on bare-metal QEMU |
| static_define      | ✅   | —              | —          | —             | —      | —           | Stub-only today |
| infer              | ✅   | —              | —          | —             | —      | —           | Needs TFLite; stub only |
| net_mqtt           | ✅   | —              | —          | —             | —      | —           | Unit test only (no socket I/O) |
| net_httpd          | ✅   | —              | —          | —             | —      | —           | SHA-1 + Base64 only |
| net_sntp           | ✅   | —              | —          | —             | —      | —           | |
| net_loopback       | ✅   | —              | —          | —             | —      | —           | TCP echo round-trip on 127.0.0.1 |
| i2c                | ✅   | —              | —          | —             | —      | —           | Skips if bus not configured |
| spi                | ✅   | —              | —          | —             | —      | —           | Skips if bus not configured |
| uart               | ✅   | —              | —          | —             | —      | —           | Skips if port not configured |
| pm                 | ✅   | —              | —          | —             | —      | —           | |

`nuttx-qemu`, `freertos-qemu`, `zephyr-qemu` each have a matching
`-zeroheap` variant that runs the same suite set with
`CONFIG_OVE_ZERO_HEAP=1`. They are not listed as separate columns to keep
the matrix readable.

## Renode STM32F746 target (board-level fidelity)

A pair of full-system targets run the CMocka suites on Renode's
`stm32f7_discovery-bb` emulation — one heap variant, one zero-heap
variant.  Unlike the QEMU targets which use the generic `mps2-an500`
board (CPU + NVIC + SysTick only), these link the real STM32F7 HAL,
FreeRTOS ARM_CM7 port, and STM32 startup + linker script — the same
firmware that would flash on a real Discovery board.

| Target | Suite count | CI trigger |
|---|---|---|
| `renode-stm32f746-freertos` | 211 | push to main + PRs + `workflow_dispatch` |
| `renode-stm32f746-freertos-zeroheap` | 184 | push to main + PRs + `workflow_dispatch` |

Caveats:
- Renode 1.16.1's ARM Cortex-M semihosting handler implements
  `SYS_WRITEC` (0x03) but not `SYS_WRITE` (0x05) or `SYS_HEAPINFO`
  (0x20).  `tests/sim/renode-stm32f746-freertos*/semihosting_io.c`
  overrides `_write` and `_sbrk` to work around this so printf and
  `malloc` function correctly.
- The Renode platform doesn't model IWDG / SAI / FMC.  Firmware code
  that touches those still builds and the canary-level storage tests
  pass, but any test that *observes* IWDG behaviour (watchdog actually
  resetting, SAI DMA callbacks) is not covered here.  Real hardware is
  still the ground truth for those paths.

## Hardware-specific storage layouts (STM32 IWDG etc.)

Hardware-specific backend structs (e.g. the STM32 `struct ove_watchdog`
that embeds `IWDG_HandleTypeDef`) are compiled only by the STM32 target
builds, not by any of the non-Renode test backends above.  Drift between
what the backend writes and what consumers see in
`ove_storage_<rtos>.h` is caught by three gates:

1. **STM32 build-only CI jobs** in `.github/workflows/alldefconfigs.yml`
   (`stm32f746-freertos`, `stm32f746-nuttx`, `stm32f746-zephyr`) —
   these build every app config against the real HAL.  Any struct-size
   drift fails CI at compile time.
2. **Renode runtime CI jobs** (see section above) — exercise the same
   firmware at runtime, catching bugs that only manifest when code
   executes on the HAL-writing backend path.
3. **`ove lint`'s `backend-struct` rule** — forbids backend-local
   `struct ove_*` definitions outside a narrow FS allowlist.  See
   `config/ove-cli/ove/lint_backend_struct.py`.

## Stub-only toolchain variants

Built from `tests/CMakeLists.txt`:

| Target                     | Default | Flags                                      | Purpose |
|----------------------------|:-------:|---------------------------------------------|---------|
| `ove_test_stub`            | on      | `-Wall -Wextra -Werror`                     | baseline |
| `ove_test_stub_asan`       | on      | `-fsanitize=address,undefined -g`           | UAF / UB catcher |
| `ove_test_stub_coverage`   | off     | `--coverage -O0 -g` (requires `-DOVE_TEST_BUILD_COVERAGE=ON`) | feeds the `coverage` target (needs lcov/genhtml) |

## Reporting targets

| Target         | Output                                    |
|----------------|-------------------------------------------|
| `ctest`        | pass/fail per CTest entry (exit code)     |
| `junit`        | `build/junit/cmocka_tests.xml` — CMocka XML; see limitation below |
| `junit-ctest`  | `build/junit/ctest.xml` — CTest JUnit (CMake 3.21+) |
| `coverage`     | `build/coverage/html/` — lcov HTML (needs lcov/genhtml) |

### Known limitations

- **CMocka JUnit granularity.** All suites pass `tests[]` as the group
  array to `cmocka_run_group_tests(tests, ...)`. CMocka's
  `CMOCKA_XML_FILE=foo_%g.xml` expands `%g` to the array name, so every
  group writes to the same `cmocka_tests.xml` and only the last suite's
  results survive. The `junit-ctest` target (CMake 3.21+) is the
  reliable CI-facing option today; renaming each suite's group array is
  a Tier 3 follow-up.
- **NuttX build artifacts.** NuttX's `Application.mk` deposits
  `.o`/`.gcno` next to the source files when the app's `CSRCS` list
  points outside the app directory. Those files land in
  `tests/suites/` and `backends/**/`; they are `.gitignore`d but clutter
  the tree. Fix requires either copying sources into the app build dir
  or patching `Application.mk`; neither is upstream-friendly. Tracked in
  `tests/sim/nuttx-qemu/nuttx_app/Makefile` as a comment.
- **Sim variants skip stub-only suites.** Networking helpers, I2C/SPI/
  UART, PM, inference, and `static_define` run under `ove_test_stub`
  only. Moving them into the sim runners is the Tier 3 "coverage
  growth" work.
