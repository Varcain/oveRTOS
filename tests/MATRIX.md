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

## Hardware-specific storage layouts (STM32 IWDG etc.)

Hardware-specific backend structs (e.g. the STM32 `struct ove_watchdog`
that embeds `IWDG_HandleTypeDef`) are compiled only by the STM32 target
builds, not by any of the test backends above.  Drift between what the
backend writes and what consumers see in `ove_storage_<rtos>.h` is
caught by two gates:

1. **`_Static_assert(sizeof(struct ove_X) == sizeof(ove_X_storage_t))`**
   in every backend `.c` — see `backends/freertos/freertos_watchdog.c`
   for the watchdog example.  Any local `struct` redefinition that
   diverges from the header fails the build.
2. **STM32 build-only CI jobs** in `.github/workflows/alldefconfigs.yml`
   (`stm32f746-freertos`, `stm32f746-nuttx`, `stm32f746-zephyr`) —
   these build every app config against the real HAL. Combined with
   (1), any future struct-size drift fails CI even without hardware.

The `ove lint` `backend-struct` rule additionally forbids backend-local
`struct ove_*` definitions (with a narrow FS allowlist); see
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
