#!/usr/bin/env python3
"""Deadline-bounded byte-stream server for the hardware hammer benchmark."""

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit


class State:
    lock = threading.Lock()
    total = 0
    connections = 0
    active = 0
    completed = 0
    errors = 0
    started = None
    ended = None
    requested_s = None

    @classmethod
    def reset(cls):
        cls.total = 0
        cls.connections = 0
        cls.completed = 0
        cls.errors = 0
        cls.started = None
        cls.ended = None
        cls.requested_s = None

    @classmethod
    def metrics(cls):
        now = time.monotonic()
        end = now if cls.active and cls.started is not None else cls.ended
        elapsed = 0 if cls.started is None or end is None else end - cls.started
        return {
            "bytes": cls.total,
            "connections": cls.connections,
            "active": cls.active,
            "completed": cls.completed,
            "errors": cls.errors,
            "requested_s": cls.requested_s,
            "elapsed_s": elapsed,
        }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, status, payload, content_type="text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        request = urlsplit(self.path)
        if request.path == "/reset":
            with State.lock:
                if State.active:
                    self.reply(409, b"stream active\n")
                    return
                State.reset()
            self.reply(200, b"ok\n")
            return
        if request.path == "/ready":
            with State.lock:
                ready = State.active > 0
            self.reply(200 if ready else 503, b"ready\n" if ready else b"waiting\n")
            return
        if request.path == "/metrics":
            with State.lock:
                payload = json.dumps(State.metrics()).encode() + b"\n"
            self.reply(200, payload, "application/json")
            return
        if request.path != "/stream":
            self.send_error(404)
            return

        values = parse_qs(request.query).get("seconds", [])
        if not values:
            self.send_error(400, "seconds is required")
            return
        try:
            duration = min(max(int(values[0]), 1), 3600)
        except ValueError:
            self.send_error(400, "invalid seconds")
            return

        chunk = bytes(64 * 1024)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        # SD VACUUM can keep a low-priority guest reader off-CPU for more than
        # one second. Keep the socket bounded without misclassifying that
        # expected backpressure as a failed stream.
        self.connection.settimeout(10.0)

        stream_started = time.monotonic()
        deadline = stream_started + duration
        with State.lock:
            State.connections += 1
            State.active += 1
            State.requested_s = duration
            if State.started is None:
                State.started = stream_started
        completed = False
        failed = False
        try:
            while time.monotonic() < deadline:
                self.wfile.write(chunk)
                with State.lock:
                    State.total += len(chunk)
            completed = True
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            if time.monotonic() >= deadline:
                completed = True
            else:
                failed = True
        finally:
            ended = time.monotonic()
            self.close_connection = True
            with State.lock:
                State.active -= 1
                State.ended = ended
                if completed:
                    State.completed += 1
                if failed:
                    State.errors += 1

    def log_message(self, format, *args):
        pass


ThreadingHTTPServer(("0.0.0.0", 8082), Handler).serve_forever()
