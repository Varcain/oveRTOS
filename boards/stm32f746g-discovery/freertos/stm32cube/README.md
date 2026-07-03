# Vendored STM32Cube BSP component headers

Third-party files from **STMicroelectronics STM32CubeF7** (v1.17.2), kept here
under their original license. They are **not** covered by oveRTOS's GPL-3.0 —
each file retains its own ST copyright + license header.

| File | Origin (STM32CubeF7) | License |
|------|----------------------|---------|
| `n25q128a.h` | `Drivers/BSP/Components/n25q128a/n25q128a.h` | BSD-3-Clause (© STMicroelectronics) |

## Why vendored

The Cube `Drivers/BSP/Components/*` are **git submodules** in the STM32CubeF7
repo, so the shallow sparse checkout `ove download` performs does not populate
them. The QSPI BSP (`stm32746g_discovery_qspi.c`, which *is* fetched) does
`#include "n25q128a.h"` for the N25Q128A register/opcode definitions, so the
STM32F746-Discovery QSPI support (`CONFIG_OVE_QSPI`) needs this one component
header. `LICENSE.md` in STM32CubeF7 marks "BSP Components" as BSD-3-Clause, which
permits source redistribution provided the copyright notice is retained — so it
is vendored here verbatim rather than fetched at build time.

This directory is added to the include path only for `CONFIG_OVE_QSPI` builds
(see the board `CMakeLists.txt`).
