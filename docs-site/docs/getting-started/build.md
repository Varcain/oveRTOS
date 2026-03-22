# Build

## Full Build Pipeline

With a configuration in place (`.config` exists), run the complete pipeline:

```bash
make
```

This is equivalent to:

```bash
make download    # Fetch RTOS sources into dl/
make configure   # Generate config headers and CMake files from .config
make build       # Compile and link
```

Each step delegates to the `ove` CLI tool in `.venv/bin/ove`.

### Step by Step

If you want to run stages individually:

```bash
# 1. Download RTOS kernel sources (clones git repos or unpacks tarballs into dl/)
make download

# 2. Generate ove_config.h, CMakeLists fragments, and RTOS config files
make configure

# 3. Compile
make build
```

`make configure` re-runs automatically whenever `.config` changes.

## Build Output

Compiled artifacts are placed under `output/`:

```
output/
└── <board>_<rtos>_<app>/
    ├── build/          # CMake build directory
    ├── generated/      # Generated config headers (ove_config.h, etc.)
    └── images/         # Final binary images (.elf, .bin, .hex)
```

The workspace name is derived from the active configuration. The `.elf` file in `images/` is the primary build product for flashing or QEMU.

## Cross-Compilation

For embedded targets (`stm32f746g-discovery`, `qemu-mps2-an500`), the build uses an ARM Cortex-M7 cross-compiler with the `arm-none-eabi-` prefix and the `thumbv7em-none-eabihf` target ABI.

The compiler is selected according to the `Toolchain` Kconfig menu:

- **Download** (default) — the `ove` CLI downloads the official ARM GNU toolchain (`arm-gnu-toolchain-15.2.rel1`) automatically into `dl/` on first use
- **System** — uses `arm-none-eabi-gcc` found on `PATH`
- **Custom** — absolute path configured via `OVE_TOOLCHAIN_CUSTOM_PATH`

The POSIX/host backend compiles with the host's native GCC or Clang and requires no cross-compiler.

## Building All Configurations

To build every defconfig in sequence and report failures:

```bash
make alldefconfigs
```

This finds all files matching `defconfigs/**/*_defconfig`, loads each one, and runs the full pipeline. A summary is printed at the end listing any failures. This is the target used in CI to validate all supported configurations.

## Cleaning

```bash
make clean          # Remove build artifacts for the active workspace
make clean-all      # Remove all output/ workspaces
make distclean      # Remove output/, dl/, .venv, and .config
```
