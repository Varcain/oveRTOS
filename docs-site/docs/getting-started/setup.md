# Setup

## Prerequisites

Install the following tools before building oveRTOS.

### Required for all targets

| Tool | Minimum version | Notes |
|---|---|---|
| Python 3 | 3.8+ | Required for the `ove` CLI and Kconfig |
| python3-venv | — | Required to create the `.venv` environment |
| CMake | 3.20+ | Used by Zephyr and NuttX build systems |
| Git | — | Source checkout and RTOS source downloads |

On Debian/Ubuntu:

```bash
sudo apt install python3 python3-venv cmake git
```

On Fedora:

```bash
sudo dnf install python3 cmake git
```

### ARM cross-compiler (for embedded targets)

Required when building for `stm32f746g-discovery` or `qemu-mps2-an500`. The `ove` CLI can download the official ARM GNU toolchain automatically (default), or you can point it at a system-installed or custom toolchain via Kconfig (`Toolchain` menu).

To install a system toolchain manually:

```bash
# Debian/Ubuntu
sudo apt install gcc-arm-none-eabi

# Or download directly from developer.arm.com
# https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
```

The default automatic download fetches `arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi`.

### QEMU (for emulated targets)

Required when the selected board is `qemu-mps2-an500`.

```bash
# Debian/Ubuntu
sudo apt install qemu-system-arm

# Fedora
sudo dnf install qemu-system-arm
```

### Rust (for Rust bindings)

Required only when `CONFIG_OVE_APP_LANG_RUST=y`. Install via rustup:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add thumbv7em-none-eabihf   # for ARM targets
```

Minimum supported Rust version: **1.85**.

### Zig (for Zig bindings)

Required only when `CONFIG_OVE_APP_LANG_ZIG=y`. Download from [ziglang.org/download](https://ziglang.org/download/).

Minimum supported Zig version: **0.13**.

### SDL2 (for POSIX/host target)

Required when the selected backend is POSIX (`host-pc` board). SDL2 provides the display and audio emulation layer.

```bash
# Debian/Ubuntu
sudo apt install libsdl2-dev

# Fedora
sudo dnf install SDL2-devel
```

## Setting Up the ove CLI

The build system runs through the `ove` CLI tool, which is installed into a local Python virtual environment. Run:

```bash
make setup
```

This will:

1. Check that `python3` and the `venv` module are available
2. Create `.venv/` in the project root
3. Install all Python dependencies from `config/requirements.txt`
4. Install the `ove` CLI from `config/ove-cli/` in editable mode

After setup, the `ove` command is available at `.venv/bin/ove` and is used automatically by every `make` target.

### Manual setup (alternative)

```bash
python3 -m venv .venv
.venv/bin/pip install -r config/requirements.txt
.venv/bin/pip install -e config/ove-cli
```

### Re-running setup

To force a clean reinstall of the virtual environment:

```bash
make setup
```

The `setup` target removes the existing stamp file and recreates the environment from scratch.
