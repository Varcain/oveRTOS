# Testing

oveRTOS ships a layered test harness that runs the same CMocka suite
sources against many backends.  Each layer trades fidelity for speed:

| Layer | Where it runs | What it catches | Typical wall time |
|---|---|---|---|
| **Stub** | host process | API contracts, pure logic | ~2 s |
| **QEMU** (`mps2/an500`) | generic Cortex-M emulator | RTOS port, scheduler, ISR plumbing | ~10 s per variant |
| **Renode** (`stm32f7_discovery-bb`) | full STM32F7 silicon model | HAL register paths, peripheral driver state machines | ~60–90 s per variant |
| **HW (HIL)** | real STM32F746G-Discovery board over OpenOCD + USART1 | silicon errata, real PHY autoneg, real wall-clock, watchdog actually resetting | ~25 s per variant + flash |

The first three are gated by CI on every PR and on `make test-all`.
The HW layer is **manual-only** — needs a board attached, never run by
CI or `make test-all`.

The remaining pages cover the two layers that aren't already documented
under [Build System → Simulation](../build-system/sim.md):

- [Renode](renode.md) — six targets (FreeRTOS / Zephyr / NuttX × heap /
  zero-heap) running the full common-suite list against a modelled
  stm32f7_discovery-bb.  Driven by `make test-renode-stm32f746-…` or
  the grouped `make test-renode`.
- [Hardware-in-the-loop](hw.md) — the same six targets flashed onto an
  actual STM32F746G-Discovery via OpenOCD, with USART1 captured by
  pyserial.  Driven by
  `OVE_HW_SERIAL_PORT=/dev/ttyACMx make test-hw-stm32f746-…`.

## Common-suite list

`tests/cmake/OveTest.cmake::OVE_TEST_COMMON_SUITES` is the single source
of truth for which suites every backend runs.  All four layers consume
the same list (Stub, QEMU, Renode, HW) — the only differences are the
build glue and the runner that captures CMocka output.  Renode-only
and HW-only tests live in `tests/suites/test_renode_stm32_*.c` and
`tests/suites/test_hw_stm32f746.c`; both files emit a single `[SKIP]`
line on backends that can't run them, keeping suite counts comparable.

## Test-target naming

Every target follows a predictable shape:

- `test-stub` — POSIX
- `test-qemu-<rtos>{,-zeroheap}` — QEMU MPS2-AN500
- `test-renode-stm32f746-<rtos>{,-zeroheap}` — Renode
- `test-hw-stm32f746-<rtos>{,-zeroheap}` — real Discovery board (manual)

`<rtos>` is one of `freertos` / `zephyr` / `nuttx`.  Group aliases:

| Group | Expansion |
|---|---|
| `make test-qemu` | all QEMU targets |
| `make test-renode` | all Renode targets |
| `make test-hw` | all HW targets — **only useful with a board attached** |
| `make test-all` | sim + QEMU + Renode (HW excluded) |

`ove test <name>` accepts the same names; `ove test all` excludes HW
the same way `make test-all` does.
