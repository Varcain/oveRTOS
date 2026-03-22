# oveRTOS C++ API Reference {#mainpage}

C++20 RAII wrappers for the oveRTOS embedded RTOS framework.
These wrappers provide type-safe, move-only handles with automatic resource
cleanup, compile-time stack sizing, and fluent LVGL widget builders.

Include every module at once with the umbrella header:

```cpp
#include <ove/ove.hpp>
```

## Wrapper Classes

| Class | Header | Description |
|-------|--------|-------------|
| `ove::Thread<StackSize>` | `ove/thread.hpp` | Compile-time stack-sized thread with move semantics |
| `ove::Mutex` | `ove/sync.hpp` | Non-recursive mutex |
| `ove::RecursiveMutex` | `ove/sync.hpp` | Recursive mutex |
| `ove::Semaphore` | `ove/sync.hpp` | Counting semaphore |
| `ove::Event` | `ove/sync.hpp` | Binary event flag |
| `ove::CondVar` | `ove/sync.hpp` | Condition variable |
| `ove::LockGuard` | `ove/sync.hpp` | RAII mutex lock guard |
| `ove::Queue<T, N>` | `ove/queue.hpp` | Type-safe, fixed-depth message queue |
| `ove::Timer` | `ove/timer.hpp` | Software timer |
| `ove::EventGroup` | `ove/eventgroup.hpp` | Multi-bit event flags |
| `ove::Workqueue` / `ove::Work` | `ove/workqueue.hpp` | Deferred work queue |
| `ove::Stream` | `ove/stream.hpp` | Byte-stream ring buffer |
| `ove::Watchdog` | `ove/watchdog.hpp` | Hardware watchdog timer |
| `ove::fs::File` / `ove::fs::Dir` | `ove/fs.hpp` | RAII file and directory handles |
| `ove::Audio` | `ove/audio.hpp` | I2S audio streaming |
| `ove::Nvs` | `ove/nvs.hpp` | Non-volatile key-value storage |

## LVGL C++ Wrappers

When `CONFIG_OVE_LVGL` is enabled, `ove/lvgl.hpp` provides:

| Class | Description |
|-------|-------------|
| `ove::lvgl::LvglGuard` | RAII guard for the LVGL display lock |
| `ove::lvgl::Component<T>` | CRTP base for composable widget components |
| `ove::lvgl::State<T>` | Reactive observable value (uses `LV_USE_OBSERVER`) |
| `ove::lvgl::Label` / `ove::lvgl::Bar` / `ove::lvgl::Box` | Fluent widget builders |

## Design Principles

- **RAII**: All kernel objects are released in destructors. No manual `_destroy()` calls needed.
- **Move-only**: Handles are non-copyable, preventing double-free.
- **Compile-time configuration**: Template parameters encode stack sizes and queue depths.
- **Zero overhead**: Wrappers inline to the underlying C API with no additional indirection.

## Further Documentation

For setup, build, configuration, and example walkthroughs, see the
[oveRTOS documentation site](../index.html).
