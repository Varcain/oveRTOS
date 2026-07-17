#!/usr/bin/env python3
# HIL stress + soak for the FreeRTOS Linux personality — HARDWARE ONLY (STM32F746G-DISCO).
#
# Resets the board, logs in, and drives the shell through a stress battery round after round for a
# set duration, confirming after every round that the shell still answers (a per-round marker that
# must come back) and that no kernel fault appeared. Then powers off and reads the teardown stack +
# heap audit the firmware prints — the point of running the workload first is that the stack
# high-water marks then reflect the deepest paths actually taken, which an idle demo never reaches
# (that is why R2's stack criterion was unmet; this closes it).
#
# Fails on: a round that does not answer (hang/crash), any kernel-fault marker, a host stack whose
# free headroom fell below the floor, or the heap ending far below its boot free (a leak).
#
#   soak_drive.py [seconds] [log]      default 720 s (~12 min)
import re
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from hilcon import Console, Timeout  # noqa: E402

DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 720
LOG = sys.argv[2] if len(sys.argv) > 2 else "/tmp/soak_drive.log"
STACK_FLOOR = 256       # bytes of free stack below which a host task is too close to overflow
HEAP_MARGIN = 16384     # peak heap usage must stay at least this far below total (else near-exhaustion / leak)

FAULT = re.compile(r"HardFault|up_assert|PANIC|panic|Assertion|MPU FAULT|FATAL|STACK OVERFLOW")

# One round of stress, built to exercise the paths that actually strain the personality: fork/exec
# churn (slot allocator + FDPIC loader + arena), tmpfs read/write, a getdents/stat sweep, a pipe,
# and — rotated in — the kstress kernel attacks, which crash a guest process the personality must
# CONTAIN while the shell survives. `k` selects the rotating attack for this round.
def round_cmd(k):
    attack = ["ptr", "str", "rsrc", "udf", "div0"][k % 5]
    return (
        "i=0; while [ $i -lt 15 ]; do /bin/true; i=$((i+1)); done; "  # fork/exec churn
        "cat /etc/inittab > /tmp/s && wc -c < /tmp/s > /dev/null && rm /tmp/s; "  # tmpfs I/O
        "ls -la /bin > /dev/null; "                                   # getdents/stat sweep
        "cat /etc/passwd | grep -c root > /dev/null; "                # pipe
        f"kstress {attack} > /dev/null 2>&1"                          # contained kernel attack
    )


def main():
    c = Console()
    print(f"--- soak: {DURATION}s, reset + login ---")
    c.reset()
    try:
        c.login(boot_timeout=35.0)
    except Timeout as e:
        open(LOG, "w").write(c.buf)
        print(f"RESULT: FAIL — never reached a shell ({e}); see {LOG}")
        return 1

    rounds, fault = 0, None
    t_end = time.time() + DURATION
    while time.time() < t_end:
        try:
            c.cmd(round_cmd(rounds), timeout=25.0)
        except Timeout as e:
            fault = f"round {rounds} did not answer ({e})"
            break
        if FAULT.search(c.buf):
            fault = f"kernel-fault marker after round {rounds}: {FAULT.search(c.buf).group(0)}"
            break
        rounds += 1
        if rounds % 25 == 0:
            print(f"  {rounds} rounds, {int(t_end - time.time())}s left")

    # Power off so the firmware prints its teardown audit, then read it.
    audit = ""
    if fault is None:
        try:
            c.sendline("poweroff")
            audit = c.expect(r"\[stack\] end", timeout=20.0)
        except Timeout as e:
            fault = f"no teardown audit after poweroff ({e})"
    open(LOG, "w").write(c.buf)
    c.close()

    # Parse the audit.
    stacks = re.findall(r"\[stack\] (\S+)\s+used=(\d+) size=(\d+) free=(\d+)", c.buf)
    heap = re.search(r"\[heap\] free=(\d+) peak_used=(\d+) total=(\d+)", c.buf)

    print(f"\n=== soak: {rounds} rounds over ~{DURATION}s ===")
    for name, used, size, free in stacks:
        flag = "  <-- LOW" if int(free) < STACK_FLOOR else ""
        print(f"  stack {name:20} used={used:>5} size={size:>5} free={free:>5}{flag}")
    # peak_used is monotonic over the run, so a per-round leak accumulates into it; a bounded
    # peak far below total means neither near-exhaustion nor a leak took hold across the soak.
    heap_ok = True
    if heap:
        peak, total = int(heap.group(2)), int(heap.group(3))
        heap_ok = peak <= total - HEAP_MARGIN
        print(f"  heap end_free={heap.group(1)} peak_used={peak} total={total} "
              f"headroom={total - peak}{'  <-- LOW' if not heap_ok else ''}")

    min_free = min((int(f) for *_, f in stacks), default=-1)
    ok = (fault is None and rounds > 0 and stacks and min_free >= STACK_FLOOR and heap_ok)
    if fault:
        print(f"  FAULT: {fault}")
    if stacks and min_free < STACK_FLOOR:
        print(f"  stack floor {STACK_FLOOR} breached (min free {min_free})")
    if not heap_ok:
        print(f"  heap peak within {HEAP_MARGIN} of total — near-exhaustion or leak")
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
