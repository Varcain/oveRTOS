# C example: RTOS kernel ↔ Linux-personality interop

One firmware image, two worlds running side by side — a **native RTOS thread**
(`ove_thread`) and a stock **Linux program** (BusyBox, an unprivileged uClibc
FDPIC ELF) launched through the oveRTOS **Linux personality** (`lxp_run`) out of
a real Buildroot rootfs — exchanging data **in both directions**, then handing
you an interactive shell.

This is a first-class oveRTOS framework app, built by the `ove` build system, and
the *same demo* runs on **all three RTOS engines** through the engine-agnostic
personality core:

| Engine | Board | CPU | Program isolation |
|--------|-------|-----|-------------------|
| **Zephyr**   | `qemu-mps2-an521` | Cortex-M33 | unprivileged + MPU (`CONFIG_USERSPACE`) |
| **FreeRTOS** | `qemu-mps2-an500` | Cortex-M7  | unprivileged + MPU (`ARM_CM4_MPU`) |
| **NuttX**    | `qemu-mps2-an500` | Cortex-M7  | unprivileged + MPU (`CONTROL.nPRIV` + `PRIVDEFENA`, programmed by the seam in `BUILD_FLAT`) |

## Phase 1 — bidirectional round trip

The RTOS thread feeds three "sensor readings" **into** the Linux program's stdin
and drains what it echoes back **out** of stdout. Fixed, pre-staged arrays keep
the demonstration allocation-free and make EOF deterministic:

```
  RTOS feeder ─► fixed feed array ─► read cb ─►┌───────────────┐
                                               │  Linux  cat   │
  RTOS consumer ◄─ fixed result array ◄─ write cb ◄──────────┘
```

## Phase 2 — interactive shell

The program then drops into an interactive BusyBox `sh`. The read callback
returns real keystrokes (semihosting `SYS_READC`) and the write callback echoes
to the console, so **you can type commands** — `ls /`, `echo hi`,
`cat /etc/hostname`, `pwd`, `echo x > /tmp/f`, … — and `exit` to finish.

## Two-channel host real-time proof (STM32F746G-DISCO)

The hardware build enables a scope-friendly experiment by default. It keeps
running throughout phase 2, so the same capture can compare an idle shell with
Linux userspace under display, syscall, and CPU load.

| Scope channel | Arduino pin | STM32 signal | Meaning |
|---------------|-------------|--------------|---------|
| CH1 | D3 | PB4 / TIM3_CH1 | 1 kHz hardware reference, 50 us high |
| CH2 | D4 | PG7 / GPIO | highest-priority host thread executing fixed work |

Connect both probe grounds to a board GND pin. Trigger on the CH1 rising edge,
start around 10 us/div horizontally, and show at least 1 ms of history when
checking missed periods. TIM3 raises CH1 in hardware and generates its update
interrupt at the same deadline. The interrupt signals an oveRTOS event; an
`OVE_PRIO_CRITICAL` host thread raises CH2 as soon as the selected engine
schedules it.

The capture has three directly readable quantities:

- CH1 period: timer stability (nominally 1.000 ms).
- CH1 rising edge to CH2 rising edge: interrupt-to-host-thread dispatch latency.
- CH2 pulse width: execution time of the same fixed host calculation.

The firmware independently measures the same path at 54 MHz and prints a
fresh timing window plus lifetime failure counters every 10 seconds:

```text
[rt-scope] window releases=10082 exec=10082 missed=0 late-finish=0 | total releases=180831 exec=180831 missed=0 late-finish=0 irq-overrun=0 pending=0
[rt-scope] dispatch-us min=7.30 avg=10.13 p99<=20 p99.9<=250 max=364.80 jitter=357.50
[rt-scope] work-us min=5.15 max=5.37 late-finish=0
[rt-scope] svc-us window calls=11430 min=10.87 avg=12.20 max=13.47 syscall=413(pselect6_time64)
[rt-scope] svc-total calls=102176 avg-us=12.20 max-us=13.69 syscall=403(clock_gettime64)
```

`missed` counts timer releases for which no distinct response execution began.
`irq-overrun` is the subset recovered after multiple hardware releases collapsed
into one pending TIM3 interrupt, and `late-finish` counts responses that crossed
the following 1 ms release. `pending` is an instantaneous release already
scheduled but not yet started; it is not counted as missed. The software report
adds a few register accesses to the measured path, so keep the GPIO capture as
the independent physical cross-check.

On Zephyr, TIM3 runs at ordinary IRQ priority 0 (the highest kernel-callable
level). Ethernet runs at 2; LTDC, QSPI, USART1, EXTI, and DMA2 run at 3. The
scope ISR can therefore post its event ahead of the active display/network
peripherals without using Zephyr's zero-latency class, whose handlers could not
call the event API.

On FreeRTOS and Zephyr, `svc-us` measures wall-clock cycles from entry into the
C portion of the Linux guest's SVC handler through syscall dispatch/parking and
register write-back. The small assembly entry/exit shim and the statistics
update itself are outside the interval. `syscall` is the ARM EABI syscall number
carried in `r7` (all guest calls use the same `svc #0` instruction); the
parenthesised name is `?` for a number not in the reporter's compact name table.
The window row is reset every 10 seconds, while `svc-total` retains the lifetime
maximum and the syscall that produced it.

Zephyr also prints `irq-lock-us`: the count, average, and maximum duration of
the coordinator's IRQ-masked process-table snapshots for both the current
window and the whole run. Its measurement update happens after IRQs are
restored, so the instrumentation does not lengthen the reported interval.

Contained Zephyr guest faults do not write the normal multi-line register dump
from exception context. The seam preserves CFSR/HFSR, fault-address registers,
PC, and the number of suppressed dump lines in `g_zephyr_lxp_fault_diag`, then
the coordinator emits the existing single `[lxp] guest-exit ...` line. A fault
in privileged Zephyr or oveRTOS code still receives Zephyr's full dump and
halts; the dump hook does not hide host failures.

Ignore the first cycle while arming the scope. Then save an idle baseline before
starting the load. At the guest prompt, use a bounded set of background jobs so
the personality's process slots are stressed without being exhausted:

```sh
lvmusic &
gui=$!
yes >/dev/null &
cpu=$!
while :; do cat /proc/stat /proc/lxp_resources >/dev/null; done &
sys=$!
```

Interact with `lvmusic` on the touch panel while capturing persistence or a long
single-shot acquisition. The GUI drives framebuffer/DMA2D and input paths,
`yes` keeps a guest runnable, and the loop adds repeated procfs reads plus
process/syscall churn. Stop the load without rebooting, then capture recovery:

```sh
kill "$sys" "$cpu" "$gui"
wait
```

For each FreeRTOS, NuttX, and Zephyr image, record the maximum CH1-to-CH2 delay,
whether any CH2 response is absent between adjacent CH1 edges, and the widest
CH2 pulse. A useful acceptance limit must come from the application's timing
budget; this demo exposes the worst observed value rather than inventing one.
The experiment demonstrates that this configured, highest-priority host path
preempts the personality workload. It is not by itself a proof for every ISR,
priority, critical section, or peripheral path in a product.

The scope output owns TIM3, pinless timebase TIM5, PB4, and PG7. Disable
`CONFIG_OVE_LINUX_RT_SCOPE` in menuconfig when the application needs any of
those resources.

## Supported profiles

The app has four supported profiles. They all compile the same
`linux_interop/src/app.c`, engine seam, coordinator, slot state machine, and
world validator. A profile selects optional personality subsystems and
instrumentation; it does not select an alternative lifecycle implementation.

| Profile | App config name | Devices, FB, DMA2D, input | Network | Read-only 9P | Remote exec | PTY | Coordinator latency | Stack canaries | RT scope |
|---------|-----------------|----------------------------|---------|--------------|-------------|-----|---------------------|----------------|----------|
| Minimal | `linux_interop_minimal` | no | no | no | no | no | no | default (off) | off |
| Full compatibility | `linux_interop` | yes | yes | yes | yes | yes | no | default (off) | board default |
| Diagnostic | `linux_interop_diagnostic` | yes | yes | yes | yes | yes | yes + debug log | default (off) | board default |
| Hardened | `linux_interop_hardened` | yes | yes | yes | no | yes | no + warning log | on | off |

Remote exec stages a fetched image in an MPU-contained RW+XN program region,
then overlays the exact copied-text prefix RO+X before launch. Full and
Diagnostic enable that path. Hardened still disables remote execution to
retain the narrower attack surface while keeping read-only remote files,
networking, display/input, and PTYs.
Minimal pins all optional personality subsystems off, including RT scope, so a
new Kconfig default cannot silently grow the baseline.

`CONFIG_OVE_LINUX_RT_SCOPE` is a hardware-board default: it resolves off on
QEMU because it depends on STM32F746G-DISCO, and on for Full and Diagnostic on
that board. Minimal and Hardened explicitly keep it off. All profiles preserve
the generated world checks and lifecycle command protocol; Diagnostic adds
timing detail around the same transitions.

The supported CI matrix is FreeRTOS, NuttX, and Zephyr crossed with all four
profiles (12 builds). The all-defconfig workflow compiles that matrix on QEMU
and STM32F746G-DISCO whenever the corresponding engine supports the board.

### Resource cost

The following reproducible QEMU build is the profile budget gate. Sizes are
bytes. Flash image is the generated binary; the value in parentheses is
Zephyr's linker-reported FLASH span. Internal BSS is the literal `.bss`
section, excluding separately reserved general RTOS heaps and main stacks.
External pools include all program regions, dynamic-link pools, any externally
resident per-slot cold captures, and the optional 256 KiB remote-exec staging
area.

| Engine | Profile | `NSLOT` / `NREG` | Flash image | Internal BSS | External pools | Cold / slot | Native stack / slot |
|--------|---------|------------------|-------------|--------------|----------------|-------------|---------------------|
| FreeRTOS | Minimal | 9 / 5 | 601,524 | 451,904 | 3,944,760 | 1,400 | 1,024 |
| FreeRTOS | Full | 8 / 4 | 1,079,700 | 776,540 | 3,419,072 | 1,400 | 1,024 |
| FreeRTOS | Diagnostic | 8 / 4 | 1,086,472 | 781,084 | 3,419,072 | 1,400 | 1,024 |
| FreeRTOS | Hardened | 9 / 5 | 1,099,720 | 782,820 | 3,944,760 | 1,400 | 1,024 |
| NuttX | Minimal | 10 / 6 | 225,228 | 204,148 | 4,718,592 | 1,400 | 1,024 |
| NuttX | Full | 10 / 6 | 296,384 | 232,840 | 4,980,736 | 1,400 | 1,024 |
| NuttX | Diagnostic | 10 / 6 | 297,780 | 235,968 | 4,980,736 | 1,400 | 1,024 |
| NuttX | Hardened | 10 / 6 | 295,640 | 232,832 | 4,718,592 | 1,400 | 1,024 |
| Zephyr | Minimal | 5 / 1 | 107,848 (113,992) | 92,845 | 793,432 | 1,400 | 1,024 |
| Zephyr | Full | 5 / 1 | 218,656 (225,824) | 115,055 | 1,055,576 | 1,400 | 1,024 |
| Zephyr | Diagnostic | 5 / 1 | 220,432 (227,600) | 117,235 | 1,055,576 | 1,400 | 1,024 |
| Zephyr | Hardened | 5 / 1 | 217,944 (225,112) | 115,043 | 793,432 | 1,400 | 1,024 |

These are board-layout costs, not portable claims about the engines. On the
STM32 board, NuttX places its cold captures and native slot stacks in SDRAM
rather than internal BSS. Zephyr/AN521 reserves the lower 15,296 KiB of PSRAM
for the runner-loaded rootfs and gives the remaining 1,088 KiB to
`OVE_PROG_RAM`; Full and Diagnostic currently use 94.75% of that window.
FreeRTOS' QEMU linker emits `.bss` as loadable `PROGBITS`, so its generated
flash image includes those zero-filled bytes; the table intentionally reports
the actual artifact rather than only `.text`.

## Build & run

```sh
# Zephyr / Cortex-M33 (an521):
ove defconfig-fragments qemu-mps2-an521.zephyr.linux_interop
# …or FreeRTOS / Cortex-M7 (an500):
ove defconfig-fragments qemu-mps2-an500.freertos.linux_interop
# …or NuttX / Cortex-M7 (an500):
ove defconfig-fragments qemu-mps2-an500.nuttx.linux_interop

ove download        # first time only — fetches the engine workspace
ove build
ove run
```

Replace the final component with `linux_interop_minimal`,
`linux_interop_diagnostic`, or `linux_interop_hardened` to select another
profile. For example:

```sh
ove defconfig-fragments stm32f746g-discovery.nuttx.linux_interop_hardened
ove configure
ove build
```

FreeRTOS has an independent Linux guest ABI choice under `ove menuconfig`:
`Linux guest floating-point calling convention`. The default soft-float guest
uses Buildroot `output`; the Cortex-M7 hard-float guest uses
`output-hardfloat` and enables full `s0-s31`/`FPSCR` preservation across parked
syscalls. This does not change the host firmware choice under
`ARM floating-point calling convention`, so hard and softfp host images can use
the same hard-float guest rootfs. Set `Buildroot output subdir override` only
when an ABI-compatible out-of-tree Buildroot directory is required.

Build and audit that rootfs first with:

```sh
make -C ../buildroot O=output-hardfloat overtos_fdpic_hardfloat_defconfig
make -C ../buildroot O=output-hardfloat
```

The opt-in QEMU regression configures the hard guest, builds the firmware, and
runs `/usr/bin/fpcheck` directly after the normal interop phase:

```sh
ove test qemu-freertos-linux-hardfloat
```

On STM32F746G-DISCO, `/usr/bin/sigctx` provides the corresponding hardware
regression for nested signal return. It nests SIGUSR2 inside SIGUSR1 and checks
both complete VFP contexts after LIFO return.

`ove run` launches QEMU with an interactive semihosting console (phase 1 is
deterministic; phase 2 is your session):

```
=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===

-- phase 1: RTOS thread <-> Linux program (bidirectional) --
[rtos-feeder] -> Linux: reading-1/2/3
[rtos-consumer] <- Linux (round trip #1/2/3 @ … ms): "reading-1/2/3"
[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.

-- phase 2: interactive BusyBox shell (type commands; `exit` to quit) --
/ # ls /
var      sys      root     mnt      lib32    etc
usr      sbin     proc     media    lib      dev
tmp      run      opt      linuxrc  init     bin
/ # exit

=== interop demo done (interactive shell exited) ===
```

## How it works (and its constraints)

The RTOS side is built on the **engine-agnostic oveRTOS APIs** (`ove_thread`,
`ove_time`) and the Linux side on the engine-neutral LXP port; no direct kernel
calls — which is why the *same* `src/app.c` runs on Zephyr, FreeRTOS and NuttX
(the only engine-specific line is the lifecycle: on FreeRTOS the demo creates a
task because the scheduler starts inside `ove_run()`, whereas Zephyr and NuttX
already call `ove_main()` with their schedulers running).
Semihosting is the console transport (an architecture facility, not an RTOS
primitive).

The `svc` handler is a bounded top half: it snapshots an ordinary syscall into a
fixed per-slot mailbox and parks that guest. I/O callbacks run later in the
privileged, preemptible coordinator task. Higher-priority host tasks therefore
retain their real-time priority, but callbacks must still return within a finite
host-defined interval because one coordinator serializes deferred guest work:

- **RTOS → Linux** — the feeder publishes all input lines before launch; the read
  callback advances a bounded index, so exhaustion is genuine EOF.
- **Linux → RTOS** — the write callback copies each bounded result into a fixed
  result array and publishes the count after the bytes are complete.

## The boards

**Zephyr — `qemu-mps2-an521` (Cortex-M33).** Runs the program *unprivileged* with
its `svc` trapped via `CONFIG_USERSPACE`. A minimal M33 USERSPACE board (no
LVGL/audio sim); when `CONFIG_OVE_LINUX` is set it adds the one engine-seam link
option (`-Wl,--wrap=z_do_kernel_oops`) and the rootfs-fixture include path. The
`CONFIG_USERSPACE` knobs come from `config/templates/prj.conf.j2`; illegal guest
accesses terminate only the current Linux process through the fatal-error hook.

**FreeRTOS — `qemu-mps2-an500` (Cortex-M7).** Reuses the *stock* an500 FreeRTOS
board (no dedicated board); when `CONFIG_OVE_LINUX` is set it (a) drops the sim
framework + dashboard/trace/profiler (the personality is headless), (b) drops the
`vPortSVCHandler → SVC_Handler` alias so the seam (`backends/freertos/freertos_lnx.c`)
owns the `SVC_Handler` vector and forwards FreeRTOS's start-scheduler `svc` to
`vPortSVCHandler`, and (c) makes the run script inject the rootfs CPIO into PSRAM
and attach the interactive console. The `ARM_CM4_MPU` port creates each guest with
`xTaskCreateRestrictedStatic`; program, dynamic pool, and XIP rootfs windows are
explicit MPU regions, while the coordinator remains privileged.

**NuttX — `qemu-mps2-an500` (Cortex-M7).** Also reuses the *stock* an500 board.
NuttX is the hard engine: its own `svc #0` *is* the syscall/context-switch ABI, so
the seam (`backends/nuttx/nuttx_lnx_trap.c`) `irq_attach`es SVCall and identifies a
Linux syscall by the saved `CONTROL.nPRIV` bit plus the current personality slot;
only a privileged NuttX SVC may chain to `arm_svcall`. Each program is a real NuttX
task created with `nxtask_init`, and both launch and resume set `CONTROL.nPRIV` in
its saved context before activation. The kernel remains `CONFIG_BUILD_FLAT`, but
that does not make these guest tasks privileged: the seam deliberately keeps
NuttX's `CONFIG_ARM_MPU` off, programs the hardware MPU itself with `PRIVDEFENA`,
and uses a scheduler note driver to grant regions 2+3 only to the incoming
program. MemManage/BusFault/UsageFault handlers contain an illegal guest access as
SIGSEGV. `CONFIG_BUILD_PROTECTED` is neither needed nor used by this personality.

## Files

| File | Role |
|------|------|
| `app.yaml`  | framework app manifest — selects the personality and RTOS modules |
| `src/app.c` | the demo (`ove_main`): worker, fixed I/O staging, interactive console, and two-phase launch |
| `src/rt_scope.c` | shared physical scope experiment with thin per-engine IRQ attachment |

The personality core (`modules/lxp`) and the selected engine seam
(`backends/{freertos,zephyr,nuttx}/*_lnx*.c`) are pulled in by the board and the
generated `ove_config.cmake`.
