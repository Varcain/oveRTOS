# Glossary

Terms that appear repeatedly in oveRTOS docs, source, and config — with the definition the project actually uses (some are reused from broader embedded / RTOS culture, others are project-specific).

---

### `app.yaml`

The manifest at the root of every app (in-tree or external). Declares the language, source list, required `CONFIG_OVE_*` modules, and optional app-level Kconfig fragments. The build system reads it to compose the workspace. See [External Apps](build-system/external-apps.md#appyaml).

### Backend

The concrete RTOS implementation behind the portable `ove_*` API. oveRTOS ships four: **FreeRTOS**, **Apache NuttX**, **Zephyr**, and **POSIX**. The active backend is selected at configure time via `CONFIG_OVE_RTOS_*` and resolves to direct calls at compile time — no runtime dispatch.

### Binding

A language-specific wrapper layer over the C API. oveRTOS provides C, C++ (RAII / templates), Rust (`no_std` crate), and Zig (`comptime`-generic) bindings. All bindings compile down to the same FFI symbols.

### Board

A hardware target descriptor under `boards/<name>/`. Pairs a `board.yaml` (metadata, LVGL config, audio nodes, LED descriptors) with per-RTOS subdirectories (`freertos/`, `nuttx/`, `zephyr/`) containing the BSP, linker script, and entry point.

### Board patches

`.patch` files under `boards/<board>/<rtos>/patches/` that the build system applies to downloaded RTOS sources before compilation. Used for board-specific fixes that haven't landed upstream. Always applied before app patches.

### `config_name`

A field in `app.yaml`. Becomes the third token of the dot-syntax target (`make host.posix.<config_name>`). Conventionally `<base>_<lang>_<mode>` (e.g. `example_rust_heap`).

### Defconfig

A minimal `.config` file. Two flavors:

- **defconfig (file)** — pre-baked, e.g. `qemu_freertos_example_c.defconfig`, loaded by `ove defconfig <name>`.
- **defconfig (field)** — list of `CONFIG_OVE_*=y` lines in `app.yaml`. The app declares its required modules here.

### Defconfig fragment

A `.defconfig` snippet under `config/fragments/` representing one slice of the final config (global, board, RTOS, app). The dot-syntax target `make <board>.<rtos>.<app>` composes one fragment from each layer to produce the workspace `.config`. See [Configure](getting-started/configure.md).

### Dot-syntax target

The `make <board>.<rtos>.<app>` form. Three tokens, period-separated. Each token names a fragment that the composer concatenates into the workspace `.config`. Replaces the older single-token defconfig style for most cases.

### External app

An application living outside the oveRTOS source tree. Has its own `app.yaml`, a three-line `Makefile` that includes `$(OVE_DIR)/config/make/ove_app.mk`, and an isolated `output/`. The framework discovers it via `OVE_EXTERNAL_APPS=$(APP_DIR)`. See [External Apps](build-system/external-apps.md).

### `FreeRTOSConfig.h`

The FreeRTOS kernel's master configuration header. oveRTOS generates it from the workspace `.config` via a Jinja2 template (`config/templates/FreeRTOSConfig.h.j2`). You do not edit it by hand.

### Fragment

See *defconfig fragment*.

### Heap mode

The build mode where kernel objects are allocated from the RTOS heap via `_create()` / `_destroy()`. The default. Gated by the absence of `CONFIG_OVE_ZERO_HEAP`.

### `_init()` vs `_create()`

Two parallel API shapes oveRTOS exposes for kernel objects:

- `ove_X_create(&handle, ...)` — heap-allocates the underlying storage and returns a handle. Paired with `ove_X_destroy(handle)`.
- `ove_X_init(&handle, &storage, ...)` — operates on caller-supplied storage (a `ove_X_storage_t`). Paired with `ove_X_deinit(handle)`.

The `_init()` form is available in both modes. The `_create()` form is gated by `OVE_HEAP_*` macros and produces a link error in zero-heap builds.

### Kconfig

The Linux-kernel configuration system. oveRTOS uses it as the single source of truth for module selection (`CONFIG_OVE_AUDIO=y`, etc.), backend choice (`CONFIG_OVE_RTOS_FREERTOS=y`), and toolchain settings. Interact via `make menuconfig` (TUI) or by editing `.config` directly.

### Layered config (NuttX / Zephyr)

NuttX and Zephyr have their own Kconfig systems separate from oveRTOS. The build system merges several layers (oveRTOS template base → board overlay → app overlay → user customisations) into the final RTOS-native config. See [External Apps → Configuration Layering](build-system/external-apps.md#configuration-layering).

### `make doctor` / `ove doctor`

Host environment health check. Probes Python, CMake, every compiler, every required and optional binary, and the manifest-pinned source tarballs. Exits non-zero on missing required tools. See [Doctor](getting-started/doctor.md).

### `make help`

Lists every board, every RTOS available per board, every app (with description) discovered from `apps/`. Use this as the live source of truth — it walks the tree, so new apps appear without doc changes.

### Manifest (`manifest.yaml`)

The root-level YAML that pins the versions and hashes of every third-party component (FreeRTOS, Zephyr, NuttX, LVGL, CMSIS-DSP, mbedTLS, lwIP). `make download` reads it to fetch sources reproducibly.

### `no_std`

A Rust crate attribute that opts out of the standard library. The `ove` Rust binding is `no_std` so it links into bare-metal targets. An `alloc` feature opts back in to heap-using types (`Vec`, `Box`, `Arc`) via the `ove-allocator` crate.

### `ove_main()`

The application entry point — the symbol the framework calls after `board_init` returns. Every app provides one: directly in C/C++, via `ove::main!(app_main)` in Rust, via `comptime { ove.exportMain(appMain); }` in Zig.

### `OVE_HEAP_*` macros

Compile-time gates that hide the `_create()` / `_destroy()` symbols in zero-heap builds. Defined only when `CONFIG_OVE_ZERO_HEAP` is unset. The macros mean accidental heap-mode calls in a zero-heap firmware fail at link time, not silently at runtime.

### `OVE_*_DEFINE_STATIC` macros

File-scope helpers that expand to a `static ove_X_storage_t` plus a handle, registering a constructor that calls `_init()` before `main()`. Available in **both** modes — same source compiles whether or not `CONFIG_OVE_ZERO_HEAP` is set. Sizes must be compile-time constants.

### Patches

Diff files applied to downloaded RTOS sources. Three classes: board-level (`boards/<board>/<rtos>/patches/`), app-level (`<app>/patches/<rtos>/`), and never to oveRTOS's own sources. Applied to build-tree copies; the originals in `dl/` stay pristine.

### `prj.conf`

Zephyr's project-level configuration file. oveRTOS generates the base `prj.conf` from a Jinja2 template; app-level additions are appended; user changes via `make zephyr-menuconfig` land in `rtos.config`.

### RAII (in oveRTOS)

In the C++ binding, every wrapper (`ove::Mutex`, `ove::Thread<N>`, `ove::Queue<T,N>`, …) destructs and calls `_destroy()` / `_deinit()` when it goes out of scope. In zero-heap mode the wrappers are move/copy-deleted because the kernel holds pointers into their storage.

### `rtos.config`

The user-level overlay that NuttX / Zephyr menuconfig writes when you tune kernel-level options interactively. Sits at the top of the layered config; survives rebuilds; not edited by hand.

### Sim / simulation framework

The `sim/` directory: a plugin architecture and WebAssembly bridge that lets the POSIX backend stand in for hardware peripherals (display, audio, GPIO, network, watchdog, profiler). The browser dashboard at `localhost:8000` is the user-facing transport.

### Workspace

A self-contained build output directory: `output/<board>/<rtos>/<app>/`. Holds the expanded `.config`, the generated build tree, downloaded RTOS sources (`dl/`), and the resulting `firmware.elf`. Different configs never collide.

### Zero-heap mode

The build mode (`CONFIG_OVE_ZERO_HEAP=y`) where the system heap is not linked and every kernel object lives in caller-supplied storage. `_create()` / `_destroy()` are unavailable; `_init()` / `_deinit()` and the `OVE_*_DEFINE_STATIC()` macros are the only ways to bring up objects. The intended target is safety-critical and memory-constrained firmware. See [Heap and Zero-Heap Modes](getting-started/overview.md#heap-mode-vs-zero-heap-mode).
