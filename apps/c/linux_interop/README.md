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

The scope output owns TIM3, PB4, and PG7. Disable
`CONFIG_OVE_LINUX_RT_SCOPE` in menuconfig when the application needs any of
those resources.

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
