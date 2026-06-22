# C example: RTOS kernel ↔ Linux-personality interop

One firmware image, two worlds running side by side — a **native RTOS thread**
(an ordinary privileged Zephyr kernel thread) and a stock **Linux program**
(BusyBox, an unprivileged uClibc `bFLT`) launched through the oveRTOS **Linux
personality** (`ove_lnx_zephyr_run`) out of a real Buildroot rootfs — exchanging
data **in both directions**, then handing you an interactive shell.

## Phase 1 — bidirectional round trip

The RTOS thread feeds three "sensor readings" **into** the Linux program's stdin
and drains what it echoes back **out** of stdout, each half crossing the
personality boundary through an ordinary RTOS kernel message queue:

```
  RTOS feeder ─► k_msgq g_feed_q ─► read cb ─►┌───────────────┐
                                              │  Linux  cat   │
  RTOS consumer ◄─ k_msgq g_consume_q ◄─ write cb ◄──────────┘
```

So an RTOS task and a Linux process exchange data both ways, concurrently, on a
NOMMU Cortex-M.

## Phase 2 — interactive shell

The program then drops into an interactive BusyBox `sh`. The read callback
returns real keystrokes (ARM semihosting `SYS_READC`) and the write callback
echoes to the console, so **you can type commands** — `ls /`, `echo hi`,
`cat /etc/hostname`, `pwd`, `echo x > /tmp/f`, … — and `exit` to finish.

## What it shows

The personality's headline value proposition: the RTOS core stays in charge (a
real kernel thread, kernel queues, real-time scheduling) while a stock Linux
binary runs as a *secondary* personality in the same image — and the two talk to
each other, both directions.

Expected output (phase 1 is deterministic; phase 2 is your session):

```
=== oveRTOS demo: a native RTOS thread + a Linux program, two-way (an521) ===

-- phase 1: RTOS thread <-> Linux program (bidirectional) --
[rtos-feeder] -> Linux: reading-1
[rtos-feeder] -> Linux: reading-2
[rtos-feeder] -> Linux: reading-3
[demo] launching the Linux program (BusyBox cat) to relay the readings...
[rtos-consumer] <- Linux (round trip #1): "reading-1"
[rtos-consumer] <- Linux (round trip #2): "reading-2"
[rtos-consumer] <- Linux (round trip #3): "reading-3"
[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.

-- phase 2: interactive BusyBox shell (type commands; `exit` to quit) --
/ # ls /
var      sys      root     mnt      lib32    etc
usr      sbin     proc     media    lib      dev
tmp      run      opt      linuxrc  init     bin
/ # echo hi from user
hi from user
/ # exit

=== interop demo done (interactive shell exited) ===
```

## Build & run

```sh
./run.sh
```

`run.sh` builds for `mps2/an521/cpu0` and launches QEMU with semihosting (the
demo exits the emulator itself). It puts your terminal in raw-ish mode for the
interactive phase and restores it on exit. It reuses the Zephyr workspace the
personality test fetches — if it is missing, run `make test-qemu-zephyr-linux`
once first.

## How it works (and its constraints)

The personality's I/O callbacks run in the `svc`-trap (exception) context, so
they do only non-blocking work there:

- **RTOS → Linux** — the read callback hands the program the next queued line
  with `k_msgq_get(K_NO_WAIT)`. Because that callback *cannot block to wait*, the
  feeder pre-fills the queue before the program is launched, so the program
  never sees a premature EOF. (A paced, blocking feed would need an MMU tier or a
  worker-pumped buffered fd; out of scope here.)
- **Linux → RTOS** — the write callback pushes each line with
  `k_msgq_put(K_NO_WAIT)`; the RTOS worker blocks on `k_msgq_get` in normal
  thread context.
- **Interactive input** — `SYS_READC` blocks the CPU in the trap while waiting
  for a keystroke, so during an input wait the RTOS threads are paused; that is
  fine here because the phase-1 worker has already finished. (The demo's two
  phases are two sequential `ove_lnx_zephyr_run` calls; the seam tears down its
  threads between runs.)

## Why this is a standalone app (not an `app.yaml` framework app)

The Linux personality needs **mps2/an521** (Cortex-M33, MPU, `CONFIG_USERSPACE`)
to run a loaded program unprivileged and trap its `svc`. The app framework's
QEMU board is **mps2-an500** (Cortex-M3) — no MPU, no userspace — so it cannot
host the personality. Hence this is a self-contained Zephyr USERSPACE app
(`CMakeLists.txt` + `prj.conf`), built directly with `west`. It links the
reusable engine seam `backends/zephyr/zephyr_lnx.c` (public API
`include/ove/linux/zephyr.h`) plus the engine-agnostic loader/arena/syscall core,
and adds the one required link option `-Wl,--wrap=z_do_kernel_oops`.

## Files

| File | Role |
|------|------|
| `src/app.c`   | the demo: the RTOS worker, the two `k_msgq` bridges, the interactive console, the two-phase launch |
| `prj.conf`    | `CONFIG_USERSPACE` + console/printk (mps2/an521) |
| `ove_config.h`| selects the engine-agnostic layers (arena + loader + Linux core) |
| `CMakeLists.txt` | links the seam + core and adds the `--wrap` |
| `run.sh`      | build for an521 + run on QEMU (with the interactive console wired up) |

The Linux rootfs is the same embedded Buildroot CPIO the personality test uses
(`tests/ontarget/loader_rootfs_image.h`).
