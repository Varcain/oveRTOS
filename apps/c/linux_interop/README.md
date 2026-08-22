# C example: RTOS kernel ↔ Linux-personality interop

One firmware image, two worlds running side by side — a **native RTOS thread**
(`ove_thread`) and a rootfs-owned **Linux guest demo** launched through the
oveRTOS **Linux personality** (`lxp_run`) out of a real Buildroot rootfs —
exchanging data **in both directions**, then handing you an interactive shell.

This is a first-class oveRTOS framework app, built by the `ove` build system, and
the *same demo* runs on **all three RTOS engines** through the engine-agnostic
personality core:

| Engine | Board | CPU | Program isolation |
|--------|-------|-----|-------------------|
| **Zephyr**   | `qemu-mps2-an521` | Cortex-M33 | unprivileged + MPU (`CONFIG_USERSPACE`) |
| **FreeRTOS** | `qemu-mps2-an500` | Cortex-M7  | unprivileged + MPU (`ARM_CM4_MPU`) |
| **NuttX**    | `qemu-mps2-an500` | Cortex-M7  | unprivileged + MPU (`CONTROL.nPRIV` + `PRIVDEFENA`, programmed by LXP's port in `BUILD_FLAT`) |

## Phase 1 — bidirectional round trip

The RTOS thread feeds three "sensor readings" **into** the Linux program's stdin
and drains what it echoes back **out** of stdout. Fixed, pre-staged arrays keep
the demonstration allocation-free and make EOF deterministic:

```
  RTOS feeder ─► fixed feed array ─► read cb ─►┌─────────────────┐
                                               │ guest roundtrip │
  RTOS consumer ◄─ fixed result array ◄─ write cb ◄────────────┘
```

## Phase 2 — interactive shell

The program then drops into an interactive BusyBox `sh`. The oveRTOS console
provider binds the launch's read, write, and non-consuming readiness callbacks,
so **you can type commands** — `ls /`, `echo hi`, `cat /etc/hostname`, `pwd`,
`echo x > /tmp/f`, … — and `exit` to finish. STM32 FreeRTOS and Zephyr publish
UART RX readiness as a run-scoped coordinator event; NuttX and QEMU retain the
bounded polling fallback because their current console transports expose no
native readiness subscription.

## Writable storage

The STM32F746G-DISCO Full profile exposes two writable trees while retaining the
CPIO system image as a deterministic, read-only root:

| Path | Backing | Lifetime | Semantics |
|------|---------|----------|-----------|
| `/tmp` | bounded LXP allocator pool in SDRAM | cleared by reset | 256 KiB pool shared by all temporary files; writes fail once its usable capacity is exhausted |
| `/data` | engine-native microSD/FAT through `ove_fs` | persistent | files, directories, seek, truncate, rename, unlink, and directory iteration; no symlinks or Unix ownership/mode persistence |

`CONFIG_OVE_LINUX_FS` enables the `/data` provider and selects
`CONFIG_OVE_FS`. FreeRTOS uses FatFs, NuttX its native VFS/VFAT mount, and
Zephyr its FAT filesystem API. The personality-facing provider is common: it
serializes native volume calls on one bounded host worker, so concurrent guests
cannot race a non-reentrant backend. Operations are synchronous and can block
the calling guest for the duration of SD I/O, but the guest's RTOS task is
parked rather than consuming CPU or being destroyed and recreated. The worker
runs above best-effort guests and below host real-time work. It admits at most
one bounded request per `CONFIG_OVE_LINUX_FS_SERVER_PERIOD_US` (1 ms by
default); one request transfers at most the personality's 4 KiB syscall
quantum. Because an opaque engine filesystem call cannot be preempted at an
arbitrary wall-time boundary, `CONFIG_OVE_LINUX_FS_SERVER_BUDGET_US` is an
overrun threshold, not a hard abort deadline.

Selecting `CONFIG_OVE_LINUX_FS` defaults `CONFIG_OVE_FS_MAX_OPEN_FILES` to 16,
matching LXP's external descriptor table. This generic OVE capacity sizes any
engine resource required per caller-owned native file handle. A private
compile-time check at the adapter boundary rejects either capacity drifting
below the personality table. The MPS QEMU boards have no native storage
medium, so their profiles resolve persistent FS and block support off while
retaining the SDRAM-backed `/tmp` tree.

The coordinator gives completed filesystem, socket, console, and ordinary
guest wakeups weighted 4:3:2:1 service opportunities, rotates among processes
within each class, and promotes a class after 20 ms of waiting. This prevents a
network-heavy guest from consuming every coordinator opportunity without
letting storage service preempt the RT scope task. `/proc/lxp_fs` reports queue
depth/high-water, request and byte counts, service time, cancellations,
failures, and budget overruns.

A missing or unformatted card does not delay Linux-personality startup. The
default virtual `/data` mount point remains visible, while operations below it
return `ENODEV` until a later mount retry succeeds. `/data` is a subtree, not
an overlay: `/bin`, `/etc`, shared libraries, and the rest of `/` continue to
come from the read-only CPIO image. The single external volume can instead be
mounted at any existing directory (for example `/mnt`); doing so moves the
provider's routing boundary rather than creating a second mount.

## Raw microSD administration

`CONFIG_OVE_LINUX_BLOCK` exposes the same microSD medium as a Linux block node,
`/dev/mmcblk0`. A bounded MBR parser creates `/dev/mmcblk0p1` through `p4` for
valid primary partitions, and `/proc/partitions` reports the discovered views.
The class driver implements 64-bit byte offsets plus `BLKSSZGET`,
`BLKGETSIZE`, `BLKGETSIZE64`, `HDIO_GETGEO`, `BLKRRPART`, and durable sync, so
the BusyBox `fdisk` and `mkfs.vfat` applets can use their normal block-device
paths even on cards larger than 2 GiB.

Raw media and the external FAT mount share the same serialized storage worker
and the engine-neutral media lease in `include/ove/media.h`. The lease is below
LXP: native RTOS callers of `ove_fs` and `ove_block` participate in the same
policy. Read-only inspection may coexist with the mounted filesystem, but a
writable raw open fails with `EBUSY` until the filesystem is unmounted.
Conversely, mounting fails while a raw writer is open. Raw handles are qualified
by the card generation; removal or replacement makes an old handle unusable
instead of letting it access the replacement medium. The final writable close
flushes the card. Guest buffers never reach an SD DMA driver directly: the
adapter moves at most 4 KiB through its aligned native staging area and performs
bounded sector-level read/modify/write for unaligned byte requests.

The conventional administration sequence is:

```sh
fdisk -l /dev/mmcblk0
cat /proc/partitions
umount /data
fdisk /dev/mmcblk0
mkfs.vfat /dev/mmcblk0p1
mount -t vfat /dev/mmcblk0p1 /data
```

Formatting is destructive. The initial partition model deliberately supports
only DOS/MBR's four primary entries: GPT, extended/logical partitions, and
automatic raw-write unmounting are not implemented. All three STM32 engines
can mount a superfloppy or any one of the four validated primary partition
views. NuttX uses `register_blockpartition()`; FreeRTOS and Zephyr use their
native FatFs `VolToPart[]` multi-partition facilities. Writable raw access has
a separate `CONFIG_OVE_LINUX_BLOCK_WRITE` gate and is off by default outside
this administration-focused demo.

### Mount semantics and deliberate limits

The guest accepts `vfat`, `msdos`, `fat`, or `auto` and rejects any other
filesystem type with `ENODEV`. It understands `ro`/`rw`, `remount`, `nosuid`,
`nodev`, `noexec`, `noatime`, `nodiratime`, and `relatime`; unknown options and
unsupported flag bits fail with `EOPNOTSUPP` rather than being silently
ignored. `ro` is enforced at the LXP VFS boundary for open, write, truncate,
mkdir, unlink, rmdir, and rename. Switching a live mount to read-only requires
all regular files to be closed, preventing a pre-existing writable native
handle from bypassing the new policy. FAT guest execution is intrinsically
disabled, and Unix device/suid semantics are not projected onto the mount.

`/proc/mounts` reports the live source, target, type, read-only state, and
intrinsic `nosuid,nodev,noexec` policy. `/proc/filesystems` lists `vfat`.
`statfs64()` and `fstatfs64()` obtain the mounted FAT volume's live allocation
geometry from the engine-native filesystem, so `df` and `df /data` report real
card capacity, used space, and available space. FAT has no fixed inode table,
so inode totals remain zero. Objects under the external mount have a distinct
device identity, allowing path-based tools to associate them with the correct
`/proc/mounts` entry. `sync`, `syncfs`, `fsync`, and `fdatasync` flush open
provider-backed files through the serialized storage worker.
Only one SD medium and one external mount are supported; a second mount is not
an alias and returns busy. The following remain explicit non-features:

- FAT cannot persist Unix ownership/modes or provide symlinks and hard links.
  Emulating those semantics would require a metadata database or an overlay,
  which would no longer be a directly PC-readable FAT tree.
- Bind mounts, overlays, and move mounts require a general mount graph and
  path-resolution layers that LXP intentionally does not have. Forced or lazy
  unmount would also violate the bounded native-handle ownership contract, so
  only a clean unmount with no open handles is accepted.
- GPT requires 64-bit GPT header/table validation and CRC processing, while
  extended MBR requires walking an unbounded-on-media EBR chain. Neither fits
  the current one-sector, bounded `BLKRRPART` state machine. Primary MBR is kept
  because it has a fixed four-entry validation cost and is supported by every
  native engine path.
- The public block contract is a singleton rather than a handle-based device
  enumerator. Adding a second block device therefore needs an ABI extension;
  pretending all media are the SD singleton would weaken ownership.
- Discard/TRIM, secure erase, sysfs, udev, and Linux hotplug broadcasts are not
  portable across the three selected RTOS storage drivers. Presence and media
  generation are available through `ove_block_get_info()` instead.
- Raw readers can inspect a mounted medium, but that is not a coherent live
  filesystem image while FAT metadata is changing. Raw writers and a mounted
  filesystem are always mutually exclusive.
- Formatting ext2 would not make it mountable: only the engine-native FAT
  implementations are shared by all three targets. A generic FatFs-over-
  `ove_block` adapter remains an optional fallback for a future engine without
  native FAT, and is deliberately disabled here to avoid duplicating the
  existing native integration.
- The interface targets bounded BusyBox media tools, not general util-linux or
  sysfs compatibility. The rootfs enables BusyBox `fdisk`, `mkfs.vfat`, and
  `dd`; ordinary programs can also use `read`, `write`, `pread`, and `pwrite`
  on the block node.

The FreeRTOS backend still uses STM32Cube's old FatFs R0.11 integration. Its
multi-partition support is used rather than replaced, but upgrading that vendor
dependency should be treated as a separate compatibility project with on-card
and power-loss tests, not mixed into the media-ownership change.

### fstab and FAT checking

The oveRTOS Buildroot image supplies `/etc/fstab` entries for `/proc` and the
superfloppy `/dev/mmcblk0` mounted at `/data`. PID 1 runs `mount -a` before
`rcS`, and BusyBox mount accepts the same entry for explicit operations such as
`mount /data`. A missing card makes the `/data` entry fail without stopping the
remaining init actions. The fstab check-pass field is zero deliberately: FAT
checking must not run implicitly against mounted or unexpectedly large media.

The image also provides BusyBox `fsck` and dosfstools `fsck.fat`. Safe media
administration is explicit:

```sh
umount /data
fsck.fat -n /dev/mmcblk0       # full read-only check
fsck.fat -a /dev/mmcblk0       # repair without questions
mount /data
```

As with other `fsck` implementations, exit status 1 means that errors were
found and corrected; a follow-up `-n` pass should return zero. Scripts must not
treat every nonzero status from an automatic repair as an unqualified failure.

`fsck.fat -b /dev/mmcblk0` validates only the FAT boot sector and uses bounded
memory, so it is useful even while diagnosing a large card. It is not a full
FAT or directory-tree check. Upstream dosfstools loads the complete FAT plus
one owner pointer per cluster for a full pass. That works for FAT volumes whose
geometry fits the selected LXP process arena, but it is not bounded by this
port and can return `ENOMEM` on a high-cluster-count card. For example, the
16 GB validation card has 487,619 clusters and needs about 3.9 MiB for those
two tables alone, while the STM32F746 FreeRTOS profile provides about 640 KiB
per guest. A future full-card embedded checker must page FAT entries and use a
compact ownership bitmap; pretending boot-only checking is equivalent, or
silently increasing every process region, would weaken either correctness or
guest concurrency.

## Guest scheduling and niceness

All Linux guests run in one best-effort native RTOS priority class below the
coordinator, filesystem worker, and host real-time work. `nice`, `renice`,
`getpriority`, and `setpriority` alter only the CPU share among runnable guests;
they never map to a native RTOS priority. Nice values survive `fork`/`clone` and
`exec`, and `/proc/<pid>/stat` and `/proc/<pid>/status` report them.

The embedded policy maps nice -20..19 linearly to bounded weights 40..1, with
nice 0 at weight 20. `CONFIG_OVE_LINUX_GUEST_QUANTUM_MS` is the nice-0 base
quantum. FreeRTOS rotates guest tasks from its seam-owned tick path. Zephyr's
timer callback only accounts the quantum and wakes a small native seam thread
to perform the guest ready-queue rotation in thread context; global
`CONFIG_TIMESLICING` remains off. NuttX scales the guests' existing
equal-priority `SCHED_RR` slices, with ratios rounded to the native kernel tick.
In every engine, higher-priority host work remains immediately preemptive.

After flashing any of the three STM32 Full-profile images, run the destructive,
namespace-confined hardware regression (it only uses
`/tmp/.ove-storage-probe` and `/data/.ove-storage-probe`):

```sh
.venv/bin/python tests/sim/freertos-linux/storage_drive.py
```

It writes a 200 KiB temporary file, exercises persistent create/read/rename and
directory operations, resets the MCU through ST-Link, and verifies that the
microSD file survived.

The corresponding guest-share regression runs two CPU-bound Lua guests at
nice -20 and nice 19, verifies their `/proc` values and forward progress, and
checks that the favoured guest receives a weighted CPU share despite the
1 kHz higher-priority RT-scope task:

```sh
.venv/bin/python tests/sim/freertos-linux/nice_drive.py
```

FreeRTOS's LXP port enforces that share with a single-runnable guest gate. Its
tick callback only accounts the current guest's weighted quantum; a small
run-scoped privileged selector then suspends that task and resumes the next
guest in thread context. This avoids FreeRTOS's otherwise independent
equal-priority ready-list rotation whenever higher-priority host work wakes.
Every Linux task remains below the coordinator and host RT classes, and global
`configUSE_TIME_SLICING` remains disabled in the final Full configuration.

Canonical LXP also carries a hardware-independent version of this regression in
its standalone FreeRTOS QEMU harness. Milestone M9 runs identical `CLONE_VM`
workers at nice -20 and nice 19 while a higher-priority native task wakes at
1 kHz, then asserts forward progress, a bounded weighted ratio, and the native
wakeup count:

```sh
cd modules/lxp/ports/qemu-mps2
M=9 bash run.sh
```

That stress measured a 33-word peak for the run-scoped selector stack. The
production port retains 128 words (512 bytes), with FreeRTOS stack-overflow
checking still enabled.

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
[rt-scope] oldest-release-us window=364.80 max-consecutive-missed=0 | total=364.80 max-consecutive-missed=0 irq-entry=2.11
[rt-scope] work-us min=5.15 max=5.37 late-finish=0
[rt-scope] svc-us window calls=11430 min=10.87 avg=12.20 max=13.47 syscall=413(pselect6_time64)
[rt-scope] svc-total calls=102176 avg-us=12.20 max-us=13.69 syscall=403(clock_gettime64)
```

`missed` counts timer releases for which no distinct response execution began.
`irq-overrun` is the subset recovered after multiple hardware releases collapsed
into one pending TIM3 interrupt, and `late-finish` counts responses that crossed
the following 1 ms release. `pending` is the number of releases not yet
acknowledged by the response task. One pending release can still become the next
execution; when the backlog exceeds one, the older releases are already
unserviceable and the lifetime `missed`, `oldest-release`, and
`max-consecutive-missed` snapshot fields include them immediately. They no
longer remain falsely at zero while a response task is stalled. The software
report adds a few register accesses to the measured path, so keep the GPIO
capture as the independent physical cross-check.

`dispatch-us` retains the phase of the newest timer release, which is directly
comparable with the CH1-to-CH2 delay while no release is missed. Because that
phase wraps every 1 ms, `oldest-release-us` separately adds all collapsed or
unserved periods and is the true worst response age when misses occur.
`max-consecutive-missed` reports the longest such run. `irq-entry` measures the
oldest release's age when TIM3's ISR finally began; compare it with the total
oldest-release age to separate interrupt masking from post-ISR scheduling delay.
`irq-signal` is sampled after the ISR publishes the response event. A large
entry-to-signal delta identifies time inside the interrupt/event-post path; a
low signal age paired with a high dispatch age identifies scheduler latency
after the wakeup was made runnable.

NuttX additionally reports `scheduler_lock_probe_available 1`. Its
`irq_preempt_locked_samples` counter records releases that interrupted a task
while its scheduler lock was held. `preempt_locked_dispatch_max_ns` and its
owner PID retain the worst corresponding dispatch, while
`preempt_unlocked_dispatch_max_ns` classifies the remaining releases. This
distinguishes NuttX's documented deferred-preemption path from interrupt
tail-chaining without enabling the substantially heavier critical-monitor note
instrumentation.

On Zephyr, TIM3 runs at ordinary IRQ priority 0 (the highest kernel-callable
level). Ethernet runs at 2; LTDC, QSPI, USART1, EXTI, and DMA2 run at 3. The
scope ISR can therefore post its event ahead of the active display/network
peripherals without using Zephyr's zero-latency class, whose handlers could not
call the event API.

On all three engines, `svc-us` measures wall-clock cycles from entry into the C
portion of the Linux guest's SVC handler through syscall dispatch/parking and
register write-back. The small assembly entry/exit shim and the statistics
update itself are outside the interval. `syscall` is the ARM EABI syscall number
carried in `r7` (all guest calls use the same `svc #0` instruction); the
parenthesised name is `?` for a number outside LXP's compact diagnostic table.
Canonical LXP owns both the accumulator and that table; the oveRTOS facade adds
only the selected engine's counter frequency and optional native diagnostics.
The window row is reset every 10 seconds, while `svc-total` retains the lifetime
maximum and the syscall that produced it.

The guest can read the same experiment without scraping the native UART:

```sh
cat /proc/rt_scope
```

`/proc/rt_scope` is a coherent, non-destructive lifetime snapshot. It reports
release/execution/failure counts, newest- and oldest-release dispatch maxima,
the longest missed run, ISR-entry and fixed-work timings in integer nanoseconds,
histogram-derived p99/p99.9 ceilings, and the lifetime SVC timing on FreeRTOS,
NuttX, and Zephyr. `timer_hz` and `svc_counter_hz` document the conversion bases.
Opening the file neither rotates the 10-second UART window nor resets any
counter, so benchmark readers cannot perturb or consume the measurement.

Zephyr also prints `irq-lock-us`: the count, average, and maximum duration of
the coordinator's IRQ-masked process-table snapshots for both the current
window and the whole run. Its measurement update happens after IRQs are
restored, so the instrumentation does not lengthen the reported interval.

Contained Zephyr guest faults do not write the normal multi-line register dump
from exception context. The seam preserves CFSR/HFSR, fault-address registers,
PC, and the number of suppressed dump lines in `g_lxp_zephyr_fault_diag`, then
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

## Host and network ownership

The app profile declares product topology—the rootfs placement,
`172.1.1.2/24`, gateway `172.1.1.1`, and the 9P endpoint—as
`CONFIG_OVE_LINUX_*` settings. `ove_lxp_host_init()` consumes that build
configuration; application C code does not reconstruct native handles or
provider composition. `ove_lxp_host_init_cpio()` remains available to products
that genuinely select their rootfs or topology at runtime.

`ove_lxp_host_t` owns the fixed CPIO file/path workspace, native interface
storage, bring-up, bounded address wait, rollback, teardown, provider selection,
and immutable LXP host. Workspace capacities come from
`CONFIG_OVE_LINUX_ROOTFS_FILE_CAPACITY` and
`CONFIG_OVE_LINUX_ROOTFS_NAME_CAPACITY`; the latter retains the smaller proven
STM32 NuttX bound; the general 15 KiB pathname bound retains more than 4 KiB
above the measured rootfs. Init and deinit reset only live handles rather than
clearing the potentially large workspace. LXP binds the opaque eth0 handle and
copied netfs configuration only for each run and clears both at teardown.
Consequently, both guest-mode launches reuse deliberate host configuration
without mutable process-global setters or stale state from the preceding run.

The application intentionally keeps the four lifecycle operations visible:
initialize one host, run sequential guests, observe the quiescent host, then
deinitialize it. A second session facade would duplicate `ove_lxp_host_t`, hide
that both launches reuse one parsed rootfs, and make partial-init cleanup less
obvious. `ove_lxp_host_deinit()` is therefore idempotent and safe after failed
initialization, while the application remains responsible for its final exit
policy.

## Guest entrypoint ownership

Both phases launch one stable rootfs contract,
`/usr/libexec/ove-interop-guest`, with a semantic mode:

| Mode | Rootfs-owned behavior |
|------|-----------------------|
| `roundtrip` | relay stdin to stdout for the native two-way demonstration |
| `boot` | replace PID 1 with the rootfs init implementation |
| `fpcheck` | run the hard-float guest-context qualification program |

The entrypoint is maintained in Buildroot at
`board/overtos/rootfs-overlay/usr/libexec/ove-interop-guest`. It owns the
BusyBox, init, and fpcheck executable choices; `src/app.c` owns only the mode
choice and launch callbacks. LXP applies the same bounded `#!` parsing to an
initial `lxp_run()` launch and a later `execve()`, including interpreter symlink
resolution, so the RTOS application does not need a shell-specific launch API.

Per-launch policy uses `ove_lxp_launch_config_t`, not canonical LXP's launch
structure. `ove_lxp_console_bind()` supplies interactive I/O while the separate
`ove_lxp_console_bind_diagnostics()` opt-in supplies bounded ENOSYS and abnormal
exit reports without overwriting application I/O callbacks. The console's
bounded `ove_lxp_console_printf()` removes private formatting helpers while
preserving the QEMU-specific personality console routing. The common binding
owns tty lookahead and run-scoped readiness; the board-selected console HAL
owns the ordinary native console or QEMU's dedicated CMSDK UART1. The host
facade translates guest termination records field by field; reason values are
mapped explicitly and the `comm` string remains callback-lifetime data by
contract.

The application retains its post-phase-1 socket smoke because that is a demo
workload and readiness report, not provider lifecycle. It queries the host-owned
address through `ove_lxp_host_netif_get_addr()` and releases the host after the
final guest exits.

Application-owned scenarios are separated from lifecycle orchestration without
promoting them into the generic API. `src/roundtrip.c` owns the fixed native
worker, guest I/O callbacks, and reply validation. `src/network_smoke.c` owns
address reporting and the bounded TCP readiness probe, and compiles to no-op
functions when networking is disabled. `src/app.c` therefore shows the public
oveRTOS composition directly: initialize the host, prepare per-launch policy,
run each guest, observe, and deinitialize.

## Observability ownership

The demo chooses the watchdog policy, measurement window, text format, and
output transport. It does not enumerate LXP's process-global diagnostic or
latency registries. `ove_lxp_run_health_snapshot()` supplies the minimal live
heartbeat needed by the watchdog, including before host initialization, while
`ove_lxp_host_observe()` takes one versioned copy only after the final run has
stopped. The latter returns `OVE_ERR_BUSY` if asked to cross an active run.

The copied record contains size accounting, automatic world-checkpoint health,
latency rows when the Diagnostic profile enables them, and an optional
host-neutral guest-stack high-water mark. FreeRTOS currently supplies that
stack metric through LXP's port vtable; NuttX and Zephyr leave it unavailable
instead of requiring engine conditionals in the app. Application-owned thread
stack and heap reporting remains local because those resources belong to this
demo rather than to LXP. Thread owners snapshot stack headroom before teardown
through the status-returning oveRTOS query. Unsupported data is printed as
`unavailable`—not as a fabricated fully consumed stack—and `src/app.c` never
reaches into another module's RTOS handles.

Generic `ove_thread_info` snapshots likewise carry only an opaque native
identity. The private LXP thread adapter resolves that identity to a guest slot;
the generic RTOS thread API does not store or expose personality ownership.
Optional stack, CPU, and per-state-time values carry granular validity bits;
the seam translates those bits explicitly and zeros unavailable values instead
of turning an unsupported metric into an apparent measurement.

Every public record is an OVE-owned, versioned type. The common backend takes
the canonical LXP snapshot into private temporary storage and copies fields
across the boundary; no public header includes or aliases an LXP observation
type. Latency storage has explicit OVE limits of 15 service classes and 16
guest wake rows. Private compile-time checks reject a canonical class, bucket,
slot, feature-gate, or ABI change that no longer fits those limits, while the
runtime translator rejects malformed versions and counts before exposing a
partial record. The temporary canonical record exists only while collecting a
quiescent post-run report, so this separation adds no fixed RAM or active-run
work.

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
selects the rootfs entrypoint's `fpcheck` mode after the normal interop phase:

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
`CONFIG_USERSPACE` knobs come from `config/templates/prj.conf.j2`; LXP's Zephyr
port (`modules/lxp/ports/zephyr/lxp_zephyr_port.c`) owns the native K_USER
thread, memory-domain and fatal-error hooks, so illegal guest accesses terminate
only the current Linux process.

**FreeRTOS — `qemu-mps2-an500` (Cortex-M7).** Reuses the *stock* an500 FreeRTOS
board (no dedicated board); when `CONFIG_OVE_LINUX` is set it (a) drops the sim
framework + dashboard/trace/profiler (the personality is headless), (b) drops the
`vPortSVCHandler → SVC_Handler` alias so LXP's FreeRTOS port
(`modules/lxp/ports/freertos/lxp_freertos_port.c`)
owns the `SVC_Handler` vector and forwards FreeRTOS's start-scheduler `svc` to
`vPortSVCHandler`, and (c) makes the run script inject the rootfs CPIO into PSRAM
and attach the interactive console. The `ARM_CM4_MPU` port creates each guest with
`xTaskCreateRestrictedStatic`; program, dynamic pool, and XIP rootfs windows are
explicit MPU regions, while the coordinator remains privileged.

**NuttX — `qemu-mps2-an500` (Cortex-M7).** Also reuses the *stock* an500 board.
NuttX is the hard engine: its own `svc #0` *is* the syscall/context-switch ABI, so
LXP's NuttX port (`modules/lxp/ports/nuttx/lxp_nuttx_port.c`) `irq_attach`es SVCall and identifies a
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
| `src/app.c` | compact native phase and Linux-host lifecycle orchestration |
| `src/roundtrip.c` | allocation-free RTOS-to-guest round-trip worker and fixed staging |
| `src/network_smoke.c` | configured-address reporting and bounded TCP readiness smoke |
| `src/qualification.c` | optional watchdog/probes plus latency and resource reporting; snapshots owned threads before teardown |
| `src/rt_scope.c` | engine-neutral RT-scope measurement, reporting, and `/proc/rt_scope` policy |
| `boards/stm32f746g-discovery/common/rt_scope.c` | board-owned timers, probe pins, IRQ attachment, worker-stack placement, and optional engine attribution |
| Buildroot `board/overtos/rootfs-overlay/usr/libexec/ove-interop-guest` | Linux-side roundtrip, boot, and fpcheck behavior |

The LXP host facade parses and publishes the selected CPIO once, retains the
provider composition, and supplies the immutable rootfs fields to each launch.
The application manifest supplies build-time topology while its C code supplies
only per-launch console, diagnostic, and display policy. The personality core
and selected port under `modules/lxp/ports/{freertos,nuttx,zephyr}/` are pulled
in by the generated `ove_config.cmake`; oveRTOS's corresponding
`*_lxp_host.c` file supplies board storage, priorities, memory policy, and
stable-HAL callbacks.
