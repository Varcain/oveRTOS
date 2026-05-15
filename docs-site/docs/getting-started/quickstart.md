# Quickstart — your first oveRTOS app in 5 minutes

oveRTOS lets you write embedded RTOS applications in **C++**, **Rust**, or **Zig** and run them on **FreeRTOS**, **Zephyr**, or **Apache NuttX**. This page takes you from a fresh checkout to a running oveRTOS application on the host — no hardware, no cross-compilation, no rustup or zig install up front. We'll lead with **C++** because it works out of the box once you have a system C++ compiler. The other bindings only need their respective toolchains when you actually use them.

If anything fails along the way, jump to [Troubleshooting](troubleshooting.md). To verify your environment up front, run `make doctor`.

## Prerequisites — 30 seconds

You need:

- Python 3.9+ with `python3-venv`
- CMake 3.20+
- A native C/C++ compiler (gcc or clang)
- Git

Debian/Ubuntu:

```bash
sudo apt install python3 python3-venv cmake build-essential git
```

Fedora:

```bash
sudo dnf install python3 cmake gcc gcc-c++ git
```

Cross-compilation, QEMU, Rust and Zig are **not** needed for this quickstart. The build system installs Rust and Zig lazily — you only need them when you target their bindings. See [Setup](setup.md) for the full toolchain matrix.

## Five steps to a running app

```bash
# 1. Clone the repo
git clone https://github.com/Varcain/oveRTOS.git
cd oveRTOS

# 2. Bootstrap the build CLI (creates .venv/, installs the `ove` tool)
make setup

# 3. Verify your environment
make doctor

# 4. Load a configuration: <board>.<rtos>.<app>
make host.posix.example_cpp

# 5. Build and run
make
make run
```

That's it. You should see the C++ example app logging producer/consumer counter values to the console, and (if a browser is reachable) the LVGL sim dashboard opening with a live bar widget.

### What you just did

| Step | Command | What it does |
|------|---------|--------------|
| 2 | `make setup` | Creates `.venv/`, installs the `ove` Python CLI in editable mode |
| 3 | `make doctor` | Probes every required and optional tool, reports green/yellow/red. See [Doctor](doctor.md) |
| 4 | `make host.posix.example_cpp` | Loads the `host.posix.example_cpp` defconfig fragment (POSIX backend, C++ example app) into `.config` |
| 5a | `make` | Runs `download → configure → build` in one go |
| 5b | `make run` | Launches the built firmware as a native POSIX process |

## Try a different binding

The same producer/consumer example exists in every supported language. Swap the third token in step 4:

```bash
make host.posix.example_cpp       # C++ — what you just ran
make host.posix.example_rust      # Rust  (needs rustup, see Setup)
make host.posix.example_zig       # Zig   (needs zig 0.15.2, see Setup)
make host.posix.example_c         # plain C — the binding substrate
```

Each compiles down to the same FFI symbols, with [near-native overhead](../benchmarks/index.md).

## Pick a different target

The pattern is always `make <board>.<rtos>.<app>`:

| You want to… | Try this |
|---|---|
| C++ example on POSIX host (this quickstart) | `make host.posix.example_cpp` |
| Rust example on QEMU running FreeRTOS | `make qemu.freertos.example_rust` |
| Zig example on QEMU running NuttX | `make qemu.nuttx.example_zig` |
| C++ example on STM32F7 Discovery + Zephyr | `make stm32f746.zephyr.example_cpp` |
| Zero-heap C++ example on POSIX host | `make host.posix.example_cpp_zh` |

After loading a config, `make && make run` always rebuilds and launches. Run `make help` to list every board, RTOS, and app pulled from the live tree.

## See it run before you build

If you'd rather watch the same example execute in your browser without installing anything, the public live demos run the WASM build of each app:

- [C++ example (heap)](https://varcain.github.io/oveRTOS/example_cpp_heap/){:target="_blank"}
- [Rust example (heap)](https://varcain.github.io/oveRTOS/example_rust_heap/){:target="_blank"}
- [Zig example (heap)](https://varcain.github.io/oveRTOS/example_zig_heap/){:target="_blank"}
- [C example (heap)](https://varcain.github.io/oveRTOS/example_c_heap/){:target="_blank"}
- [LVGL gallery (C++)](https://varcain.github.io/oveRTOS/lvgl_gallery_cpp_heap/){:target="_blank"}

The same `firmware.elf` would boot on a STM32F746G-DISCO; the dashboard is the simulation transport, not a fake.

## Next steps

You now have a working oveRTOS checkout. Common next moves:

| Goal | Read |
|------|------|
| Understand the architecture | [Architecture Overview](overview.md) |
| Pick modules for your app (threads, queues, audio, net, …) | [API Reference](../api/index.md) |
| Browse runnable example code in every language | [Examples Index](../examples/index.md) |
| Create your own app outside the oveRTOS source tree | [External Apps](../build-system/external-apps.md) — or just run `ove app new --lang cpp --name my_app` |
| Common patterns: read a sensor, persist a value, post to HTTPS, run inference | [Cookbook](../cookbook/index.md) |
| Port existing code from raw FreeRTOS / Zephyr / bare C | [Migration](../migration/from-freertos.md) |
| Tune the workspace interactively | [Configure](configure.md) → `make menuconfig` |
| Hit a problem | [Troubleshooting](troubleshooting.md) |

## Recap

```bash
git clone https://github.com/Varcain/oveRTOS.git && cd oveRTOS
make setup && make doctor
make host.posix.example_cpp && make && make run
```

Three commands, one running app. Welcome to oveRTOS.
