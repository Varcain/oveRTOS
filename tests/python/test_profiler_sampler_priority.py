# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""The profiler's sampling interrupt must outrank the driver interrupts
whose time it attributes, from a level the kernel still masks.

The samplers take their sample in interrupt context. When the sampler
preempted another handler (SCB_ICSR.RETTOBASE == 0) the sample carries
that handler's exception number ("[ISR n]" in the dashboard). On Cortex-M
an interrupt of equal priority cannot preempt another one, so a driver
interrupt at the sampler's own level is never attributed.

The sampler also reads the kernel's clock and calls kernel APIs, so it
must stay at a level the kernel's critical sections mask: above it, a
sample taken inside the kernel's own clock update re-enters it.

  FreeRTOS - the sampling timer runs at configMAX_SYSCALL_INTERRUPT_PRIORITY
             and never changes its level at run time; the an500 drivers
             sit strictly below it, for every NVIC width the kernel may
             assume.
  Zephyr   - the board timer is connected at Zephyr priority 0, an
             ordinary level that irq_lock() masks, with no NVIC override.

The test is static: it reads the priorities out of the sources. No build,
no toolchain, no network.
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

FREERTOS_BACKEND = os.path.join(ROOT, "backends", "freertos", "freertos_profiler.c")
ZEPHYR_BACKEND = os.path.join(ROOT, "backends", "zephyr", "zephyr_profiler.c")
FREERTOS_CONFIG = os.path.join(ROOT, "config", "templates", "FreeRTOSConfig.h.j2")
AN500_FREERTOS = os.path.join(ROOT, "boards", "qemu-mps2-an500", "freertos")
AN500_ZEPHYR_BOARD = os.path.join(ROOT, "boards", "qemu-mps2-an500", "zephyr", "inc",
                                  "ove_board_profiler.h")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def template_default(name):
    """The default the FreeRTOSConfig template gives a config.get() value."""
    m = re.search(r'config\.get\("%s",\s*(\d+)\)' % name, read(FREERTOS_CONFIG))
    if not m:
        raise AssertionError(f"{name} has no default in {FREERTOS_CONFIG}")
    return int(m.group(1))


def evaluate(expr, prio_bits):
    """Evaluate a C priority expression with the template's FreeRTOS values."""
    values = {
        "configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY":
            template_default("CONFIG_FREERTOS_MAX_SYSCALL_INTERRUPT_PRIORITY"),
        "configLIBRARY_LOWEST_INTERRUPT_PRIORITY":
            template_default("CONFIG_FREERTOS_LOWEST_INTERRUPT_PRIORITY"),
        "configPRIO_BITS": prio_bits,
    }
    values["configMAX_SYSCALL_INTERRUPT_PRIORITY"] = (
        values["configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY"] << (8 - prio_bits))
    values["configKERNEL_INTERRUPT_PRIORITY"] = (
        values["configLIBRARY_LOWEST_INTERRUPT_PRIORITY"] << (8 - prio_bits))
    expr = " ".join(expr.split())
    expr = re.sub(r"\((?:uint8_t|uint32_t|unsigned int)\)", "", expr)
    expr = re.sub(r"(\d+)[uU]\b", r"\1", expr)
    for name, value in values.items():
        expr = re.sub(r"\b%s\b" % name, str(value), expr)
    if re.search(r"[A-Za-z_]", expr):
        raise AssertionError(f"cannot evaluate the priority expression {expr!r}")
    return eval(expr, {"__builtins__": {}}) & 0xFF  # noqa: S307 - digits and operators only


def implemented(prio, bits):
    """The priority byte as an NVIC with `bits` implemented bits keeps it."""
    return prio & ((0xFF << (8 - bits)) & 0xFF)


class FreeRTOSSamplerTest(unittest.TestCase):
    def setUp(self):
        self.src = strip_comments(read(FREERTOS_BACKEND))

    def timer_writes(self):
        return re.findall(r"NVIC_IPR_BYTE\(\s*PROF_TIMER_IRQ\s*\)\s*=\s*([^;]+);", self.src)

    def test_the_sampler_runs_at_the_syscall_ceiling(self):
        writes = self.timer_writes()
        self.assertEqual(len(writes), 1,
                         "the sampling timer's priority must be written exactly once, "
                         f"at start: {writes}")
        for bits in range(3, 9):
            self.assertEqual(evaluate(writes[0], bits), evaluate(
                "configMAX_SYSCALL_INTERRUPT_PRIORITY", bits),
                "the sampling timer must run at configMAX_SYSCALL_INTERRUPT_PRIORITY, "
                "the most urgent level the kernel's critical sections still mask")

    def test_the_sampler_never_changes_its_level(self):
        """No demote/restore window: the level is the same for every call."""
        self.assertNotRegex(self.src, r"NVIC_IPR_BYTE\(\s*PROF_TIMER_IRQ\s*\)\s*;",
                            "the sampler reads its own priority byte (a save/restore "
                            "window around kernel calls)")
        self.assertNotRegex(self.src, r"\bcpsid\b",
                            "the profiler masks interrupts with PRIMASK: the kernel's "
                            "critical sections are expected to mask the sampler")

    def test_the_an500_drivers_sit_below_the_sampler(self):
        """Every an500 driver line is strictly less urgent than the sampler
        and still at or above the kernel's own level, for every NVIC width."""
        writes = []
        for dirpath, _, files in os.walk(AN500_FREERTOS):
            for name in files:
                if name.endswith(".c"):
                    path = os.path.join(dirpath, name)
                    for line, expr in re.findall(r"(NVIC_IPR\[\s*(?:\w+)\s*\])\s*=\s*([^;]+);",
                                                 strip_comments(read(path))):
                        writes.append((os.path.relpath(path, ROOT), line, expr))
        self.assertTrue(writes, "no an500 FreeRTOS driver sets an NVIC priority")
        for path, line, expr in writes:
            for bits in range(3, 9):
                with self.subTest(path=path, line=line, prio_bits=bits):
                    prio = evaluate(expr, bits)
                    ceiling = evaluate("configMAX_SYSCALL_INTERRUPT_PRIORITY", bits)
                    kernel = evaluate("configKERNEL_INTERRUPT_PRIORITY", bits)
                    self.assertGreater(
                        implemented(prio, bits), implemented(ceiling, bits),
                        f"{line} = {expr} shares the sampler's level (or outranks it) "
                        f"on a {bits}-bit NVIC, so its time is never attributed")
                    self.assertLessEqual(prio, kernel,
                                         f"{line} sits below the kernel's own level")


class ZephyrSamplerTest(unittest.TestCase):
    def setUp(self):
        self.src = strip_comments(read(ZEPHYR_BACKEND))

    def test_the_board_timer_is_connected_at_priority_0(self):
        self.assertTrue(os.path.isfile(AN500_ZEPHYR_BOARD),
                        "the an500 has no Zephyr sampling timer header")
        self.assertRegex(read(AN500_ZEPHYR_BOARD), r"#define\s+OVE_BOARD_PROFILER_TIMER_BASE\b")
        connects = re.findall(r"IRQ_CONNECT\(\s*PROF_TIMER_IRQ\s*,\s*([^,]+),", self.src)
        self.assertEqual([c.strip() for c in connects], ["0"],
                         "the sampling timer must be connected at Zephyr priority 0")

    def test_no_level_above_the_kernel_mask(self):
        """irq_lock() must keep masking the sampler: no NVIC override, no
        zero-latency flag, no arithmetic on the kernel's priority offset."""
        self.assertNotRegex(self.src, r"NVIC_SetPriority\s*\(",
                            "the sampler overrides the priority IRQ_CONNECT gave it")
        self.assertNotRegex(self.src, r"\bIRQ_ZERO_LATENCY\b")
        self.assertNotRegex(self.src, r"\b_IRQ_PRIO_OFFSET\b")
        connect = re.search(r"IRQ_CONNECT\(\s*PROF_TIMER_IRQ\s*,[^;]*\)", self.src)
        self.assertTrue(connect)
        self.assertRegex(connect.group(0), r",\s*0\s*\)$",
                         "the sampling timer must be connected without flags")

    def test_the_sampler_skips_its_own_exception(self):
        """The [ISR n] lookup must never report the sampler itself."""
        self.assertRegex(self.src,
                         r"#define\s+SAMPLER_EXC\s+\(\s*16u?\s*\+\s*"
                         r"OVE_BOARD_PROFILER_TIMER_IRQ\s*\)")


if __name__ == "__main__":
    unittest.main()
