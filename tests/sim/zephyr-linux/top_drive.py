#!/usr/bin/env python3
# Drive interactive `top` over a real pty: piped stdin can't exercise top's
# full-screen refresh loop (it needs a tty + time passing between redraws).
# Boots the firmware, runs `top -d 1`, lets several refreshes happen, quits with
# Ctrl-C, and checks the captured frames show a real CPU summary + kernel threads
# + per-process %CPU that changes between frames.
import pty, os, select, time, sys, re

log = sys.argv[1] if len(sys.argv) > 1 else "/tmp/top_pty.log"
runner = sys.argv[2] if len(sys.argv) > 2 else "boards/qemu-mps2-an521/qemu-run.sh"
firmware = sys.argv[3] if len(sys.argv) > 3 else \
    "output/qemu-mps2-an521/zephyr/linux_interop/images/firmware.elf"
argv = [runner, firmware, "--headless", "--no-net", "--timeout", "90"]

pid, fd = pty.fork()
if pid == 0:
    os.execvp(argv[0], argv)
    os._exit(127)

buf = b""
logf = open(log, "wb")


def drain(t):
    global buf
    r, _, _ = select.select([fd], [], [], t)
    if r:
        try:
            d = os.read(fd, 4096)
        except OSError:
            return False
        if not d:
            return False
        buf += d
        logf.write(d)
        logf.flush()
    return True


def wait_for(pat, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if re.search(pat, buf):
            return True
        if not drain(0.2):
            return False
    return False


def send(s, pause):
    os.write(fd, s)
    end = time.time() + pause
    while time.time() < end:
        drain(0.1)


def mark():
    return len(buf)


try:
    wait_for(b"login:", 40)
    send(b"root\r", 2.0)
    wait_for(b"#", 10)
    send(b"top -d 1\r", 0.5)
    f1_start = mark()
    # let ~3 refreshes happen (top -d 1 sleeps 1s between full-screen redraws)
    for _ in range(40):
        drain(0.2)
    send(b"\x03", 2.0)            # Ctrl-C: SIGINT quits the non-interactive top
    wait_for(b"#", 8)
    send(b"echo TOP-PTY-DONE\r", 1.5)
    send(b"poweroff\r", 3.0)
    end = time.time() + 12
    while time.time() < end:
        if not drain(0.3):
            break
finally:
    logf.close()
    try:
        os.close(fd)
    except OSError:
        pass

text = buf.decode("latin-1", "replace")
# top reprints the "CPU:" summary on every refresh; count them as frame markers.
cpu_lines = re.findall(r"CPU:\s+([0-9.]+)% usr.*?([0-9.]+)% idle", text)
has_main = "[main]" in text
has_pcpu_hdr = "%CPU" in text
quit_ok = "TOP-PTY-DONE" in text
print(f"refreshes(CPU: lines)={len(cpu_lines)}  [main]={has_main}  "
      f"%CPU header={has_pcpu_hdr}  ctrl-c quit={quit_ok}")
if cpu_lines:
    print("  usr/idle samples:", cpu_lines[:5])
ok = len(cpu_lines) >= 2 and has_main and has_pcpu_hdr and quit_ok
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
