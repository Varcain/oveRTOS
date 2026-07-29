#!/usr/bin/env python3
"""STM32F746 HIL coverage for Linux-personality writable storage.

Despite the legacy directory name, this driver is shared by FreeRTOS, NuttX,
and Zephyr images.  Flash one STM32 linux_interop image first, then run:

    .venv/bin/python tests/sim/freertos-linux/storage_drive.py

The test deliberately resets the MCU between the write and final read so it
distinguishes persistent /data from the bounded, volatile /tmp.
"""

import re
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from hilcon import Console, Timeout  # noqa: E402

LOG = sys.argv[1] if len(sys.argv) > 1 else "/tmp/storage_drive.log"
TMP_PATH = "/tmp/.ove-storage-probe"
DATA_DIR = "/data/.ove-storage-probe"
PAYLOAD_BYTES = 200 * 1024
FAULT = re.compile(r"HardFault|BusFault|up_assert|PANIC|panic|FATAL|STACK OVERFLOW")


def checked(console, command, timeout=30.0):
    """Run a command and fail on its real status, not on a matching input echo."""
    output = console.cmd(command, timeout=timeout)
    if console.last_status != 0:
        raise RuntimeError(f"command failed: {command}\n{output}")
    return output


def require_output(console, command, expected, timeout=30.0):
    output = checked(console, command, timeout)
    if expected not in output:
        raise RuntimeError(
            f"missing {expected!r} in output from {command!r}\n{output}"
        )
    return output


def run():
    # Long Lua command lines can overrun the board's small polled console RX
    # path. Pace bytes so the test measures storage rather than UART buffering.
    console = Console(tx_delay=0.025)
    try:
        print("--- storage: reset + login ---")
        console.reset()
        console.login(boot_timeout=45.0)

        identity = checked(console, "uname -a")
        engines = [name for name in ("FreeRTOS", "NuttX", "Zephyr") if name in identity]
        if len(engines) != 1:
            raise RuntimeError(f"could not identify exactly one RTOS engine\n{identity}")
        print(f"engine: {engines[0]}")

        checked(console, "ls -ld /data /tmp")
        checked(console, f"rm -f {TMP_PATH}")
        checked(
            console,
            "lua -e \"local f=assert(io.open('"
            + TMP_PATH
            + "','wb')); for i=1,200 do "
            "assert(f:write(string.rep('x',1024))) end; assert(f:close())\"",
            timeout=45.0,
        )
        require_output(console, f"wc -c {TMP_PATH}", f"{PAYLOAD_BYTES} {TMP_PATH}")
        checked(console, f"rm -f {TMP_PATH}")
        print(f"/tmp: PASS ({PAYLOAD_BYTES} byte bounded-pool write)")

        checked(console, f"rm -rf {DATA_DIR}")
        checked(console, f"mkdir {DATA_DIR}")
        checked(
            console,
            f"printf 'persistent-storage-ok\\n' > {DATA_DIR}/value",
        )
        require_output(console, f"cat {DATA_DIR}/value", "persistent-storage-ok")
        checked(console, f"mv {DATA_DIR}/value {DATA_DIR}/renamed")
        require_output(console, f"ls -l {DATA_DIR}", "renamed")

        print("--- storage: hardware reset for persistence check ---")
        console.reset()
        console.login(boot_timeout=45.0)
        require_output(console, f"cat {DATA_DIR}/renamed", "persistent-storage-ok")
        checked(console, f"rm -rf {DATA_DIR}")
        print("/data: PASS (create/read/rename/list + persistence across reset)")

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
