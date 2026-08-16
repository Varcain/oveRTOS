# LXP Port Migration Baseline

This is the Iteration 0 baseline for moving reusable Linux-personality and RTOS
seam logic out of the oveRTOS `linux_interop` application and into LXP. The
iteration changes no firmware source or runtime behavior. It defines ownership,
records the current cost, and adds a migration ledger that later iterations
must update explicitly.

## Baseline identity

The baseline was rebuilt on 2026-08-16 from:

- oveRTOS `1fc497b189bc`;
- LXP `21272d5d3b59`;
- STM32F746G-DISCO `linux_interop` production profile;
- 10 ms guest quantum, QSPI rootfs, networking, `/data`, raw block access,
  netfs execution, PTY, devices, display/input, and RT-scope enabled; and
- nine co-resident program regions and thirteen process slots on every engine.

Each program region is 256 KiB and each dynamic pool is 512 KiB. The generated
configuration hashes are:

| Engine | `.config` SHA-256 |
|---|---|
| FreeRTOS | `0cda1c8ac4def4b1ab730522b3b77ffa3ac1f3511cebcd21572609f5b226a012` |
| NuttX | `12f9308b518044ae8fc018587369c16655c173be8d7e4a64066434b2850bfdd8` |
| Zephyr | `b421f6dcbca1c78b6f2d9232b79858efa8630ba37af2cf1e0ea3211b2bd63e61` |

Generated configurations live under
`output/stm32f746g-discovery/<engine>/linux_interop/.config`.

## Firmware and memory baseline

The figures below come from clean current engine linker reports. Binary size is
the produced flash file; flash footprint is the linker's region accounting and
can differ where the file omits address-space gaps.

| Engine | Flash binary | Flash footprint | Internal static memory | External SDRAM |
|---|---:|---:|---:|---:|
| FreeRTOS | 310,700 B | 308,548 B | 287,992 B across regions | 8,063,824 B |
| NuttX | 352,676 B | 352,676 B | 231,204 B SRAM1 | fixed-address pools not reported by its linker |
| Zephyr | 336,688 B | 355,120 B | 257,024 B RAM + 29,568 B DTCM | 7,912,272 B |

FreeRTOS's internal placement is split across 320 B of Ethernet descriptors,
45,856 B of DSP/DTCM, and 241,816 B of RAM rather than one linker region. Its
external accounting also reserves 1,536 B of the separate Ethernet TX region.
The authoritative per-region values are:

| FreeRTOS region | Used | Capacity | Headroom |
|---|---:|---:|---:|
| DSP/DTCM | 45,856 B | 48,832 B | 2,976 B |
| RAM | 241,816 B | 262,144 B | 18,392 B |
| SDRAM | 8,063,824 B | 8,386,560 B | 59,568 B |
| Ethernet TX | 1,536 B | 2,048 B | 512 B |

Zephyr has about 5 KiB of main-RAM headroom. These narrow margins mean a seam
migration must compare all three link reports even when it is intended to be a
source-only move.

## Current source ownership

The baseline integration was 10,012 lines across the following groups. The
FreeRTOS portion has since moved to canonical LXP and is no longer an oveRTOS
migration exception.

| Group | Lines | Current owner | Intended owner |
|---|---:|---|---|
| `app.c` and `rt_scope.c` | 2,463 | oveRTOS app | app behavior remains; generic bootstrap and lifecycle move out |
| five `lxp_ove_*` host adapters | 2,507 | oveRTOS common backend | oveRTOS, narrowed to stable HAL/provider translation |
| FreeRTOS, NuttX, and Zephyr task/trap/MPU seams | 4,430 | oveRTOS backends | LXP `ports/<rtos>/` |
| Cortex-M cache/MPU/memory helpers and LXP metrics | 612 | oveRTOS backends | reusable port mechanics move to LXP; host metrics facade may remain |

The exact transitional inventory is enforced by
`tests/cmake/TestLxpPortOwnership.cmake`. It currently admits:

- `backends/nuttx/nuttx_lnx_trap.c`;
- `backends/zephyr/zephyr_lnx.c`;
- the common provider adapters and narrow FreeRTOS host-policy binding named by
  the test;
- `app.c`, `rt_scope.c`, and `rt_scope.h`; and
- `modules/lxp/ports/freertos/lxp_freertos_port.c` and its LXP-owned kernel
  patch.

These are migration exceptions, not endorsements of their current location.
Each later move must remove the old file and update the ledger atomically.

## Ownership rule

LXP owns Linux semantics and reusable RTOS mechanisms: guest task lifecycle,
trap entry, MPU profiles, executable publication, and SVC accounting. oveRTOS
owns board resources, stable-HAL provider implementations, and translation of
Kconfig into LXP configuration. The application owns workload and diagnostic
policy only.

The canonical rationale and completion test live in LXP's
`docs/port-ownership.md`. In addition to the oveRTOS ledger, LXP's
`scripts/check-decoupled.sh` prevents its portable core from including either
oveRTOS or native RTOS dependencies; those dependencies are allowed only in
explicit ports.

## Iteration acceptance rule

For every later iteration:

1. state which ownership violation is being removed;
2. move one coherent mechanism with no parallel implementation left behind;
3. update the ledger and build inventory in the same commit;
4. run the LXP contract checks and oveRTOS structural tests;
5. build all affected engines and compare flash/RAM against this baseline; and
6. perform hardware validation when task, trap, MPU, cache, or lifecycle code
   changed.

Runtime numbers are intentionally not duplicated here. The existing
[complexity remediation baseline](complexity-baseline.md) contains the detailed
functional and real-time history; this document freezes the current repository
boundary and current build cost for the new port migration.

## Iteration 1: LXP-owned Cortex-M support

Iteration 1 moves the reusable cache geometry, bounded executable-publication,
PMSAv7 descriptor decoding, live MPU snapshot, and CPU-memory-contract
validation helpers into canonical LXP under `include/lxp/arch/`. Their public
names now use the `lxp_cortex_m_*` namespace. The pure host tests moved with the
implementation.

oveRTOS removed its two 420-line duplicate architecture headers and the
188-line pure architecture test. It retains a 37-line host-policy header that
declares the uncached and STM32F746 WBWA contracts, plus a focused 57-line test
that checks those board-owned declarations against LXP's decoder. No forwarding
header or compatibility alias remains.

The generated configurations and all static-memory figures are unchanged.
Production links against LXP `3ca880f` compare with Iteration 0 as follows:

| Engine | Iteration 0 flash | Iteration 1 flash | Delta | Iteration 1 binary |
|---|---:|---:|---:|---:|
| FreeRTOS | 308,548 B | 308,556 B | +8 B | 310,708 B |
| NuttX | 352,676 B | 352,684 B | +8 B | 352,684 B |
| Zephyr | 355,120 B | 355,132 B | +12 B | 336,700 B |

The 8–12 byte text differences are within compiler/link layout noise from the
renamed inline interface. There is no configuration, data, BSS, pool, or stack
change.

All 47 canonical LXP CTest targets and all 46 oveRTOS host/structural targets
pass. The LXP header contract now recursively compiles architecture subdirectory
headers by themselves in both C11 and C++17. The oveRTOS migration guard requires
the three LXP headers and rejects either retired oveRTOS duplicate.

Each production image was then flashed to the same STM32F746G-DISCO and checked
through the Linux guest over SSH. Every engine booted with LXP `3ca880f`, exposed
the expected 13 slots and nine regions, and served both `/proc/lxp_resources`
and `/proc/rt_scope`:

| Engine | RT releases/executions | Missed / late / overrun | Dispatch avg / max |
|---|---:|---:|---:|
| FreeRTOS 11.2.0 | 11,449 / 11,449 | 0 / 0 / 0 | 7.35 / 34.67 us |
| NuttX 12.12.0 | 11,828 / 11,828 | 0 / 0 / 0 | 8.02 / 33.72 us |
| Zephyr 4.4.0 | 131,421 / 131,421 | 0 / 0 / 0 | 7.98 / 57.46 us |

These are boot smoke intervals, not workload benchmarks; they establish that
the relocated cache and MPU logic still validates live silicon and permits
unprivileged Linux guests to execute on every engine.
