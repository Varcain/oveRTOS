#!/usr/bin/env python3
# C6 proof: a fault in privileged/host context is FATAL, never mis-contained as a guest fault.
# HARDWARE ONLY (STM32F746G-DISCO). Requires a build with CONFIG_OVE_LINUX_FAULTTEST=y (which also
# needs CONFIG_OVE_WATCHDOG for recovery + the one-shot).
#
# A few seconds into phase 2, a privileged host task executes an undefined instruction. The seam's
# fault handler cannot attribute it to a guest slot, so it declines containment, prints a HOST FAULT
# diagnostic (faulting PC + CFSR/HFSR) and halts; the watchdog then reboots and the reset cause reads
# "watchdog". The next boot recognises the recovery and does not re-trip.
#
# Passes iff: the diagnostic banner appeared (declined + reported), the reboot cause was watchdog
# (fatal + recovered), and the run was one-shot. Uses hilcon.py — the same console harness as the
# soak driver.
import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from hilcon import Console, Timeout  # noqa: E402

LOG = sys.argv[1] if len(sys.argv) > 1 else "/tmp/faulttest_drive.log"


def main():
    c = Console()
    print("--- C6 host-fault test: reset, then watch the fault + recovery ---")
    c.reset()
    ok = True
    try:
        c.expect(r"\[c6\] faulting a privileged host task", timeout=40.0)   # the test fired
        c.expect(r"!!! HOST FAULT", timeout=8.0)                            # seam declined + reported
        diag = c.expect(r"host state compromised", timeout=4.0)            # ...and halted
        c.expect(r"\[reset\] cause: watchdog", timeout=15.0)               # fatal -> watchdog reboot
        c.expect(r"recovered from the host-fault test", timeout=10.0)      # one-shot
    except Timeout as e:
        ok = False
        print(f"  MISSING: {e}")

    text = c.buf
    open(LOG, "w").write(text)
    c.close()

    # No guest-exit path may have run for the host fault (it must NOT be recovered as a slot exit).
    mis_contained = re.search(r"exit.*139|MEMORY_FAULT|guest exited", text, re.I) is not None
    pcline = next((l.strip() for l in text.splitlines() if "pc=0x" in l and "HOST FAULT" not in l), "")
    for l in text.splitlines():
        s = l.strip()
        if s.startswith("!!!") or s.startswith("[c6]") or "cause:" in s:
            print(" ", s[:98])
    print(f"\ndiagnostic={pcline!r}")
    print(f"mis_contained_as_guest={mis_contained}")
    ok = ok and not mis_contained
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
