# C example: RTOS kernel ↔ Linux-personality interop

One firmware image, two worlds running side by side — a **native RTOS thread**
(`ove_thread`) and a stock **Linux program** (BusyBox, an unprivileged uClibc
`bFLT`) launched through the oveRTOS **Linux personality** (`ove_lnx_run`) out of
a real Buildroot rootfs — exchanging data **in both directions**, then handing
you an interactive shell.

This is a first-class oveRTOS framework app, built by the `ove` build system, and
the *same demo* runs on two engines through the engine-agnostic personality core:

| Engine | Board | CPU | Program isolation |
|--------|-------|-----|-------------------|
| **Zephyr**   | `qemu-mps2-an521` | Cortex-M33 | unprivileged + MPU (`CONFIG_USERSPACE`) |
| **FreeRTOS** | `qemu-mps2-an500` | Cortex-M7  | privileged (functional parity; MPU isolation is a follow-up) |

## Phase 1 — bidirectional round trip

The RTOS thread feeds three "sensor readings" **into** the Linux program's stdin
and drains what it echoes back **out** of stdout, each half crossing the
personality boundary through an oveRTOS message queue (`ove_queue`):

```
  RTOS feeder ─► ove_queue g_feed_q ─► read cb ─►┌───────────────┐
                                                 │  Linux  cat   │
  RTOS consumer ◄─ ove_queue g_consume_q ◄─ write cb ◄──────────┘
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

ove download        # first time only — fetches the engine workspace
ove build
ove run
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

The RTOS side is built entirely on the **engine-agnostic oveRTOS APIs**
(`ove_thread`, `ove_queue`, `ove_time`) and the Linux side on `ove_lnx_run`
(`include/ove/linux/run.h`); no direct kernel calls — which is why the *same*
`src/app.c` runs on both Zephyr and FreeRTOS (the only engine-specific line is
the lifecycle: on FreeRTOS the demo runs in a task because the scheduler starts
inside `ove_run()`, whereas Zephyr's `ove_main()` is already a thread).
Semihosting is the console transport (an architecture facility, not an RTOS
primitive).

The personality's I/O callbacks run in the `svc`-trap (exception) context, so
they do only non-blocking work there:

- **RTOS → Linux** — the read callback hands the program the next queued line
  with `ove_queue_receive_from_isr` (the ISR-safe, non-blocking variant). Because
  that callback *cannot block to wait*, the feeder pre-fills the queue before the
  program is launched, so it never sees a premature EOF.
- **Linux → RTOS** — the write callback pushes each line with
  `ove_queue_send_from_isr`; the RTOS worker blocks on `ove_queue_receive` in
  normal thread context.

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
console in the run script. Phase 1 runs the program *privileged* on the non-MPU
`ARM_CM7` port (functional parity); switching to the `ARM_CM4_MPU` port for
unprivileged + MPU isolation is a follow-up.

## Files

| File | Role |
|------|------|
| `app.yaml`  | framework app manifest — selects the personality + RTOS modules (`CONFIG_OVE_LINUX/ARENA/LOADER/QUEUE/TIME`) |
| `src/app.c` | the demo (`ove_main`): the `ove_thread` worker, the two `ove_queue` bridges, the interactive console, the two-phase launch |

The personality core/seam (`linux/ove_linux_syscall.c`, `backends/zephyr/zephyr_lnx.c`)
and the rootfs CPIO (`tests/ontarget/loader_rootfs_image.h`) are pulled in by the
board + the generated `ove_config.cmake`.
