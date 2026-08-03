#!/usr/bin/env python3
"""STM32F746 HIL coverage for Linux-personality niceness scheduling.

Despite the legacy directory name, this driver is shared by FreeRTOS, NuttX,
and Zephyr images. Flash one STM32 linux_interop image first, then run:

    .venv/bin/python tests/sim/freertos-linux/nice_drive.py

Two compute-only Lua guests run concurrently at opposite ends of the supported
nice range. The test verifies that nice survives exec, both guests make forward
progress, and the more-favoured guest receives measurably more CPU time.
"""

import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from hilcon import Console, Timeout  # noqa: E402

LOG = sys.argv[1] if len(sys.argv) > 1 else "/tmp/nice_drive.log"
FAULT = re.compile(r"HardFault|BusFault|up_assert|PANIC|panic|FATAL|STACK OVERFLOW")


def checked(console, command, timeout=30.0):
    """Run a command and fail on its real status, not on a matching input echo."""
    output = console.cmd(command, timeout=timeout)
    if console.last_status != 0:
        raise RuntimeError(f"command failed: {command}\n{output}")
    return output


def run():
    # Pace the compound command so the test measures guest scheduling rather
    # than the small polled STM32 console RX path.
    console = Console(tx_delay=0.025)
    try:
        print("--- niceness: reset + login ---")
        console.reset()
        console.login(boot_timeout=45.0)

        identity = checked(console, "uname -a")
        engines = [name for name in ("FreeRTOS", "NuttX", "Zephyr") if name in identity]
        if len(engines) != 1:
            raise RuntimeError(f"could not identify exactly one RTOS engine\n{identity}")
        print(f"engine: {engines[0]}")

        command = (
            "nice -n -20 lua -e 'local x=0 while true do x=x+1 end' & hi=$!; "
            "nice -n 19 lua -e 'local x=0 while true do x=x+1 end' & lo=$!; "
            "sleep 6; "
            "awk '{print \"CPU\",$1,$14,$19}' /proc/$hi/stat /proc/$lo/stat; "
            "kill $hi $lo; wait $hi; wait $lo; true"
        )
        output = checked(console, command, timeout=50.0)
        rows = re.findall(r"CPU\s+(\d+)\s+(\d+)\s+(-?\d+)", output)
        if len(rows) != 2:
            raise RuntimeError(f"did not receive two unambiguous CPU rows\n{output}")

        high_ticks = int(rows[0][1])
        low_ticks = int(rows[1][1])
        if high_ticks <= 0 or low_ticks <= 0:
            raise RuntimeError(f"one compute-only guest made no progress: {rows}")
        if int(rows[0][2]) != -20 or int(rows[1][2]) != 19:
            raise RuntimeError(f"nice values were not preserved through exec: {rows}")
        if high_ticks * 4 < low_ticks * 5:
            raise RuntimeError(
                "higher-priority guest did not receive a larger CPU share: "
                f"high={high_ticks}, low={low_ticks}"
            )

        ratio = high_ticks / low_ticks
        print(
            f"weighted CPU: PASS (nice -20={high_ticks} ticks, "
            f"nice 19={low_ticks} ticks, ratio={ratio:.2f})"
        )
        fault = FAULT.search(console.buf)
        if fault:
            raise RuntimeError(f"host fault marker in transcript: {fault.group(0)}")
        return 0
    except (Timeout, RuntimeError) as error:
        print(f"RESULT: FAIL — {error}")
        return 1
    finally:
        with open(LOG, "w") as log:
            log.write(console.buf)
        console.close()
        print(f"transcript: {LOG}")


if __name__ == "__main__":
    result = run()
    print("RESULT:", "PASS" if result == 0 else "FAIL")
    sys.exit(result)
