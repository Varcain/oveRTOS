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
| **NuttX**    | `qemu-mps2-an500` | Cortex-M7  | privileged (functional parity; MPU isolation is a follow-up) |

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
calls — which is why the *same*
`src/app.c` runs on both Zephyr and FreeRTOS (the only engine-specific line is
the lifecycle: on FreeRTOS the demo runs in a task because the scheduler starts
inside `ove_run()`, whereas Zephyr's `ove_main()` is already a thread).
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
option (`-Wl,--wrap=z_do_kernel_oops`) and the rootfs-fixture include path, and
the `CONFIG_USERSPACE` knobs come from `config/templates/prj.conf.j2`.

**FreeRTOS — `qemu-mps2-an500` (Cortex-M7).** Reuses the *stock* an500 FreeRTOS
board (no dedicated board); when `CONFIG_OVE_LINUX` is set it (a) drops the sim
framework + dashboard/trace/profiler (the personality is headless), (b) drops the
`vPortSVCHandler → SVC_Handler` alias so the seam (`backends/freertos/freertos_lnx.c`)
owns the `SVC_Handler` vector and forwards FreeRTOS's start-scheduler `svc` to
`vPortSVCHandler`, and (c) adds the rootfs include + the interactive semihosting
console in the run script. The `ARM_CM4_MPU` port creates each guest with
`xTaskCreateRestrictedStatic`; program, dynamic pool, and XIP rootfs windows are
explicit MPU regions, while the coordinator remains privileged.

**NuttX — `qemu-mps2-an500` (Cortex-M7).** Also reuses the *stock* an500 board.
NuttX is the hard engine: its own `svc #0` *is* the syscall/context-switch ABI, so
the seam (`backends/nuttx/nuttx_lnx_trap.c`) `irq_attach`es SVCall and discriminates
by the program's PC range (in-region → Linux, else chain `arm_svcall`). Each program
is a real NuttX task created with `nxtask_init` given its own region as the task
stack, its initial register context set to the uClinux entry state. Two NuttX
landmines the seam handles: `task_create` makes argv[0] the task *name* (the index
is argv[1]), and `arm_doirq` treats an SVCall with `r0 == SYS_restore_context (1)`
as its own command — which a Linux `ioctl(fd=1, …)` collides with — so the handler
re-asserts `nxsched_self()->xcp.regs`. A `CONFIG_OVE_LINUX` defconfig overlay
disables the MPU (NuttX's FLAT MPU marks the program's RAM region execute-never).
Phase 1 is privileged (FLAT); unprivileged + MPU = `CONFIG_BUILD_PROTECTED` follow-up.

## Files

| File | Role |
|------|------|
| `app.yaml`  | framework app manifest — selects the personality and RTOS modules |
| `src/app.c` | the demo (`ove_main`): worker, fixed I/O staging, interactive console, and two-phase launch |

The personality core (`modules/lxp`) and the selected engine seam
(`backends/{freertos,zephyr,nuttx}/*_lnx*.c`) are pulled in by the board and the
generated `ove_config.cmake`.
