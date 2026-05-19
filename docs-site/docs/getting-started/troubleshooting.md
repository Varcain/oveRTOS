# Troubleshooting

Common failure modes when setting up, building, and running oveRTOS, with the exact command that diagnoses the problem and the fix. If your symptom isn't here, run [`make doctor`](doctor.md) first — it reports everything required and optional.

## Setup and environment

### `make setup` fails with "python3-venv is not installed"

**Symptom**

```
The virtual environment was not created successfully because ensurepip is not
available.  On Debian/Ubuntu systems, you need to install the python3-venv
package using the following command.
```

**Diagnose**

```bash
python3 -m venv /tmp/_ovecheck && echo ok || echo missing
```

**Fix**

- Debian/Ubuntu: `sudo apt install python3-venv`
- Fedora: already part of `python3`, ensure it's installed
- Arch: `sudo pacman -S python`

Then rerun `make setup`.

### `make` complains about an old CMake

**Symptom** — Zephyr or NuttX configure fails with `CMake 3.20 or higher is required.`

**Diagnose**

```bash
cmake --version
```

**Fix** — install a newer CMake. On older Ubuntu LTS releases, the official Kitware APT repository is the easiest path:

```bash
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/kitware.list
sudo apt update && sudo apt install cmake
```

### `make doctor` reports `arm-gnu (downloaded)  WARN`

This means the ARM toolchain hasn't been downloaded yet. It is fetched lazily by `make download` (which `make` invokes as the first step of the pipeline) — running a build for any embedded target resolves it.

If the download stalls or fails:

```bash
make download    # retries idempotently
```

The toolchain lands under `output/toolchains/arm-gnu-toolchain-*/`. On corporate networks behind a TLS-inspecting proxy, set `HTTPS_PROXY` / `SSL_CERT_FILE` before retrying.

### `make doctor` reports `py:yaml  FAIL` or `py:jinja2  FAIL`

The Python virtual environment is incomplete. Recreate it:

```bash
rm -rf .venv
make setup
```

If that still fails, your distro's `python3-pip` may be missing. Install it (`sudo apt install python3-pip`) and retry.

## Configuration

### `make menuconfig` shows garbled text or hangs

ncurses is missing or `TERM` is wrong.

**Diagnose**

```bash
ldd $(which kconfig-mconf 2>/dev/null) 2>&1 | grep ncurses || echo "no system kconfig"
echo $TERM
```

**Fix**

- Debian/Ubuntu: `sudo apt install libncurses-dev` then `rm -rf .venv && make setup`
- WSL: ensure `TERM=xterm-256color` in your shell rc

You can also skip menuconfig entirely and stay on the dot-syntax (`make <board>.<rtos>.<app>`) flow.

### `make host.posix.example_c` reports "no matching defconfig fragment"

The fragment composer didn't find one of the three pieces. Check what's actually available:

```bash
make help
```

If the board/RTOS/app you typed isn't listed, you have a typo or the app is missing an `app.yaml`. Run `ove appgen --check` to validate every app manifest.

### Switching configs leaves stale build artifacts

`make` is incremental within a workspace, not across them. If you change config mid-stream and something looks off:

```bash
make clean       # clean the active workspace
# or
make clean-all   # nuke output/ entirely
```

Each `<board>.<rtos>.<app>` combination already gets its own `output/<board>/<rtos>/<app>/` workspace, so they don't normally collide.

## Build

### `make` fails with "undefined reference to `ove_*_create`"

You're building a zero-heap target but calling a heap-only API. The `_create()` / `_destroy()` symbols are gated by `OVE_HEAP_*` macros and not linked in zero-heap builds; this is the intended failure mode.

**Diagnose**

```bash
grep -E '^CONFIG_OVE_ZERO_HEAP' .config
```

If `=y`, switch to `OVE_*_DEFINE_STATIC()` or `_init()` / `_deinit()`. See [Two allocation modes](overview.md#two-allocation-modes).

### Cross-LTO linker errors with the Rust binding

**Symptom** — `lld` or `ld` reports `LLVM bitcode is incompatible with the target host` when linking Rust + C.

**Fix** — your `rustc` is older than 1.85, which is the minimum for the bindgen path. Update:

```bash
rustup update stable
rustc --version    # should report >= 1.85
```

The required version is recorded in `rust-toolchain.toml`.

### Zig build fails with "expected version 0.15.2"

**Diagnose**

```bash
zig version
```

**Fix** — download the matching Zig from [ziglang.org/download](https://ziglang.org/download/) or let the build system fetch it automatically:

```bash
make ensure-toolchain-zig
```

### `clang-tidy` flagged a third-party header during `make lint`

`make lint` follows the active compile database, so it walks into Zephyr / NuttX / FreeRTOS sources transitively. If you didn't touch them, this is upstream code and the warning is informational — the gating check is `make lint` for the oveRTOS surface, not every transitive header. Filter your view:

```bash
make lint 2>&1 | grep -v 'dl/'
```

## Run

### `make run` says "qemu-system-arm: command not found"

QEMU is missing for the current config.

```bash
# Debian/Ubuntu
sudo apt install qemu-system-arm

# Fedora
sudo dnf install qemu-system-arm
```

For non-ARM targets you don't need QEMU at all — POSIX builds run as native processes.

### POSIX build runs but the LVGL dashboard never opens

The browser dashboard runs on `http://localhost:8000` by default; check that nothing else is bound to that port, and that your firewall isn't blocking localhost. Headless run:

```bash
make run HEADLESS=1
```

The app still writes log lines to stdout in headless mode.

### WSL2: audio playback is choppy or out of sync

Known issue — WSL2's clock and the host's audio clock drift, so the browser-side PCM playback occasionally underruns. The cleanest fix is to switch the simulator's audio transport to native POSIX miniaudio (see project notes). For everyday development, expect a small amount of glitching; correctness of the audio graph is unaffected.

### Renode targets fail with "JVM not found"

Renode bundles its own runtime, but on some distros the symbolic launcher expects `java` on PATH. The build system downloads a portable Renode on first run into `output/tools/renode/`. If the download stalled:

```bash
make ensure-toolchain-renode
```

For Ubuntu where the portable build still complains, install a JRE: `sudo apt install default-jre`.

## Flashing hardware

### `make flash` reports "openocd: command not found"

OpenOCD is optional (not needed for QEMU or POSIX), but required to flash the STM32F746G-Discovery.

```bash
sudo apt install openocd
```

Then `make flash` should detect the board over ST-LINK.

### Flashing succeeds but the board boots into the previous app

ST-LINK occasionally needs a reset between programs:

```bash
make flash
sleep 1
# press the black reset button on the Discovery board
```

If symptoms persist, run `openocd -f interface/stlink.cfg -f target/stm32f7x.cfg -c "init; reset halt; flash erase_sector 0 0 last; exit"` once to erase the bank.

## Tests

### `make test` reports "pyserial missing"

Only needed for hardware-in-the-loop tests. Install it inside the venv:

```bash
.venv/bin/pip install 'pyserial>=3.5'
```

`make doctor` flags this as `WARN`, not `FAIL`, because nothing under POSIX or QEMU needs it.

### Renode tests time out on first run

The first run downloads ~150 MB of Renode binaries; subsequent runs reuse `output/tools/renode/`. Re-run if it timed out — it will resume the download.

## Runtime hazards

### `ove_main` locals are UB after `ove_run()`

**Symptom** — a worker thread reads garbage from a variable that `ove_main()` declared and "passed in"; the bug shows up most loudly on FreeRTOS (often as a hard fault on the first context switch) and may stay hidden on POSIX or Zephyr until you change a build flag.

**Why** — `ove_main()` runs on a temporary bootstrap stack. Any object in its automatic storage (plain locals, including arrays and structs) becomes dangling once `ove_main()` returns. `ove_run()` calls into `vTaskStartScheduler()`, which on Cortex-M resets MSP to the initial stack top — the first interrupt then overwrites the region that used to hold those locals. On POSIX / Zephyr the hosting thread never unwinds, so the memory happens to survive, which masks the same UB.

**Fix** — give long-lived state `static` storage:

```c
void ove_main(void)
{
    static struct app_ctx ctx;           /* OK — static storage */
    /* or */
    struct app_ctx *ctx_heap = malloc(sizeof(*ctx_heap));   /* OK — heap mode */
    /* but never: */
    struct app_ctx ctx_bad;              /* WRONG — automatic storage */

    ove_thread_create(&t, "worker", worker, &ctx, OVE_PRIO_NORMAL, 4096);
    ove_run();
}
```

In zero-heap mode, `static` (or file-scope) is the only option. The technical detail behind the UB is documented under [Internals → Backends → FreeRTOS backend notes](../backends/index.md#freertos-backend-notes).

## Still stuck?

- Run `make doctor` and paste the output into a GitHub issue: <https://github.com/Varcain/oveRTOS/issues>
- Search existing issues: most setup problems have a previous thread.
- Browse the [Glossary](../glossary.md) if a piece of jargon is the part you're stuck on.
