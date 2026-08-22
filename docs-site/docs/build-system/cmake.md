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
│   ├── posix/              # POSIX backend implementation
│   ├── zephyr/             # Zephyr backend implementation
│   ├── nuttx/              # NuttX backend implementation
│   └── wasm/               # WebAssembly (Emscripten) backend implementation
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

```cmake
# In your project's CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(my_firmware C)

add_subdirectory(oveRTOS)

# Add a board (e.g. STM32F746G-DISCO with FreeRTOS backend)
add_subdirectory(oveRTOS/boards/stm32f746g-discovery)

add_executable(firmware
    src/main.c
    ${OVE_BOARD_SOURCES}      # populated by the board subdirectory
    ${OVE_BACKEND_SOURCES}    # populated by the board subdirectory
)

target_include_directories(firmware PRIVATE
    ${OVE_BOARD_INCLUDE_DIRS}
    ${OVE_BACKEND_INCLUDE_DIRS}
)

target_link_libraries(firmware PRIVATE ove)
```

Existing projects may continue to put `${OVE_SOURCES}` in their source list and `${OVE_INCLUDE_DIR}` in their include path instead of linking `ove`; do not use both forms in the same target.

## Module selection

Feature modules are enabled at compile time through Kconfig-style `CONFIG_OVE_*` preprocessor symbols. The board's CMakeLists (or the parent project) passes these as compile definitions:

```cmake
target_compile_definitions(firmware PRIVATE
    CONFIG_OVE_SYNC
    CONFIG_OVE_QUEUE
    CONFIG_OVE_TIMER
    CONFIG_OVE_EVENTGROUP
    CONFIG_OVE_STREAM
    CONFIG_OVE_WORKQUEUE
    CONFIG_OVE_FS
    CONFIG_OVE_CONSOLE
    CONFIG_OVE_TIME
    CONFIG_OVE_BOARD
    CONFIG_OVE_GPIO
    CONFIG_OVE_LED
    CONFIG_OVE_NVS
    CONFIG_OVE_WATCHDOG
    CONFIG_OVE_SHELL
    CONFIG_OVE_AUDIO
    # CONFIG_OVE_LVGL (optional, requires LVGL library)
)
```

Modules that are not enabled compile to empty inline stubs, producing no object code. The entire umbrella header `ove/ove.h` can always be included regardless of which modules are active.

## Backend selection

Each board's CMakeLists selects a backend by including the appropriate subdirectory from `backends/`. The backend populates `OVE_BACKEND_SOURCES` and `OVE_BACKEND_INCLUDE_DIRS`. The active backend is identified to the oveRTOS core via one of:

| Preprocessor symbol | Backend |
|---------------------|---------|
| `CONFIG_OVE_RTOS_FREERTOS` | FreeRTOS |
| `CONFIG_OVE_RTOS_ZEPHYR` | Zephyr |
| `CONFIG_OVE_RTOS_NUTTX` | NuttX |
| `CONFIG_OVE_RTOS_POSIX` | POSIX threads (native host or Emscripten pthreads) |

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
