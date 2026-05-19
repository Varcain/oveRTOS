# Architecture Overview

A typed binding (C, C++, Rust, or Zig) sits on top of a portable C API that resolves to the selected RTOS at **compile time** — `CONFIG_OVE_RTOS_FREERTOS` / `..._NUTTX` / `..._ZEPHYR` / `..._POSIX` picks the backend, the preprocessor selects the corresponding source tree, and the compiler sees exactly one implementation per function. No vtables, no runtime branch on the backend identity.

```mermaid
flowchart TD
    A["<b>Application Code</b><br>C · C++ · Rust · Zig"]
    B["<b>oveRTOS API Layer</b><br>thread · sync · queue · timer · audio · fs · lvgl …"]
    C["<b>CONFIG_OVE_RTOS_*</b><br>compile-time dispatch via Kconfig"]

    A --> B --> C

    C --> D & E & F & G

    subgraph backends [" "]
        direction LR
        D["FreeRTOS<br>backend"] --> H["FreeRTOS<br>kernel"]
        E["NuttX<br>backend"] --> I["NuttX<br>kernel"]
        F["Zephyr<br>backend"] --> J["Zephyr<br>kernel"]
        G["POSIX<br>backend"] --> K["pthreads<br>sim"]
    end

    style A fill:#5c6bc0,stroke:#3949ab,color:#fff
    style B fill:#1e88e5,stroke:#1565c0,color:#fff
    style C fill:#f57c00,stroke:#e65100,color:#fff
    style D fill:#43a047,stroke:#2e7d32,color:#fff
    style E fill:#43a047,stroke:#2e7d32,color:#fff
    style F fill:#43a047,stroke:#2e7d32,color:#fff
    style G fill:#43a047,stroke:#2e7d32,color:#fff
    style H fill:#546e7a,stroke:#37474f,color:#fff
    style I fill:#546e7a,stroke:#37474f,color:#fff
    style J fill:#546e7a,stroke:#37474f,color:#fff
    style K fill:#546e7a,stroke:#37474f,color:#fff
    style backends fill:none,stroke:none
```

## Two allocation modes

Every primitive has a heap-mode constructor (`_create()` / `_destroy()`) and a static-storage constructor (`_init()` / `_deinit()` with caller-owned `ove_*_storage_t`, or the `OVE_*_DEFINE_STATIC` one-step macro).

| Constructor | Heap mode (default) | Zero-heap (`CONFIG_OVE_ZERO_HEAP=y`) |
|---|---|---|
| `_create()` / `_destroy()` | Allocate / free from RTOS heap | **Symbol not declared** — calling is a link error |
| `_init()` / `_deinit()` | Caller-supplied storage | Caller-supplied storage |
| `OVE_*_DEFINE_STATIC()` | File-scope static + auto-init | Same — portable across both modes |

Apps that need both modes pick `OVE_*_DEFINE_STATIC` because that macro alone compiles identically in either configuration. The link-error in zero-heap mode is intentional — no silent fallback.

## See also

- [API Reference](../api/index.md) — every module
- [Build System → CMake](../build-system/cmake.md) — how the configure-time choice happens
- [Internals → Backends](../backends/index.md) — implementation details per RTOS
