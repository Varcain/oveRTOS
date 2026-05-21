# Cookbook

Pattern recipes for common oveRTOS app shapes. Each page is a focused walkthrough of *how to combine modules* — the API reference tells you what each function does; the cookbook tells you what to assemble.

Every recipe is short (one page), runnable, and constrained to either the POSIX host or QEMU/FreeRTOS so the code in the docs can be copied directly into an app and built. Recipes link to the in-tree example apps for the full producer/consumer shape.

## Recipes

| # | Pattern | Modules used |
|---|---|---|
| 1 | [Periodic sensor read via timer + queue](periodic-sensor.md) | `ove_timer`, `ove_queue`, `ove_thread` |
| 2 | [Persist a value across reboots](persist-nvs.md) | `ove_nvs` |
| 3 | [Stream UART input into a worker thread](uart-worker.md) | `ove_uart`, `ove_stream`, `ove_thread` |
| 4 | [POST telemetry to HTTPS](https-post.md) | `ove_net`, `ove_net_http`, `ove_net_tls` |
| 5 | [Run TFLite Micro inference on an audio buffer](inference.md) | `ove_infer`, `ove_audio` |
| 6 | [Show a value on LVGL with thread-safe locking](lvgl-thread-safe.md) | `ove_lvgl`, `ove_mutex`, `ove_timer` |
| 7 | [Sleep and wake on a GPIO edge](pm-gpio-wake.md) | `ove_pm`, `ove_gpio` |
| 8 | [Add a custom shell command](shell-command.md) | `ove_shell`, `ove_log` |
| 9 | [Async TCP heartbeat with embassy-net](async-tcp-echo.md) | `ove_async`, `ove_async_net` (Rust only) |

## How to use a recipe

1. Read the recipe end-to-end. Each one shows a complete `ove_main()` plus any helper threads.
2. Copy the code into your app. If you don't have one yet:

   ```bash
   ove app new --lang c --name my_app
   ```

3. Make sure the `defconfig:` list in your `app.yaml` enables every module the recipe uses. Recipes call this out explicitly in their "What to enable" box.
4. Build and run:

   ```bash
   make host.posix.my_app && make && make run
   ```

   Or `make qemu.freertos.my_app` if the recipe targets QEMU.

## Conventions

- Code is C unless the pattern is meaningfully different in the language binding (rare).
- Error checking is shown but kept brief — production apps should still log and recover; the recipes optimise for *clarity*, not robustness.
- Where a recipe needs hardware-only modules (real I2C, real UART), the code uses the POSIX simulation transport via `sim/` so it still runs as a host process.

## Want more recipes?

The example apps under `apps/{c,cpp,rust,zig}/{heap,zeroheap}/` are full implementations:

| Example | Demonstrates |
|---|---|
| `example` | Producer / consumer / UI timer |
| `example_net` | Sockets, DNS, optional TLS |
| `example_pm` | Battery simulation, sleep / wake |
| `example_keyword_live` | DMIC audio + TFLite Micro micro-speech |
| `lvgl_benchmark` / `lvgl_gallery` | LVGL widgets and rendering performance |

If a pattern you need isn't covered here, the [Examples Index](../examples/index.md) is the next place to look.
