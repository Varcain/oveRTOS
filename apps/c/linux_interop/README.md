# C example: RTOS kernel ↔ Linux-personality interop

One firmware image, two worlds running **side by side**:

- a **native RTOS thread** — an ordinary privileged Zephyr kernel thread — that
  blocks on a kernel message queue and processes whatever it receives, in real
  time; and
- a stock **Linux program** — BusyBox, an unprivileged uClibc `bFLT` — launched
  through the oveRTOS **Linux personality** (`ove_lnx_zephyr_run`) out of a real
  Buildroot rootfs.

They **interop** across the personality boundary. The Linux program's `stdout`
is handed — by the host write callback, straight from the `svc` trap — into the
RTOS kernel's `k_msgq`; the native RTOS thread drains that queue and acts on
each line. So a Linux process and an RTOS task exchange data through an ordinary
RTOS kernel object, concurrently, on a NOMMU Cortex-M.

```
   BusyBox `sh -c "echo measurement-1; ..."   (unprivileged user thread, Linux ABI)
        │  write(1, "measurement-1\n")
        ▼  svc  ──trap──►  ove_lnx_zephyr_run seam  ──►  demo_write()
                                                            │ k_msgq_put(K_NO_WAIT)
                                                            ▼
                                                   ┌──────────────────┐
                                                   │  k_msgq g_to_rtos │   (RTOS kernel object)
                                                   └──────────────────┘
                                                            │ k_msgq_get()
                                                            ▼
                                          native RTOS worker thread  ──►  logs in real time
```

## What it shows

This is the personality's headline value proposition: the RTOS core stays in
charge (a real kernel thread, kernel queue, real-time scheduling) while a stock
Linux binary runs as a *secondary* personality in the same image — and the two
talk to each other.

Expected output (QEMU, deterministic):

```
=== oveRTOS demo: a native RTOS thread + a Linux program, side by side (an521) ===
[demo] launching the Linux program (BusyBox sh) via the personality...
[rtos-worker] native Zephyr thread up; blocking on the kernel msgq for Linux data
[rtos-worker] consumed from Linux #1 @ 60 ms: "measurement-1"
[rtos-worker] consumed from Linux #2 @ 120 ms: "measurement-2"
[rtos-worker] consumed from Linux #3 @ 180 ms: "measurement-3"
[rtos-worker] Linux side finished; worker exiting
[demo] the RTOS worker received all 3 measurements from the Linux process.
=== interop demo OK: RTOS kernel + Linux personality ran concurrently ===
```

## Build & run

```sh
./run.sh
```

`run.sh` builds for `mps2/an521/cpu0` and launches QEMU with semihosting (the
demo exits the emulator itself). It reuses the Zephyr workspace the personality
test fetches — if it is missing, run `make test-qemu-zephyr-linux` once first.

## Why this is a standalone app (not an `app.yaml` framework app)

The Linux personality needs **mps2/an521** (Cortex-M33, MPU, `CONFIG_USERSPACE`)
to run a loaded program unprivileged and trap its `svc`. The app framework's
QEMU board is **mps2-an500** (Cortex-M3) — no MPU, no userspace — so it cannot
host the personality. Hence this is a self-contained Zephyr USERSPACE app
(`CMakeLists.txt` + `prj.conf`), built directly with `west`, rather than an
`app.yaml` app driven by the framework. It links the reusable engine seam
`backends/zephyr/zephyr_lnx.c` (public API `include/ove/linux/zephyr.h`) plus the
engine-agnostic loader/arena/syscall core, and adds the one required link option
`-Wl,--wrap=z_do_kernel_oops`.

## Files

| File | Role |
|------|------|
| `src/app.c`   | the demo: the RTOS worker thread, the `k_msgq` bridge, and the personality launch |
| `prj.conf`    | `CONFIG_USERSPACE` + console/printk (mps2/an521) |
| `ove_config.h`| selects the engine-agnostic layers (arena + loader + Linux core) |
| `CMakeLists.txt` | links the seam + core and adds the `--wrap` |
| `run.sh`      | build for an521 + run on QEMU |

The Linux rootfs is the same embedded Buildroot CPIO the personality test uses
(`tests/ontarget/loader_rootfs_image.h`).
