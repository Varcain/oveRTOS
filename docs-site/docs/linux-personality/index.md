# Linux Personality

Run unmodified Linux/POSIX userspace software — BusyBox, uClibc-ng, `vi`/`less`,
`ps`/`top`, pthreads — as **processes** on top of FreeRTOS, NuttX, or Zephyr, on
an **MMU-less Cortex-M**. A real Buildroot uClinux boots to `init → login →
shell` over the serial console; the *same* rootfs runs on all three engines.

!!! note "Status"
    Functional on QEMU (mps2-an500 / an521) and on real **STM32F746G-DISCOVERY**
    hardware, across all three RTOS engines. Programs run **unprivileged in
    per-program MPU regions**; a stray access is contained (killed like SIGSEGV,
    the shell survives).

## Architecture

The "kernel" is the RTOS plus a thin **Linux syscall personality**. Guest
programs are **FDPIC** dynamic executables (uClibc-ng) whose read-only text is
executed in place (XIP) from a rootfs CPIO backed by internal flash, emulator
PSRAM, or external QSPI NOR, depending on the board.

```mermaid
flowchart LR
  P["FDPIC guest process<br/>(BusyBox, /bin/sh, vi…)"] -->|"SVC (Linux syscall)"| S
  subgraph K["Linux personality (the kernel)"]
    S["syscall dispatch<br/>open/read/write/execve<br/>vfork/wait4/pipe/clock_gettime…"]
    R["run loop + coordinator<br/>schedules processes,<br/>park/wake, signals"]
    V["VFS<br/>RO CPIO + /tmp + /data"]
    S --- R
    S --- V
  end
  S --> RTOS["RTOS kernel<br/>FreeRTOS / NuttX / Zephyr"]
```

- **Processes** are RTOS threads placed in fixed, MPU-isolated regions (a small
  pool of `NREG` slots). The single-source run loop + coordinator
  (`modules/lxp/src/lxp_run.c`) schedules them: `vfork`/`exec` co-run,
  `wait4`/pipe reads block via park/wake, signals deliver at syscall boundaries.
- **Isolation** — programs run unprivileged with a per-program MPU region set
  (Zephyr K_USER domains; FreeRTOS `ARM_CM4_MPU` restricted tasks; NuttX raw MPU
  regions). A fault traps to a containment handler → the process is killed, the
  kernel and other processes are untouched.
- **Filesystem** — a read-only Buildroot rootfs (BusyBox + coreutils + editors)
  supplied as a CPIO blob, a bounded volatile `/tmp`, and an optional
  host-backed `/data` mount (microSD/FAT on STM32). `/proc`, `/dev/null`, and
  `/dev/console` are synthesised. `/data` is a separate subtree, not an overlay
  on the immutable system root.
- **No MMU** — NOMMU FDPIC only (bFLT retired); `vfork` (no copy-on-write `fork`);
  shared-library text is XIP from the board's rootfs backing store.

## Use it

```bash
# QEMU (an500 = FreeRTOS/NuttX, an521 = Zephyr)
ove defconfig-fragments qemu.freertos.linux_interop
ove download && ove build
ove run                       # boots uClinux → `overtos login:` → root → /root #

# real hardware (STM32F746G-DISCOVERY via ST-Link)
ove defconfig-fragments stm32f746.freertos.linux_interop
ove download && ove build && ove flash
#   console: /dev/ttyACM0 @ 115200
```

On FreeRTOS an500, `make run` serves the dashboard and shows `/dev/fb0` on its LCD.
Without a DMA2D accelerator, the personality copies each LVGL flush rectangle with the
CPU in one call instead of one write per scanline.
The **Terminal** panel shows the guest serial session and sends typed keys to the UART.
The **Log** panel shows other output and dashboard events.
The dashboard shows CPU usage and FreeRTOS heap usage. The **Threads** panel
lists RTOS tasks, including Linux guest slots named `lnxN`. The **Perf**
workspace shows trace swimlanes and sampling profiles, with guest code grouped
as `[Linux guest]`.
The **Debug** panel browses firmware source while QEMU's GDB server stays available for
[debugging Linux programs with `ove-fdpic-gdb`](debugging.md).
Once a frame is visible, LCD clicks and drags feed `/dev/input/event0`.
Use `make run HEADLESS=1` for a terminal-only console.

Swap `freertos` for `nuttx` or `zephyr` — the same demo runs on each. At the
shell: `uname -a`, `ls -l /`, `ps`, `top`, `cat`, a pipe (`seq 5 | wc -l`), a
background job, `vi`, then `poweroff`.

Try it in the browser:
[FreeRTOS dashboard](demo:freertos/linux_interop){:target="_blank"} ·
[NuttX](demo:nuttx/linux_interop){:target="_blank"} ·
[Zephyr](demo:zephyr/linux_interop){:target="_blank"}. Log in as `root`,
password `root`.

The FreeRTOS an500 browser dashboard also shows Terminal, Log, Threads, CPU,
RTOS heap, and trace swimlanes, shows `/dev/fb0` on the LCD and sends pointer
input to `/dev/input/event0`. The browser has no profiler or debugger. NuttX
an500 and Zephyr an521 bundles open a guest console terminal. All three use
the full `linux_interop` profile. Besides the shell, the rootfs has
`micropython` (with `sqlite3`) and `lua` (with SQLite, LuaSocket, cjson and
lfs); on the dashboard, `lvmusic >/dev/null &` draws an LVGL demo on the LCD.

```bash
make web.freertos.linux_interop                 # an500, FreeRTOS dashboard
make web.nuttx.linux_interop                    # an500, NuttX terminal
make web-an521.zephyr.linux_interop             # an521, Zephyr terminal
make && make run                                # http://127.0.0.1:8080, or the next free port
```

The bundle carries the rootfs and needs nothing else to run, so `serve/` can be
put on a static host. Set `OVE_LINUX_ROOTFS=<rootfs.cpio>` to bundle another
image than the Buildroot output. See
[Board Configurations](../build-system/boards.md).

The guest userspace is stock Buildroot (`../buildroot`, `output`). Add your
own guest programs as FDPIC binaries under `board/overtos/progs/` (see
`post-build.sh`) — that is how the test programs (`dbgdemo`, `lbench`) are built.

## More

- [Capabilities & limitations](capabilities.md) — what runs, and the current simplifications.
- [Debugging](debugging.md) — turnkey source-level GDB of FDPIC guest programs.
- [Benchmarks](benchmarks.md) — the personality tax (process vs native thread) on silicon.

LXP owns the port boundary. See `modules/lxp/docs/port-ownership.md`
and `modules/lxp/scripts/check-decoupled.sh`, which runs in LXP CI.
