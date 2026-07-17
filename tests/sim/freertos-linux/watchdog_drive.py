#!/usr/bin/env python3
# Watchdog verification for the FreeRTOS Linux personality — HARDWARE ONLY (STM32F746G-DISCO,
# USART1 @115200, reset via openocd). The IWDG is STM32 HAL only: the QEMU an500 machine has no
# IWDG and no RCC reset latch, so there is nothing to drive there. QEMU builds simply omit the
# watchdog (its Kconfig default is off outside this board), so no QEMU mode exists here.
#
#   watchdog_drive.py normal    [log]   requires CONFIG_OVE_WATCHDOG=y (the STM32 default).
#                                       Confirms the IWDG arms and the board runs stably at the
#                                       login prompt without a spurious reset — i.e. an idle host
#                                       is fed, not reset.
#   watchdog_drive.py selftest  [log]   requires CONFIG_OVE_WATCHDOG_SELFTEST=y as well. Proves the
#                                       policy actually resets a wedged host: the firmware starves
#                                       the coordinator (scheduler left alive), the monitor sees a
#                                       stalled heartbeat, withholds the feed, and the IWDG resets
#                                       the board — which must then report reset cause "watchdog"
#                                       and, being one-shot, not re-trip.
#
# Note on boot count: openocd asserts NRST on connect and again on disconnect, so an openocd-driven
# reset shows up as one or two "pin"-cause boots before the firmware settles. The checks below key
# on the reset CAUSE and the marker lines, never on an exact boot count.
import subprocess
import sys
import time

PORT, BAUD = "/dev/ttyACM0", 115200
mode = sys.argv[1] if len(sys.argv) > 1 else "selftest"
log = sys.argv[2] if len(sys.argv) > 2 else "/tmp/watchdog_drive.log"


def capture(seconds):
    import serial  # pyserial; only needed for the hardware path

    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    subprocess.run(["openocd", "-f", "board/stm32f7discovery.cfg", "-c", "init; reset run; exit"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        d = ser.read(4096)
        if d:
            buf.extend(d)
    return buf.decode("latin-1", "replace")


text = capture(30 if mode == "normal" else 45)
open(log, "w").write(text)

armed = "[wd] IWDG armed" in text
causes = [ln.split("cause:", 1)[1].strip() for ln in text.splitlines() if "[reset] cause:" in ln]
kfault = any(m in text for m in ("STACK OVERFLOW", "HardFault", "up_assert", "!!!"))
for ln in text.splitlines():
    s = ln.strip()
    if s.startswith("[wd]") or s.startswith("[reset]"):
        print(s)
print(f"\narmed={armed} reset_causes={causes} kfault={kfault}")

if mode == "normal":
    reached_login = "Welcome to Buildroot" in text
    # A normal run wedges nothing, so the watchdog must never be the cause of a reset here.
    spurious = "watchdog" in causes
    ok = armed and reached_login and not spurious and not kfault
    print(f"reached_login={reached_login} spurious_watchdog_reset={spurious}")
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
else:
    wedged = "selftest: starving the coordinator" in text
    fired = "watchdog" in causes  # the IWDG reset, decoded from RCC->CSR
    oneshot = "not re-tripping" in text  # recognised the recovery, did not wedge again
    ok = armed and wedged and fired and oneshot and not kfault
    print(f"wedge_triggered={wedged} watchdog_reset_seen={fired} one_shot_recovery={oneshot}")
    print("RESULT:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)
