# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""The simulation build-system doc must agree with the code it documents.

`docs-site/docs/build-system/sim.md` is the reference for the sim
framework: the profiler's sampling-rate ceiling, the trace/profiler wire
formats, the plugin vtable and the Kconfig options.  A stale number or a
stale wire format there is worse than no documentation at all, because the
dashboard and the firmware are written against the code, not the page.

This test cross-checks the concrete, machine-checkable claims of that page
against the tree:

  * the profiler sampling-rate ceiling: `CONFIG_OVE_PROFILER_HZ`, further
    clamped to the kernel tick rate on exactly the backends whose
    `ove_backend_profiler_get_max_hz()` clamps it, and the caps event
    carrying the *effective* maximum,
  * the trace envelope: version, `sub_type` values and the record size,
  * the profiler envelope version,
  * the plugin `tick` op: the consolidated pump's 50 ms / ~20 Hz cadence
    and the fact that `ove_sim_plugin_tick_all()` has no caller,
  * the Kconfig table's defaults,
  * the markdown itself: balanced code fences and uniform table rows (a
    stray `|` or an unbalanced fence breaks the docs build).

The document under test is `docs-site/docs/build-system/sim.md`; pass a
path as the first argument to check another copy (e.g. a pre-fix one).
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOC = os.path.join(ROOT, "docs-site", "docs", "build-system", "sim.md")

TRACE_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_trace.h")
TRACE_C = os.path.join(ROOT, "sim", "hal", "sim_trace.c")
PROFILER_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_profiler.h")
PLUGIN_H = os.path.join(ROOT, "sim", "include", "ove", "sim", "ove_sim_plugin.h")
DEBUG_C = os.path.join(ROOT, "sim", "hal", "sim_debug.c")
SIM_PROFILER_C = os.path.join(ROOT, "sim", "hal", "sim_profiler.c")
CONFIG_DEBUG = os.path.join(ROOT, "config", "Config.in.debug")
CONFIG_MODULES = os.path.join(ROOT, "config", "Config.in.modules")
AN500_BOARD = os.path.join(ROOT, "boards", "qemu-mps2-an500", "board.yaml")

BACKENDS = {
    "freertos": os.path.join(ROOT, "backends", "freertos", "freertos_profiler.c"),
    "zephyr": os.path.join(ROOT, "backends", "zephyr", "zephyr_profiler.c"),
    "nuttx": os.path.join(ROOT, "backends", "nuttx", "nuttx_profiler.c"),
}

# Backend name as it is spelled in the document.
BACKEND_LABEL = {
    "freertos": "freertos",
    "zephyr": "zephyr",
    "nuttx": "nuttx",
}

# The kernel-tick symbol each backend clamps against.
TICK_SYMBOL = {
    "zephyr": "CONFIG_SYS_CLOCK_TICKS_PER_SEC",
    "nuttx": "TICK_PER_SEC",
}

# A sentence that ties the ceiling to the kernel tick rate.
TICK_TERM = re.compile(r"\bticks?\b|TICK_PER_SEC|SYS_CLOCK_TICKS_PER_SEC", re.I)
CLAMP_WORD = re.compile(
    r"\b(clamp(?:ed|s|ing)?|capped|cap|limit(?:ed|s|ing)?|bound(?:ed)?|"
    r"restrict(?:ed|s|ing)?|at\s+most|no\s+more\s+than|not\s+exceed\w*|"
    r"maximum|max|min|effective|smaller|lower|ceiling)\b",
    re.I,
)
EFFECTIVE_WORD = re.compile(r"\b(effective|effectively|clamp(?:ed|s)?|actual|"
                             r"resulting|limited|bound|maximum)\b", re.I)

# Source roots scanned for a caller of ove_sim_plugin_tick_all().
SCAN_ROOTS = ("sim", "include", "src", "backends", "drivers", "apps", "boards",
              "tests", "config", "loader")
SCAN_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".py", ".cmake", ".inc", ".j2",
                  ".sh", ".mk", ".s", ".S")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def doc_arg(argv):
    """The document named on the command line: (index, path) or (None, None)."""
    for i, arg in enumerate(argv):
        if not arg.startswith("-") and os.path.isfile(arg):
            return i, os.path.abspath(arg)
    return None, None


# The document under test: the repository's, unless another copy is named on
# the command line.  Resolved at import time, before the argument is taken
# out of argv for unittest.
_DOC_INDEX, _DOC_PATH = doc_arg(sys.argv[1:])
DOC_FILE = _DOC_PATH if _DOC_PATH else DOC


def sentences(lines):
    """Yield (line_no, sentence) for every line of the markdown."""
    for no, line in lines:
        for sent in re.split(r"(?<=[.!?])\s+", line):
            sent = sent.strip()
            if sent:
                yield no, sent


def code_block_after(lines, prefix):
    """Return the lines of the first fenced block that follows `prefix`."""
    for i, (_, line) in enumerate(lines):
        if line.startswith(prefix):
            body = []
            in_block = False
            for no, rest in lines[i + 1:]:
                if rest.strip().startswith("```"):
                    if in_block:
                        return body
                    in_block = True
                    continue
                if in_block:
                    body.append((no, rest))
            return body
    return []


def kconfig_symbol(text, symbol):
    """Return the body of a Kconfig symbol."""
    out = []
    inside = False
    for line in text.splitlines():
        m = re.match(r"^config\s+(\w+)\s*$", line)
        if m:
            inside = m.group(1) == symbol
            continue
        if inside:
            if re.match(r"^(config|menu|endmenu|source|if|endif)\b", line):
                inside = False
                continue
            out.append(line)
    return out


def kconfig_default(text, symbol):
    """Effective default of a Kconfig symbol: a number, 'y', 'n' or
    'y if <cond>'.  None when the symbol has no default line."""
    body = kconfig_symbol(text, symbol)
    fallback = None
    for line in body:
        d = re.match(r"^\s*default\s+(.+?)\s*$", line)
        if not d:
            continue
        value = d.group(1)
        if " if " in value:
            fallback = value
            continue
        return value
    if fallback is not None:
        return fallback
    # A bool without a default line defaults to n.
    if any(re.match(r"^\s*bool\b", line) for line in body):
        return "n"
    return None


def normalise_default(value):
    """Normalise a Kconfig default to the spelling used in the doc."""
    if value is None:
        return None
    value = value.replace("`", "").strip()
    value = value.replace("OVE_", "").replace("SIM", "SIM")
    value = value.replace("default ", "")
    if value == "y if SIM":
        return "y if SIM"
    return value


def extract_function_body(src, func):
    """Lift the verbatim body of `uint32_t func(void) { ... }` out of C source."""
    m = re.search(r"\buint32_t\s+" + re.escape(func) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        return ""
    start = src.index("{", m.start())
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    return ""


class SimDocsConsistencyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc_file = DOC_FILE
        cls.text = read(cls.doc_file)
        cls.lines = [(i + 1, text) for i, text in enumerate(cls.text.split("\n"))]

    # -- profiler sampling-rate ceiling ---------------------------------

    def test_ceiling_claim_matches_the_backends(self):
        """The doc must document the clamp exactly where the code clamps."""
        clamping = {}
        for name, path in BACKENDS.items():
            body = extract_function_body(read(path), "ove_backend_profiler_get_max_hz")
            self.assertTrue(body, f"cannot lift ove_backend_profiler_get_max_hz() "
                                  f"out of {path}")
            symbol = None
            for candidate in TICK_SYMBOL.values():
                if re.search(r"\b" + candidate + r"\b", body):
                    symbol = candidate
            self.assertIn("CONFIG_OVE_PROFILER_HZ", body,
                          f"{path}: the ceiling must come from CONFIG_OVE_PROFILER_HZ")
            if symbol:
                # A clamped backend must take the lower of the two values:
                # `(a < b) ? a : b` or `(a > b) ? b : a`.
                self.assertTrue(
                    re.search(r"\(\s*(\w+)\s*<=?\s*(\w+)\s*\)\s*\?\s*\1\s*:\s*\2\b",
                              body)
                    or re.search(r"\(\s*(\w+)\s*>=?\s*(\w+)\s*\)\s*\?\s*\2\s*:\s*\1\b",
                                 body),
                    f"{path}: the tick clamp must be a min()")
                clamping[name] = symbol
            else:
                self.assertNotRegex(body, r"TICK_PER_SEC|SYS_CLOCK_TICKS_PER_SEC",
                                    f"{path}: an unrecognised tick symbol")

        # The doc must tie the ceiling to the kernel tick rate, and must do
        # so for exactly the clamping backends.
        clamp_sentences = [(no, sent) for no, sent in sentences(self.lines)
                            if TICK_TERM.search(sent) and CLAMP_WORD.search(sent)]
        self.assertTrue(clamp_sentences,
                        "the document never mentions the kernel-tick clamp; the "
                        "code clamps the ceiling to the kernel tick rate on "
                        + ", ".join(sorted(clamping)))
        mentioned = set()
        for _, sent in clamp_sentences:
            for name in BACKENDS:
                if re.search(r"\b" + BACKEND_LABEL[name] + r"\b", sent, re.I):
                    mentioned.add(name)
        self.assertEqual(mentioned, set(clamping),
                         "the documented clamp does not match the code: the code "
                         f"clamps on {sorted(clamping)}, the document mentions "
                         f"{sorted(mentioned)}")

    def test_ceiling_default_matches_kconfig(self):
        default = kconfig_default(read(CONFIG_DEBUG), "OVE_PROFILER_HZ")
        self.assertEqual(default, "250")
        claim = [sent for _, sent in sentences(self.lines)
                 if "CONFIG_OVE_PROFILER_HZ" in sent]
        self.assertTrue(claim, "the document never mentions CONFIG_OVE_PROFILER_HZ")
        self.assertTrue(any(f"{default} Hz" in s for s in claim),
                        f"the document does not state the {default} Hz default of "
                        "CONFIG_OVE_PROFILER_HZ")

    def test_caps_event_carries_the_effective_maximum(self):
        caps = [sent for _, sent in sentences(self.lines)
                if re.search(r"caps", sent, re.I) and "max_hz" in sent]
        self.assertTrue(caps, "the document does not describe the caps event")
        self.assertTrue(any(EFFECTIVE_WORD.search(s) for s in caps),
                        "the document does not say that the caps event carries the "
                        "effective (tick-clamped) maximum")
        # max_hz is the backend's ceiling as sent: the sim lowers the
        # selected rate (current_hz), never the advertised maximum.
        emit = re.search(r"static void emit_caps\(.*?\n\}", read(SIM_PROFILER_C), re.S)
        self.assertTrue(emit, "emit_caps() not found in sim/hal/sim_profiler.c")
        self.assertRegex(emit.group(0),
                         r"max_hz\s*=\s*ove_backend_profiler_get_max_hz\(\)",
                         "the caps event no longer sends the backend's maximum")
        described = re.findall(r"`u32 max_hz` \(([^)]*)\)", self.text)
        self.assertTrue(described, "the document does not describe the caps max_hz field")
        for text in described:
            self.assertNotRegex(text, r"\blower",
                                "the document says the sim lowers max_hz; it "
                                "lowers current_hz")

    # -- trace wire format ----------------------------------------------

    def test_trace_envelope_version(self):
        code = re.search(r"#define\s+OVE_SIM_TRACE_VERSION\s+(\d+)", read(TRACE_H))
        self.assertTrue(code, "OVE_SIM_TRACE_VERSION not found")
        version = int(code.group(1))
        block = code_block_after(self.lines, "**Trace**")
        documented = [int(m.group(1)) for _, line in block
                      for m in re.finditer(r"^\s*u8\s+version\s+(\d+)\s*$", line)]
        self.assertTrue(documented, "the trace envelope block does not state a version")
        self.assertEqual(documented[0], version,
                         "the documented trace envelope version differs from "
                         f"OVE_SIM_TRACE_VERSION {version}")

    def test_trace_sub_types(self):
        """Each documented `N = name` pair must match OVE_SIM_TRACE_SUB_<NAME>."""
        defined = {name.lower(): int(v) for name, v in re.findall(
            r"#define\s+OVE_SIM_TRACE_SUB_(\w+)\s+(\d+)", read(TRACE_H))}
        self.assertTrue(defined, "no OVE_SIM_TRACE_SUB_* values found")
        block = code_block_after(self.lines, "**Trace**")
        documented = {}
        for _, line in block:
            m = re.match(r"^\s*u8\s+sub_type\s+(.*)$", line)
            if m:
                documented.update({name.lower(): int(v) for v, name in
                                   re.findall(r"(\d+)\s*=\s*(\w+)", m.group(1))})
        self.assertTrue(documented, "the trace envelope block does not state sub_type")
        self.assertEqual(documented, defined,
                         "the documented trace sub_type values differ from the "
                         "OVE_SIM_TRACE_SUB_* values")

    def test_trace_record_size(self):
        asserted = re.search(r"trace record wire size is (\d+) bytes", read(TRACE_C))
        self.assertTrue(asserted, "no record-size assertion in sim/hal/sim_trace.c")
        size = int(asserted.group(1))
        struct = re.search(r"struct\s+ove_trace_record\s*\{(.*?)\};", read(TRACE_C)
                           + read(os.path.join(ROOT, "backends", "common",
                                               "ove_trace_ring.h")), re.S)
        self.assertTrue(struct, "struct ove_trace_record not found")
        for field in ("ts_us", "tid", "kind", "code", "reserved", "object"):
            self.assertIn(field, struct.group(1),
                          f"the documented record field {field} is not in the struct")
        for _, line in self.lines:
            for m in re.finditer(r"(\d+)-byte records", line):
                self.assertEqual(int(m.group(1)), size,
                                 "the documented trace record size differs from the "
                                 f"size asserted in sim/hal/sim_trace.c ({size} bytes)")
        # The wire-format paragraph states the current record size in words.
        current = [int(m.group(1)) for _, sent in sentences(self.lines)
                   for m in re.finditer(r"Stream records are (\d+) bytes", sent)]
        self.assertTrue(current, "the document does not state the stream record size")
        self.assertEqual(current, [size] * len(current),
                         "the documented stream record size differs from the size "
                         f"asserted in sim/hal/sim_trace.c ({size} bytes)")

    def test_profiler_envelope_version(self):
        code = re.search(r"#define\s+OVE_SIM_PROFILER_VERSION\s+(\d+)", read(PROFILER_H))
        self.assertTrue(code, "OVE_SIM_PROFILER_VERSION not found")
        version = int(code.group(1))
        block = code_block_after(self.lines, "**Profiler samples**")
        documented = [int(m.group(1)) for _, line in block
                      for m in re.finditer(r"^\s*u8\s+version\s+(\d+)\s*$", line)]
        self.assertTrue(documented, "the profiler envelope block does not state a version")
        self.assertEqual(documented[0], version,
                         "the documented profiler envelope version differs from "
                         f"OVE_SIM_PROFILER_VERSION {version}")

    # -- plugin vtable ---------------------------------------------------

    def test_plugin_tick_rate(self):
        pump = re.search(r"#define\s+PUMP_TICK_MS\s+(\d+)u?", read(DEBUG_C))
        self.assertTrue(pump, "PUMP_TICK_MS not found in sim/hal/sim_debug.c")
        period = int(pump.group(1))
        self.assertEqual(period, 50)
        row = [line for _, line in self.lines
               if line.startswith("| `tick`")]
        self.assertTrue(row, "the plugin ops table has no `tick` row")
        row = row[0]
        self.assertNotIn("1 kHz", row, "the plugin `tick` op still claims ~1 kHz")
        self.assertIn(f"{period} ms", row,
                      f"the plugin `tick` row does not state the pump's {period} ms "
                      "cadence")

    def test_plugin_tick_all_has_no_caller(self):
        """`tick` is dead code: ove_sim_plugin_tick_all() must have no caller."""
        name = "ove_sim_plugin_tick_all"
        allowed = {os.path.abspath(TRACE_H), os.path.abspath(PLUGIN_H),
                   os.path.abspath(os.path.join(ROOT, "sim", "src",
                                                "ove_sim_registry.c"))}
        allowed.add(os.path.abspath(__file__))
        callers = []
        self_ = os.path.abspath(__file__)
        for root in SCAN_ROOTS:
            base = os.path.join(ROOT, root)
            if not os.path.isdir(base):
                continue
            for dirpath, dirnames, filenames in os.walk(base):
                dirnames[:] = [d for d in dirnames if d not in (".git", "__pycache__")]
                for fn in filenames:
                    if not fn.endswith(SCAN_SUFFIXES):
                        continue
                    path = os.path.join(dirpath, fn)
                    if os.path.abspath(path) in allowed or os.path.abspath(path) == self_:
                        continue
                    try:
                        text = read(path)
                    except OSError:
                        continue
                    if name in text:
                        callers.append(os.path.relpath(path, ROOT))
        self.assertEqual(callers, [],
                         f"{name}() has a caller now: {callers}; the document says "
                         "it has none, so one of the two must be updated")

    def test_plugin_header_comment_matches(self):
        text = read(PLUGIN_H)
        m = re.search(r"/\*\*(.*?)\*/\s*void\s*\(\*tick\)", text, re.S)
        self.assertTrue(m, "the `tick` op has no doc comment")
        comment = m.group(1)
        self.assertNotIn("1 kHz", comment,
                         "the `tick` op comment still claims ~1 kHz")

    # -- Kconfig table ---------------------------------------------------

    def test_kconfig_table_defaults(self):
        text_debug = read(CONFIG_DEBUG)
        text_modules = read(CONFIG_MODULES)
        rows = {}
        for _, line in self.lines:
            m = re.match(r"^\|\s*`(CONFIG_OVE_\w+)`\s*\|\s*(\w+)\s*\|\s*([^|]+?)\s*\|",
                         line)
            if m:
                rows[m.group(1)] = m.group(3).strip()
        self.assertTrue(rows, "the Kconfig table is empty")
        for symbol, documented in rows.items():
            name = symbol[len("CONFIG_"):]
            text = None
            for candidate in (text_debug, text_modules):
                if f"config {name}" in candidate:
                    text = candidate
                    break
            self.assertIsNotNone(text, f"{symbol} is documented but not in the Kconfig")
            if symbol == "CONFIG_OVE_SIM":
                # Selected by the board, not by a default line.
                self.assertIn("select OVE_SIM", read(AN500_BOARD),
                              "the document says the an500 selects CONFIG_OVE_SIM")
                continue
            value = normalise_default(kconfig_default(text, name))
            self.assertEqual(documented.replace("`", "").strip(), value,
                             f"the documented default of {symbol} differs from the "
                             f"Kconfig default ({value})")

    # -- markdown ---------------------------------------------------------

    def test_markdown_is_well_formed(self):
        fences = [no for no, line in self.lines if line.strip().startswith("```")]
        self.assertEqual(len(fences) % 2, 0,
                         f"unbalanced code fences in {self.doc_file}: {fences}")
        i = 0
        lines = self.lines
        while i < len(lines):
            line = lines[i][1]
            if line.strip().startswith("|") and i + 1 < len(lines) \
                    and re.match(r"^\s*\|[\s\-:|]+\|\s*$", lines[i + 1][1]):
                columns = line.count("|") - 1
                j = i
                while j < len(lines) and lines[j][1].strip().startswith("|"):
                    self.assertEqual(lines[j][1].count("|") - 1, columns,
                                     f"table row {lines[j][0]} has a different number "
                                     "of columns than its header")
                    j += 1
                i = j
            else:
                i += 1


if __name__ == "__main__":
    # The document path is ours, not unittest's: drop it from argv.
    if _DOC_INDEX is not None:
        sys.argv.pop(_DOC_INDEX + 1)
    unittest.main()
