# Capabilities & limitations

What the Linux personality can run today, and the simplifications to be aware of.
All items below are verified on QEMU **and** real STM32F746, across FreeRTOS,
NuttX, and Zephyr unless noted.

## Works

| Area | Detail |
|------|--------|
| **Boot** | BusyBox `init` → sysinit → `rcS` → real `/sbin/getty` → `overtos login:` → `root` → `/root #` |
| **Coreutils** | `ls -l`, `cat`, `cp`/`mv`/`rm`/`mkdir`, `grep`/`sed`/`awk`/`find`, `date`, `uname` |
| **Process view** | `ps` and interactive `top` — real per-process CPU%, plus the RTOS kernel threads shown bracketed (e.g. `[hpwork]`) |
| **Editors** | `vi` (edit + `:wq`), `less` (paginate, `q`), `more` |
| **Shell** | `hush` — builtins, pipelines (co-run with backpressure), background jobs (`sleep & ; yes & ; top`), `poweroff`/`reboot` |
| **Signals** | `Ctrl-C`; cross-process `kill(pid, sig)` — delivered at the target's next syscall boundary, or by the coordinator if it is parked |
| **Threads** | real pthreads (LinuxThreads: `clone(CLONE_VM)` + signal-based suspend/restart) |
| **/proc, /dev** | `/proc/{stat,uptime,meminfo,cpuinfo,…}`, `/dev/{null,console,tty}` synthesised |
| **Networking** | socket syscalls bridge to the selected RTOS network stack; BusyBox networking, curl, dropbear/SSH, and optional 9P netfs are supported |
| **Isolation** | programs run **unprivileged** in per-program MPU regions; a stray access is contained — the process is killed (like SIGSEGV, exit 139) and the shell survives |
| **Hardening** | the syscall boundary validates user pointers (`access_ok`-style); fault classes (UsageFault/BusFault/MemManage) are contained |

## Limitations

| Simplification | Consequence |
|----------------|-------------|
| **No MMU** | NOMMU FDPIC only (bFLT retired); `vfork` has no copy-on-write; shared-library text is XIP from the rootfs backing store |
| **Fixed MPU region pool** | concurrency is bounded by compile-time `NREG` (current QEMU defaults: Zephyr/an521 = 8, FreeRTOS/an500 = 5, NuttX/an500 = 6); `/proc/lxp_resources` reports live capacity |
| **Read-only rootfs** | the CPIO rootfs is RO; `/tmp` is writable but volatile, while persistence requires an engine/device-backed filesystem or netfs |
| **Ring-buffer pipes** | pipes are small (< 4 KiB) blocking ring buffers with backpressure — not POSIX-atomic for large writes |
| **Signals are not preemptive** | delivered at syscall boundaries (or by the coordinator when the target is parked), not asynchronously mid-computation |
| **Cosmetic noise** | a few unimplemented syscalls return `ENOSYS`; harmless |

!!! note "Scope"
    This is a compatibility *personality*, not a full Linux kernel — it
    implements the syscalls the target userspace actually uses. It is enough to
    boot a real Buildroot uClinux to an interactive shell with editors, job
    control, and threads, isolated per-process. See the
    [Benchmarks](benchmarks.md) for what that isolation and the syscall boundary
    cost in cycles.
