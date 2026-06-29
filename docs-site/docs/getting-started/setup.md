# Setup

Install the tools you need for the binding(s) and deployment target(s) you'll work with. Most setup is lazy — the build system fetches the ARM toolchain, Renode, and Zig automatically when first needed. You only have to install host-level prerequisites up front.

## Always required

| Tool | Minimum version | Notes |
|---|---|---|
| Python 3 | 3.9+ | Required for the `ove` CLI and Kconfig |
| python3-venv | — | Required to create the `.venv` environment |
| CMake | 3.20+ | Used by Zephyr and NuttX build systems |
| Git | — | Source checkout and RTOS source downloads |
| A native C/C++ compiler | gcc or clang | Builds the POSIX backend, the C and C++ bindings, and the framework's own tooling |

On Debian/Ubuntu:

```bash
sudo apt install python3 python3-venv cmake git build-essential
```

On Fedora:

```bash
sudo dnf install python3 cmake git gcc gcc-c++
```

## Language toolchains

oveRTOS ships first-class bindings for four languages. Install the toolchain for the binding(s) you're authoring in. The build system invokes each lazily — you can work in C++ without installing Rust or Zig.

### C / C++

Already covered above (gcc or clang). No further setup.

### Rust (≥ 1.85)

Required when authoring in Rust. Install via rustup:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add thumbv7em-none-eabihf   # for ARM embedded targets
```

The pinned toolchain version is recorded in `rust-toolchain.toml`; rustup picks it up automatically on first invocation.

### Zig (≥ 0.16.0)

Required when authoring in Zig. Two options:

- **Let the build system fetch it** (recommended for first-time setup):

  ```bash
  make ensure-toolchain-zig
  ```

  The pinned Zig lands under `output/toolchains/zig-*/` and the build system uses it transparently.

- **Install system-wide** — download from [ziglang.org/download](https://ziglang.org/download/) and add to PATH.

## Deployment-target toolchains

For embedded deployment targets (`stm32f746g-discovery`, `qemu-mps2-an500`, `qemu-mps2-an521`) you need a cross-compiler. The default automatic download fetches `arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi` for FreeRTOS/NuttX and the pinned Zephyr SDK for Zephyr — no manual install needed.

You can prefetch the Zephyr SDK explicitly with:

```bash
make ensure-toolchain-zephyr-sdk
```

To install a system toolchain instead:

```bash
# Debian/Ubuntu
sudo apt install gcc-arm-none-eabi

# Or download directly from developer.arm.com
# https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
```

Then point the build system at it via Kconfig (`Toolchain` menu).

### QEMU (for emulated targets)

Required when the selected board is `qemu-mps2-an500`:

```bash
# Debian/Ubuntu
sudo apt install qemu-system-arm

# Fedora
sudo dnf install qemu-system-arm
```

Not needed for POSIX or hardware-flash workflows.

### OpenOCD (for hardware flashing)

Required only to flash the STM32F746G-Discovery (`make flash`):

```bash
sudo apt install openocd
```

## Setting up the ove CLI

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

## Verify your environment

After setup, run:

```bash
make doctor
```

It probes every required and optional tool and reports green / yellow / red. See [Doctor](doctor.md) for what each check means.
