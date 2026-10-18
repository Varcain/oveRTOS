# CMake Integration

oveRTOS is designed to be embedded into a firmware project as a subdirectory rather than built as a standalone library. This approach keeps all sources compiled into a single firmware target, which is the standard pattern for embedded projects.

## Repository structure

```
oveRTOS/
├── CMakeLists.txt          # oveRTOS CMake entry point
├── include/ove/            # Public C headers
├── src/                    # Core abstraction layer sources
├── backends/
│   ├── common/             # Shared backend utilities
│   ├── freertos/           # FreeRTOS backend implementation
│   ├── zephyr/             # Zephyr backend implementation
│   └── nuttx/              # NuttX backend implementation
└── boards/                 # Per-board CMakeLists and config
```

## How it works

`oveRTOS/CMakeLists.txt` creates the `ove` interface target. Linking it adds the portable core sources and public include directory to a firmware target. Backend and board sources remain selected separately because they depend on the configured engine and hardware.

The entry point also exports compatibility variables for older parent projects:

| Variable | Purpose |
|----------|---------|
| `OVE_SOURCES` | List of all core oveRTOS `.c` source files |
| `OVE_INCLUDE_DIR` | Path to the `include/` directory |
| `OVE_BACKENDS_DIR` | Path to the `backends/` root |
| `OVE_BACKENDS_COMMON_DIR` | Path to `backends/common/` |

The parent project adds `${OVE_SOURCES}` to its own target alongside the board-specific backend sources selected by the board's `CMakeLists.txt`.

## Adding oveRTOS to a project

`ove configure` turns the Kconfig selection into `generated/ove_config.h` (the `CONFIG_OVE_*` symbols) and `generated/ove_config.cmake`, which sets `OVE_BACKEND_SOURCES` for the chosen engine and modules. `ove build` then runs the board's own CMake project in `boards/<board>/<rtos>/`, which brings the core, the backend sources and the board support together. That is the supported way to build firmware.

A parent CMake project that adds `oveRTOS/` as a subdirectory gets the `ove` target and the variables above for the portable core; it must also put the generated directory on its include path (for `ove_config.h`) and compile the `OVE_BACKEND_SOURCES` that `ove_config.cmake` lists.

Existing projects may continue to put `${OVE_SOURCES}` in their source list and `${OVE_INCLUDE_DIR}` in their include path instead of linking `ove`; do not use both forms in the same target.

## Module selection

Feature modules are enabled in Kconfig (`CONFIG_OVE_*`, chosen with `ove menuconfig` or an app's `defconfig:` list). `ove configure` writes them to `generated/ove_config.h`, which every oveRTOS header includes; nothing is passed as a compile definition.

Modules that are not enabled compile to empty inline stubs, producing no object code. The entire umbrella header `ove/ove.h` can always be included regardless of which modules are active.

## Backend selection

The engine is a Kconfig choice too. The generated `ove_config.cmake` lists that engine's backend sources in `OVE_BACKEND_SOURCES`, and the generated `ove_config.h` identifies it to the core with one of:

| Preprocessor symbol | Backend |
|---------------------|---------|
| `CONFIG_OVE_RTOS_FREERTOS` | FreeRTOS |
| `CONFIG_OVE_RTOS_ZEPHYR` | Zephyr |
| `CONFIG_OVE_RTOS_NUTTX` | NuttX |

## Zero-heap mode

Defining `CONFIG_OVE_ZERO_HEAP` removes the `_create()` / `_destroy()` declarations entirely — they are gated behind `OVE_HEAP_*` macros which are only defined in heap mode. Apps target either the heap surface or the static surface; calling `_create()` in a zero-heap build is a link error.

```c
// Heap mode only — link error if CONFIG_OVE_ZERO_HEAP is set
ove_queue_t q;
ove_queue_create(&q, sizeof(uint32_t), 8);
```

For zero-heap apps, or for code that must compile in both modes, use `OVE_*_DEFINE_STATIC()` for file-scope objects or `_init()` / `_deinit()` with caller-supplied storage:

```c
// Works in both modes — declares storage in BSS and auto-initialises before main()
OVE_QUEUE_DEFINE_STATIC(q, sizeof(uint32_t), 8);
```

`OVE_*_DEFINE_STATIC()` always expands to `static ove_*_storage_t s; ove_*_init(&handle, &s, …)` regardless of mode, so size parameters must be compile-time constants.
