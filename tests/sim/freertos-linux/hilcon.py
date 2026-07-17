#!/usr/bin/env python3
# Reusable hardware-in-the-loop console for the STM32F746G-DISCO (USART1 @115200, reset via
# openocd). Prompt-driven expect/sendline, so a driver reacts to what the board actually printed
# instead of guessing with timed sleeps — the blind-keystroke approach the earlier drivers used
# desyncs the moment the board is a little slower or faster than the script assumed.
#
# Not a QEMU harness: the personality's Cortex-M MPU emulation is timing-fragile under QEMU with no
# pty, which is why the sim drivers stayed blind there. This talks to real silicon over a real UART,
# where expect is reliable.
#
# Typical use:
#   c = Console(); c.reset(); c.login(); c.cmd("uname -a"); c.close()
import re
import subprocess
import time


class Timeout(Exception):
    pass


class Console:
    PORT = "/dev/ttyACM0"
    BAUD = 115200
    OPENOCD_CFG = "board/stm32f7discovery.cfg"

    def __init__(self, port=None, baud=None):
        import serial  # pyserial; hardware-only dependency

        self.ser = serial.Serial(port or self.PORT, baud or self.BAUD, timeout=0.1)
        self.buf = ""          # everything seen, for post-hoc checks
        self._un = ""          # unconsumed tail expect() scans

    def reset(self):
        """Hardware-reset the board so a run starts from a known boot, not mid-session state."""
        subprocess.run(["openocd", "-f", self.OPENOCD_CFG, "-c", "init; reset run; exit"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)

    def _pump(self):
        n = self.ser.in_waiting
        if n:
            d = self.ser.read(n).decode("latin-1", "replace")
            self.buf += d
            self._un += d
            return True
        return False

    def expect(self, pattern, timeout=15.0):
        """Block until `pattern` (regex) appears in the stream; return the text up to and including
        it. Consumes through the match so the next expect starts after it. Raises Timeout."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            m = rx.search(self._un)
            if m:
                out = self._un[: m.end()]
                self._un = self._un[m.end():]
                return out
            if not self._pump():
                time.sleep(0.02)
        raise Timeout(f"expected {pattern!r}, unseen in {timeout}s")

    def sendline(self, s=""):
        self.ser.write((s + "\r").encode())
        self.ser.flush()

    def login(self, user="root", password="root", boot_timeout=30.0):
        """Wait out boot (phase 1 round trip + phase 2 init) and log into the BusyBox shell. Two
        credentials: the account has a password, so login prompts for name and password."""
        self.expect(r"overtos login:", timeout=boot_timeout)
        self.sendline(user)
        self.expect(r"[Pp]assword:", timeout=8.0)
        self.sendline(password)
        self.expect(r"# ", timeout=8.0)

    _MARK = 0

    def cmd(self, line, timeout=15.0):
        """Run `line` in the shell and return its output. A per-call marker is appended and waited
        on, so output is delimited exactly regardless of how long the command took or how much it
        printed — no fixed sleep can desync it."""
        Console._MARK += 1
        mark = f"__OK_{Console._MARK}__"
        self.sendline(f"{line}; echo {mark}")
        out = self.expect(re.escape(mark), timeout=timeout)
        return out

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass
