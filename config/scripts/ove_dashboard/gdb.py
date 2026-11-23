# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import json
import os
import struct
import subprocess
import sys
import threading
import time
import traceback

from pygdbmi.gdbmiparser import parse_response

from . import frames, sources
from .protocol import (
    DBG_CMD_BACKTRACE,
    DBG_CMD_BP_CLEAR,
    DBG_CMD_BP_SET,
    DBG_CMD_CONTINUE,
    DBG_CMD_DISASSEMBLE,
    DBG_CMD_PAUSE,
    DBG_CMD_REGISTERS,
    DBG_CMD_RESET,
    DBG_CMD_SOURCE,
    DBG_CMD_STEP_INTO,
    DBG_CMD_STEP_OUT,
    DBG_CMD_STEP_OVER,
    DBG_RESP_BACKTRACE,
    DBG_RESP_BREAKPOINT,
    DBG_RESP_DISASSEMBLY,
    DBG_RESP_REGISTERS,
    DBG_RESP_SOURCE,
    DBG_RESP_STATE,
    FRAME_DEBUG_RESP,
)

# ── GDB/MI Controller ────────────────────────────────────────────────

MAX_DISASSEMBLE_COUNT = 4096

class GdbController:
    """Drives a GDB/MI subprocess to debug the target."""

    def __init__(self, toolchain, elf_path, gdb_port):
        self.toolchain = toolchain
        self.elf_path = elf_path
        self.gdb_port = gdb_port
        self.proc = None
        self._token = 0
        self._lock = threading.Lock()
        self._reader_thread = None
        self._pending = {}  # token → threading.Event + result
        self._source_cache = {}  # file path → list of lines
        self._last_interrupt = 0  # monotonic time of last interrupt
        self._reg_names = None  # cached register names from GDB

    def start(self):
        """Launch GDB subprocess and connect to target."""
        cmd = [self.toolchain, "--interpreter=mi3", "-q"]
        if self.elf_path:
            cmd += [self.elf_path]
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)

        self._reader_thread = threading.Thread(
            target=self._reader_loop, daemon=True)
        self._reader_thread.start()

        # Wait for GDB startup
        time.sleep(0.3)

        self._send_mi_sync(
            f"-target-select remote 127.0.0.1:{self.gdb_port}")


        # Fetch register names once (architecture-specific).
        result = self._send_mi_sync("-data-list-register-names")
        if result and isinstance(result.get("data"), dict):
            names = result["data"].get("register-names", [])
            if isinstance(names, list):
                self._reg_names = names

        # Connecting halts the target — resume it immediately so the
        # firmware keeps running.  The user can pause from the dashboard.
        time.sleep(0.1)
        self._send_mi("-exec-continue")

        self._broadcast_state("running", None, None, None)

    def stop(self):
        if self.proc:
            try:
                self.proc.stdin.write("-gdb-exit\n")
                self.proc.stdin.flush()
            except Exception:
                pass
            self.proc.terminate()
            self.proc = None

    def handle_debug_cmd(self, data):
        """Handle a FRAME_DEBUG_CMD from the dashboard."""
        if len(data) < 1 or not self.proc:
            return
        cmd_type = data[0]
        payload = data[1:]

        if cmd_type == DBG_CMD_CONTINUE:
            self._send_mi("-exec-continue --all")
            self._broadcast_state("running", None, None, None)
        elif cmd_type == DBG_CMD_PAUSE:
            # Debounce: ignore rapid clicks (< 1s apart) to prevent
            # multiple breaks from killing the remote target.
            import signal as _signal
            now = time.monotonic()
            if now - self._last_interrupt < 1.0:
                return
            self._last_interrupt = now
            # SIGINT to GDB process — GDB catches it and sends a break
            # character to the remote gdbstub.  -exec-interrupt alone
            # doesn't reliably work with QEMU's gdbstub.
            try:
                os.kill(self.proc.pid, _signal.SIGINT)
            except (OSError, ProcessLookupError):
                pass
        elif cmd_type == DBG_CMD_STEP_OVER:
            self._send_mi("-exec-next")
        elif cmd_type == DBG_CMD_STEP_INTO:
            self._send_mi("-exec-step")
        elif cmd_type == DBG_CMD_STEP_OUT:
            self._send_mi("-exec-finish")
        elif cmd_type == DBG_CMD_RESET:
            self._send_mi("monitor system_reset")
            time.sleep(0.1)
            self._send_mi("-exec-continue")
            self._broadcast_state("running", None, None, None)
        elif cmd_type == DBG_CMD_BACKTRACE:
            result = self._send_mi_sync("-stack-list-frames")
            if result:
                self._send_debug_resp(DBG_RESP_BACKTRACE, result)
        elif cmd_type == DBG_CMD_REGISTERS:
            result = self._send_mi_sync(
                "-data-list-register-values x")
            if result:
                self._send_debug_resp(
                    DBG_RESP_REGISTERS, self._enrich_registers(result))
        elif cmd_type == DBG_CMD_DISASSEMBLE:
            if len(payload) >= 8:
                addr = struct.unpack_from("<I", payload, 0)[0]
                count = struct.unpack_from("<I", payload, 4)[0]
                end_addr = addr + min(count, MAX_DISASSEMBLE_COUNT) * 4
                result = self._send_mi_sync(
                    f"-data-disassemble -s 0x{addr:x} -e 0x{end_addr:x} -- 1")
                if result:
                    self._send_debug_resp(DBG_RESP_DISASSEMBLY, result)
        elif cmd_type == DBG_CMD_BP_SET:
            if len(payload) >= 6:
                path_len = struct.unpack_from("<H", payload, 0)[0]
                if path_len > len(payload) - 6:
                    return
                path = payload[2:2 + path_len].decode("utf-8", "replace")
                if any(ord(ch) < 32 or 127 <= ord(ch) <= 159 for ch in path):
                    return
                line = struct.unpack_from("<I", payload, 2 + path_len)[0]
                escaped_path = path.replace("\\", "\\\\").replace('"', '\\"')
                # A linespec can interpret a leading '*' as an address expression.
                result = self._send_mi_sync(
                    f'-break-insert --source "{escaped_path}" --line {line}')
                if result:
                    self._send_debug_resp(DBG_RESP_BREAKPOINT, result)
        elif cmd_type == DBG_CMD_BP_CLEAR:
            if len(payload) >= 4:
                bp_id = struct.unpack_from("<I", payload, 0)[0]
                self._send_mi(f"-break-delete {bp_id}")
        elif cmd_type == DBG_CMD_SOURCE:
            if len(payload) >= 10:
                path_len = struct.unpack_from("<H", payload, 0)[0]
                if path_len > len(payload) - 10:
                    return
                path = payload[2:2 + path_len].decode("utf-8", "replace")
                line = struct.unpack_from("<I", payload, 2 + path_len)[0]
                ctx_lines = struct.unpack_from(
                    "<I", payload, 6 + path_len)[0]
                self._handle_source_request(path, line, ctx_lines)

    def _send_mi(self, cmd):
        """Send a GDB/MI command (fire-and-forget)."""
        if not self.proc:
            return
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def _send_mi_sync(self, cmd, timeout=2.0):
        """Send a GDB/MI command and wait for the result record."""
        if not self.proc:
            return None
        with self._lock:
            self._token += 1
            token = self._token
        evt = threading.Event()
        self._pending[token] = {"event": evt, "result": None}
        try:
            self.proc.stdin.write(f"{token}{cmd}\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            del self._pending[token]
            return None
        evt.wait(timeout=timeout)
        entry = self._pending.pop(token, None)
        return entry["result"] if entry else None

    def _reader_loop(self):
        """Read GDB/MI output and dispatch async events + result records."""
        while self.proc:
            try:
                line = self.proc.stdout.readline()
                if not line.endswith("\n"):
                    break  # EOF or an incomplete final record
                line = line.rstrip("\n\r")
                if not line or line == "(gdb)":
                    continue
                # Log only *stopped and *running,thread-id="all"
                # (skip per-thread *running from short-lived threads)
                if line.startswith("*stopped") or \
                        line == '*running,thread-id="all"':
                    short = line[:120] + "..." if len(line) > 120 else line
                    try:
                        sys.stderr.write(f"[gdb] {short}\n")
                        sys.stderr.flush()
                    except Exception:
                        pass
                self._handle_mi_output(line)
            except Exception:
                try:
                    sys.stderr.write("[gdb] reader error\n")
                    traceback.print_exc(file=sys.stderr)
                except Exception:
                    pass

    def _handle_mi_output(self, line):
        record = parse_response(line)
        message = record["message"]
        if record["type"] == "result":
            data = record["payload"] or {}
            entry = self._pending.get(record["token"])
            if entry:
                entry["result"] = {"status": message, "data": data}
                entry["event"].set()
            # ^error from any command — log it
            if message == "error":
                msg = data.get("msg", "")
                print(f"[gdb] ERROR: {msg}", flush=True)
            return

        # pygdbmi groups exec (*) and notify (=) records under "notify".
        if record["type"] != "notify" or not line.startswith("*"):
            return
        info = record["payload"] or {}
        if message == "stopped":
            # Dispatch to a worker thread — _handle_stopped uses
            # _send_mi_sync which would deadlock in the reader loop.
            threading.Thread(
                target=self._handle_stopped, args=(info,),
                daemon=True).start()
        elif message == "running" and info.get("thread-id") == "all":
            # Only broadcast on "all threads running" — ignore
            # per-thread *running from short-lived glibc threads.
            self._broadcast_state("running", None, None, None)

    def _handle_stopped(self, info):
        """Broadcast a parsed *stopped event to the dashboard."""
        reason = info.get("reason", "unknown")
        frame = info.get("frame", {})
        func = frame.get("func", "??")
        src_file = frame.get("fullname", frame.get("file", ""))
        src_line = int(frame.get("line", "0") or "0")
        addr = frame.get("addr", "0x0")

        self._broadcast_state(
            "stopped", reason, src_file, src_line, addr=addr)

        # Auto-fetch source context for the current location
        if src_file and src_line:
            self._handle_source_request(src_file, src_line, 20)
        else:
            # No source — send a placeholder so the UI shows something
            resp = {
                "file": "",
                "start_line": 0,
                "current_line": 0,
                "lines": [f"  No source available for {func} ({addr})"],
            }
            self._send_debug_resp(
                DBG_RESP_SOURCE,
                {"status": "done", "data": resp})

        # Auto-fetch backtrace
        result = self._send_mi_sync("-stack-list-frames", timeout=3.0)
        if result:
            self._send_debug_resp(DBG_RESP_BACKTRACE, result)

        # Auto-fetch registers
        result = self._send_mi_sync(
            "-data-list-register-values x", timeout=3.0)
        if result:
            self._send_debug_resp(
                DBG_RESP_REGISTERS, self._enrich_registers(result))

        # Auto-fetch disassembly around current PC
        if addr:
            try:
                pc = int(addr, 16)
                start = max(0, pc - 32)
                end = pc + 64
                result = self._send_mi_sync(
                    f"-data-disassemble -s 0x{start:x} -e 0x{end:x} -- 1",
                    timeout=3.0)
                if result:
                    self._send_debug_resp(DBG_RESP_DISASSEMBLY, result)
            except (ValueError, TypeError):
                pass

    def _enrich_registers(self, result):
        """Add register names to register-values result."""
        if not result or not self._reg_names:
            return result
        data = result.get("data", {})
        regs = data.get("register-values", [])
        if not isinstance(regs, list):
            return result
        for r in regs:
            num = int(r.get("number", -1))
            if 0 <= num < len(self._reg_names):
                name = self._reg_names[num]
                if name:  # skip empty names (unused registers)
                    r["name"] = name
        # Filter out registers with empty names (GDB returns many
        # unused slots like "" for internal pseudo-registers).
        data["register-values"] = [
            r for r in regs
            if r.get("name") or (
                int(r.get("number", -1)) < len(self._reg_names)
                and self._reg_names[int(r.get("number", -1))]
            )
        ]
        return result

    def _broadcast_state(self, exec_state, reason, src_file, src_line,
                         addr=None):
        """Send debug state change to all dashboard clients."""
        state = {
            "state": exec_state,
            "reason": reason,
            "file": src_file,
            "line": src_line,
            "addr": addr,
        }
        self._send_debug_resp(DBG_RESP_STATE, {"status": "done", "data": state})

    def _handle_source_request(self, path, line, ctx_lines):
        """Read source file and send to dashboard."""
        lines = []
        try:
            source_path = os.path.realpath(path)
            if os.path.commonpath((sources.OVE_DIR, source_path)) == sources.OVE_DIR:
                if source_path not in self._source_cache:
                    with open(source_path, "r", errors="replace") as f:
                        self._source_cache[source_path] = f.readlines()
                lines = self._source_cache[source_path]
        except (OSError, ValueError):
            pass

        start = max(0, line - ctx_lines - 1)
        end = min(len(lines), line + ctx_lines)
        snippet = [text.rstrip("\n\r") for text in lines[start:end]]

        resp = {
            "file": path,
            "start_line": start + 1,
            "current_line": line,
            "lines": snippet,
        }
        self._send_debug_resp(DBG_RESP_SOURCE, {"status": "done", "data": resp})

    def _send_debug_resp(self, resp_type, data):
        """Send a FRAME_DEBUG_RESP to all WebSocket clients."""
        try:
            json_bytes = json.dumps(data, default=str).encode("utf-8")
        except Exception:
            return
        payload = struct.pack("<IB", FRAME_DEBUG_RESP, resp_type) + json_bytes
        frames.post_frame(payload)


# Global GDB controller instance (set in main if --gdb-port)
gdb_controller = [None]
