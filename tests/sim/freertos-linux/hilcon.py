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

    def __init__(self, port=None, baud=None, tx_delay=0.0):
        import serial  # pyserial; hardware-only dependency

        self.ser = serial.Serial(port or self.PORT, baud or self.BAUD, timeout=0.1)
        self.buf = ""          # everything seen, for post-hoc checks
        self._un = ""          # unconsumed tail expect() scans
        self.tx_delay = tx_delay
        self.last_status = None

    def reset(self):
        """Hardware-reset the board so a run starts from a known boot, not mid-session state."""
        subprocess.run(["openocd", "-f", self.OPENOCD_CFG, "-c", "init; reset run; exit"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        # ST-Link may retain console bytes emitted before/during the reset. If
        # an old login prompt remains in either buffer, login() can accept it
        # and transmit credentials while the new firmware is still booting.
        # Preserve the complete transcript in self.buf, but make future
        # expect() calls consume only bytes emitted after this reset.
        self.ser.reset_input_buffer()
        self._un = ""

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
        data = (s + "\r").encode()
        if self.tx_delay:
            for byte in data:
                self.ser.write(bytes((byte,)))
                self.ser.flush()
                time.sleep(self.tx_delay)
        else:
            self.ser.write(data)
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
        mark_id = Console._MARK
        mark = re.compile(rf"__OK_{mark_id}_([0-9]+)__")
        # Do not put the expanded marker in the command line: terminals echo
        # input, and expect() would otherwise accept that echo before the
        # command had run.  The shell expands both %s fields only when printf
        # executes; the second field captures the command's real exit status.
        self.sendline(f"{line}; printf '\\n__OK_%s_%s__\\n' {mark_id} $?")
        out = self.expect(mark.pattern, timeout=timeout)
        match = mark.search(out)
        self.last_status = int(match.group(1))
        # A completed child may hand the console back just after the RT-scope
        # task's reporting window. Allow one full report interval for the
        # interactive shell to emit its next prompt.
        out += self.expect(r"# ", timeout=15.0)
        return out

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass
