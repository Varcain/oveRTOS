#!/usr/bin/env python3
# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Guest-fault containment matrix for the FreeRTOS-MPU Linux personality.
#
# segv_drive.py proves ONE fault class (a kernel-SRAM write) is contained. This
# runs the whole set in a single boot and asserts the same invariants for each,
# so a change to MPU attributes, exception frames, cache maintenance or handler
# ordering cannot weaken one class while the others still pass:
#
#   - the offending guest dies with 139 (128 + SIGSEGV)
#   - the coordinator reports it: reason=memory-fault, signal=11, comm=<prog>
#   - the kernel never faults (no HardFault / Default_Handler / panic)
#   - the shell survives every case and still runs the next one
#
# and, for the concurrency case, that sibling guests keep running while another
# one faults — a fault must kill its own slot, not the personality.
#
# Not covered, deliberately: divide-by-zero and unaligned access need SCB->CCR
# traps this target leaves off (only bit 18, branch prediction, is set), so
# neither fault class exists to contain. See board/overtos/progs/mpufault.c.
#
# Keystrokes are fed by a plain shell pipeline, NOT a Python pty/reader: QEMU's
# mps2-an500 ARM-MPU emulation is timing-fragile and any host-side reader
# perturbs it enough to corrupt an unrelated guest (see segv_drive.py). Output
# is parsed afterwards.
#
# Requires the active workspace to be the an500 FreeRTOS linux_interop build.
#
# Usage: faultmatrix_drive.py [logfile]
import os
import re
import subprocess
import sys

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/faultmatrix_drive.log"

# (tag, shell command, program name the coordinator should name, marker the
#  fixture prints before faulting). The marker matters: without it a case that
#  never ran would look identical to a case that was contained.
CASES = [
    ("segv",   "segv",             "segv",     "[segv] write kernel SRAM"),
    ("kread",  "mpufault kread",   "mpufault", "[mpufault] read kernel SRAM"),
    ("periph", "mpufault periph",  "mpufault", "[mpufault] read SCB CPUID"),
    ("nxexec", "mpufault nxexec",  "mpufault", "[mpufault] execute from data region"),
    ("xregion", "xregion",         "xregion",  "[xregion] write sibling pool region"),
    ("udf",    "kstress udf",      "kstress",  "[kstress] udf"),
]

# fpbadsp needs VFP: on a soft-float guest it exits 77 rather than faulting, so
# it is a skip there and a full case on a hard-float rootfs.
FP_CASE = ("fpbadsp", "fpbadsp", "fpbadsp", "[fpbadsp]")

seq = [r"printf 'root\n'; sleep 5", r"printf 'root\n'; sleep 5"]
for tag, cmd, _prog, _marker in CASES:
    seq.append(rf"printf '{cmd}; echo RC_{tag}=$?\n'; sleep 5")
seq.append(rf"printf '{FP_CASE[1]}; echo RC_{FP_CASE[0]}=$?\n'; sleep 5")

# Concurrency: sibling guests occupy slots while another one faults. Only the
# faulter may die, and the slot it released must be immediately reusable — so a
# fresh exec has to succeed straight afterwards, with the siblings still parked.
# `mpufault` with no valid mode is a real exec that returns 2 without faulting,
# which distinguishes "a new guest ran" from "the shell printed something".
#
# ONE sleeper. Concurrent-guest budgets differ per target — the STM32 (8 MB
# SDRAM pools) holds ~5 beside init/getty/inetd, the an500 (-m 16) far fewer —
# and this must leave room for the faulter AND the reuse probe on top of the
# sibling. Asking for more turns a containment test into a resource test: the
# faulter never starts, and "no fault was contained" looks the same as "the
# fault escaped". One sibling is enough to prove a fault does not take its
# neighbours with it.
seq.append(r"printf 'sleep 30 &\n'; sleep 2")
seq.append(r"printf 'segv; echo RC_conc=$?\n'; sleep 5")
seq.append(r"printf 'mpufault bogus; echo RC_reuse=$?\n'; sleep 5")
seq.append(r"printf 'uname -a\n'; sleep 3")
seq.append(r"printf 'poweroff\n'; sleep 4")

cmd = f"( {'; '.join(seq)} ) | QEMU_TIMEOUT=160 .venv/bin/ove run > {log} 2>&1"
try:
    subprocess.run(["/bin/sh", "-c", cmd], timeout=240)
except subprocess.TimeoutExpired:
    subprocess.run(["pkill", "-9", "-f", "qemu-system-arm"])

text = open(log, "rb").read().decode("latin-1", "replace") if os.path.exists(log) else ""

failures = []


def rc_of(tag):
    m = re.search(rf"^RC_{tag}=(\d+)", text, re.M)
    return int(m.group(1)) if m else None


def check(tag, prog, marker):
    """One case: it ran, it was contained, and the coordinator attributed it."""
    if marker not in text:
        failures.append(f"{tag}: never ran (no {marker!r} in the log)")
        return
    rc = rc_of(tag)
    if rc != 139:
        failures.append(f"{tag}: exit {rc}, want 139")
    exit_line = re.search(rf"\[lxp\] guest-exit [^\n]*comm={re.escape(prog)}[^\n]*", text)
    if not exit_line:
        failures.append(f"{tag}: no coordinator guest-exit naming comm={prog}")
        return
    line = exit_line.group(0)
    for want in ("reason=memory-fault", "signal=11", "status=139"):
        if want not in line:
            failures.append(f"{tag}: guest-exit missing {want}: {line}")


for tag, _cmd, prog, marker in CASES:
    check(tag, prog, marker)

# fpbadsp: 139 on a hard-float guest, 77 (its own skip) on soft.
fp_tag, _fp_cmd, fp_prog, fp_marker = FP_CASE
fp_rc = rc_of(fp_tag)
if fp_rc == 77:
    print("fpbadsp: SKIP (soft-float guest — no VFP state to mis-stack)")
elif fp_rc == 139:
    check(fp_tag, fp_prog, fp_marker)
else:
    failures.append(f"fpbadsp: exit {fp_rc}, want 139 (hard-float) or 77 (soft-float skip)")

# Concurrency: the faulter died, and its slot came back for the next guest.
if rc_of("conc") != 139:
    failures.append(f"concurrent: faulting guest exit {rc_of('conc')}, want 139")
if rc_of("reuse") != 2:
    failures.append(f"concurrent: no guest could exec after the fault "
                    f"(exit {rc_of('reuse')}, want 2) — released capacity not reusable")

# The host must never fault, and the shell must outlive every case.
if re.search(r"HardFault|Default_Handler|panic", text):
    failures.append("kernel faulted (HardFault/Default_Handler/panic in the log)")
if "Linux overtos" not in text:
    failures.append("shell did not survive the matrix (no uname output)")

print(f"cases={len(CASES)} failures={len(failures)}")
for f in failures:
    print(f"  FAIL: {f}")
print("RESULT:", "PASS" if not failures else "FAIL")
sys.exit(0 if not failures else 1)
