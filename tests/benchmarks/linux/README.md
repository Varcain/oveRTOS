# Linux-personality vs native-thread benchmark

Quantifies the **personality tax** — the cost of running work as an unprivileged
FDPIC Linux process (SVC syscall boundary + MPU + run-loop coordinator) versus
the SAME work as a native privileged `ove_thread`.

Two firmware images, one clock. On the **STM32F746 @216 MHz** both sides time off
`DWT->CYCCNT` (the native harness via `bench_cyccnt`, the personality via
`clock_gettime`→`ove_time_get_ns`, which reads the same register), so the ns
figures are directly comparable and `--mhz 216` converts them to cycles.

## Pieces
- **native side** — `tests/benchmarks/c` app (suite `compute` added here:
  `compute_mix` = the shared kernel, `null_call` = the syscall-boundary floor).
  Reuses the existing `thread`/`stream`/`queue` suites for B5/B6/B7.
- **linux side** — `board/overtos/progs/lbench.c` in buildroot → `/usr/bin/lbench`
  in the rootfs. Emits the same `###BENCH_JSON` envelope.
- **compare** — `lbench_compare.py --native <cap> --linux <cap>` → the tax table.

The B1 compute kernel is byte-identical in both (`bench_kernel.h` ⇄ the copy in
`lbench.c`), cross-checked by `BENCH_KERNEL_CHECKSUM` (0x855ee3aa) which both
print — a mismatch means the copies drifted and the B1 comparison is invalid.

## Fairness: caches ON
The personality firmware runs with caches on (normal operation), so build the
**native** bench with caches on too — override the app default:

    CONFIG_OVE_BENCHMARK_WORST_CASE_TIMING=n   # (app.yaml defaults y for the cacheless publish)

(A second, caches-off pass on both sides gives the worst-case bound.)

## Run (per engine: freertos / nuttx / zephyr)

### 1. native numbers
    ove defconfig-fragments stm32f746.<engine>.benchmark_heap
    # override caches-off -> on for the personality comparison:
    ove menuconfig   # OVE_BENCHMARK_WORST_CASE_TIMING = n   (or edit .config)
    ove configure && ove build && ove flash
    # capture the USART1 console (/dev/ttyACM0 @115200) to native-<engine>.txt

### 2. personality numbers
    # rebuild the rootfs so /usr/bin/lbench is current:
    ( cd ../../../.. ; make -C <buildroot> O=output-fdpic )   # repacks rootfs.cpio
    ove defconfig-fragments stm32f746.<engine>.linux_interop
    ove configure && ove build && ove flash
    # at the login: root -> run `lbench` -> capture to linux-<engine>.txt

### 3. the tax table
    python3 lbench_compare.py --native native-<engine>.txt --linux linux-<engine>.txt --mhz 216

## The axes (native case ⇄ lbench case)
| axis | isolates | native | lbench |
|---|---|---|---|
| B1 compute | userspace / FDPIC-PIC (control, ≈1×) | compute/compute_mix | compute_mix |
| B2 syscall | the SVC boundary (headline) | compute/null_call | null_syscall (raw, uncached) |
| B3 write | a real syscall | (stream) | write_devnull |
| B5 ctx-switch | coordinator round-trip | thread/context_switch | ctx_switch (pipe ping-pong) |
| B6 IPC 4KiB | pipe data path | (stream/queue) | pipe_wr_4k |
| B7 spawn | process load+MPU vs thread create | thread/create_destroy | spawn_vfork_exec |

## Notes / caveats
- **QEMU is not cycle-accurate** and does not tick `DWT->CYCCNT` for the native
  build (native rows read 0). Use QEMU only to validate the harness runs and
  emits valid JSON; take numbers on hardware.
- B7 uses `vfork`+`execve` (NOMMU has no `fork`); a *tight* vfork+exec loop can
  fault children on NOMMU-FDPIC, so B7 fully reaps each child and uses few
  windows. If it still faults on an engine, drop its window count.
- The personality pipe ring is < 4 KiB, so B6 loops over partial writes to push
  a full 4 KiB (drain-limited); report as MB/s = 4096 / (ns·1e-9).
