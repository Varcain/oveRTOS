#!/usr/bin/env python3
# Coordinator/host latency for the FreeRTOS Linux personality.
#
#   latency_drive.py [log]        QEMU mps2-an500 via `ove run` — reports, does not gate timing
#   latency_drive.py --hw [log]   STM32F746G-DISCO over /dev/ttyACM0 — reports AND gates
#
# Requires a build with CONFIG_OVE_LINUX_LATENCY=y; without it the firmware prints no [lat] rows
# and this says so rather than passing on an empty log.
#
# What is gated, and what is deliberately not
# -------------------------------------------
# Only host-wake overshoot is gated, and only on hardware.
#
# Not coordinator service time: one dispatch of `fork` legitimately takes ~105 ms (it copies the
# parent's writable data through an uncached mapping). That is accepted and documented — it is a
# coordinator bound, not a host one. The run loop takes its single critical section only to claim
# an event and does the work outside it, so the host is not made to wait for that 105 ms. This
# gate is what keeps that property true.
#
# Not on QEMU: it is a functional emulator, not cycle-accurate, so wall-clock latency there is the
# developer's PC scheduling rather than the target's. Measured, the difference is not subtle —
# the >=64us tail is 4.2% and 7.0% on QEMU against 0.000% on hardware, and the max is 290-622us
# against 7-13us. Gating those numbers would test the wrong machine, so QEMU reports only.
#
# Thresholds, from 7 runs on STM32F746G-DISCO (216 MHz):
#   host-wake max = 7.2, 7.4, 11.4, 11.5, 13.1, 13.9 us, plus one outlier at 284 us (1 sample of
#   6319; unattributed — a masked ring_write in the stream backend is a candidate). The >=64us
#   tail was 0 in six of the seven runs and 1/6319 in the outlier.
#
#   MAX_NS 1 ms   - 3.5x the lone outlier, ~70x the typical worst, 10% of the monitor's 10 ms
#                   period. Catches millisecond-scale work appearing in a masked window without
#                   flaking on that outlier.
#   TAIL_FRAC 1%  - at most 1% of wakes in the >=64us bucket. The max alone would let a
#                   systematic slide from ~14us to ~60us pass; this catches the bulk moving
#                   while the peak still looks legal.
# Both sit above measured behaviour. Neither is a deadline anyone specified.
import argparse
import os
import re
import subprocess
import sys
import time

HOST_WAKE_MAX_NS = 1_000_000  # 1 ms
HOST_WAKE_TAIL_FRAC = 0.01  # <=1% of wakes may land in the >=64us bucket

PORT, BAUD = "/dev/ttyACM0", 115200

# (command, seconds to let it run). Two bare 'root' lines first: the account has a password
# (BR2_TARGET_GENERIC_ROOT_PASSWD) so login prompts for name and password; with only the first,
# the next command is eaten as the password.
#
# The load targets the paths most likely to hold the coordinator:
#   cat  - 200 KiB of read()/write(), ~50 back-to-back DEFER events each capped at
#          LXP_SYSCALL_QUANTUM_BYTES (4096)
#   find - a getdents/stat storm: many small DEFER events
#   fork - a fork+exec loop: program load + FDPIC relocation per iteration
# NOT covered: LXP_SYSCALL_FILE_QUANTUM_BYTES (64 KiB) bounds pread64/pwrite64 only, and nothing
# here issues them (busybox cp is read/write, so it rides the 4 KiB quantum).
STEPS = [
    ("root", 5),
    ("root", 5),
    ("cat /bin/busybox > /dev/null; echo CAT=$?", 9),
    ("find / > /dev/null; echo FIND=$?", 11),
    ("i=0; while [ $i -lt 30 ]; do /bin/true; i=$((i+1)); done; echo FORK=$?", 13),
    ("poweroff", 8),
]


def run_qemu(log):
    seq = "; ".join(rf"printf '{c}\n'; sleep {s}" for c, s in STEPS)
    cmd = f"( {seq} ) | QEMU_TIMEOUT=200 .venv/bin/ove run > {log} 2>&1"
    try:
        subprocess.run(["/bin/sh", "-c", cmd], timeout=280)
    except subprocess.TimeoutExpired:
        subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])
    return open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""


def run_hw(log):
    import serial  # only needed for --hw; keep QEMU runs free of the dependency

    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    buf = bytearray()

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            d = ser.read(4096)
            if d:
                buf.extend(d)

    # Reset rather than assume the board's state: the counters reset when the coordinator starts,
    # so a board left at a shell would report a window that does not match the load below.
    subprocess.run(["openocd", "-f", "board/stm32f7discovery.cfg", "-c", "init; reset run; exit"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    pump(22)  # boot: phase 1 round trip, then phase 2 init -> login prompt
    for cmd, settle in STEPS:
        ser.write((cmd + "\r").encode())
        ser.flush()
        pump(settle)
    text = buf.decode("latin-1", "replace")
    open(log, "w").write(text)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hw", action="store_true", help="drive real hardware and gate the timing")
    ap.add_argument("log", nargs="?", default="/tmp/latency_drive.log")
    args = ap.parse_args()

    text = run_hw(args.log) if args.hw else run_qemu(args.log)
    rows = [ln for ln in text.splitlines() if ln.startswith("[lat]")]
    for r in rows:
        print(r)

    if not rows:
        # Distinguish "the gate is off" from "the run died": both leave no rows, and naming the
        # wrong one sends the reader to the wrong place.
        print(f"RESULT: NO DATA — no [lat] rows. Build with CONFIG_OVE_LINUX_LATENCY=y; see {args.log}")
        return 1

    load_ok = all(m in text for m in ("CAT=0", "FIND=0", "FORK=0"))
    no_kfault = not re.search(r"HardFault|up_assert|PANIC|panic|Assertion|MPU FAULT|FATAL", text)
    # Rows with no samples are skipped, so a report cut short looks exactly like a quiet one.
    complete = "[lat] end" in text
    print(f"\nload_completed={load_ok} report_complete={complete} no_kernel_fault={no_kfault}")

    # A run whose load died early describes an idle coordinator and understates every maximum, so
    # gating on it would pass by measuring nothing. Suspect is a failure, not a pass with a note.
    if not (load_ok and no_kfault and complete):
        print("RESULT: SUSPECT — load or report incomplete; maxima understate, so the gate is void")
        return 1

    host = next((ln for ln in rows if "host-wake-overshoot" in ln), None)
    m = re.search(r"n=(\d+) max_ns=(\d+) us\[([\d ]+)\]", host) if host else None
    if not m:
        print("RESULT: NO DATA — the host-wake row is missing or unparsable; the monitor did not run")
        return 1
    n, max_ns = int(m.group(1)), int(m.group(2))
    tail = [int(x) for x in m.group(3).split()][-1]  # the >=64us bucket

    if not args.hw:
        print(f"host-wake: n={n} max={max_ns/1000:.1f}us tail>=64us={tail}/{n}={tail/n:.3%}")
        print("RESULT: MEASURED — timing not gated on QEMU (emulated clock); use --hw to gate")
        return 0

    max_ok = max_ns <= HOST_WAKE_MAX_NS
    tail_ok = n > 0 and (tail / n) <= HOST_WAKE_TAIL_FRAC
    print(f"host-wake: n={n} max={max_ns/1000:.1f}us (limit {HOST_WAKE_MAX_NS/1000:.0f}us) -> "
          f"{'ok' if max_ok else 'FAIL'}; tail>=64us={tail}/{n}={tail/n:.3%} "
          f"(limit {HOST_WAKE_TAIL_FRAC:.0%}) -> {'ok' if tail_ok else 'FAIL'}")
    print("RESULT:", "PASS" if (max_ok and tail_ok) else "FAIL — host real-time regressed")
    return 0 if (max_ok and tail_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
