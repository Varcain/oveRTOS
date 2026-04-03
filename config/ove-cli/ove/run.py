# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""QEMU launch, display viewer, and WASM browser server."""

import os
import shutil
import subprocess
import sys
import webbrowser

from .workspace import Workspace


def _get_config_bool(ws, key):
    return ws.config.get(key) is True


def _run_wasm(ws, args):
    """Serve WASM build in browser with COOP/COEP headers."""
    build_dir = os.path.join(ws.build_dir, "firmware")
    wasm_html = os.path.join(build_dir, "ove_wasm.html")
    wasm_js = os.path.join(build_dir, "ove_wasm.js")
    wasm_bin = os.path.join(build_dir, "ove_wasm.wasm")

    if not os.path.isfile(wasm_bin):
        print("Error: WASM build not found. Run 'ove build' first.")
        sys.exit(1)

    # Assemble serve directory
    serve_dir = os.path.join(ws.workspace_dir, "serve")
    os.makedirs(serve_dir, exist_ok=True)

    shutil.copy2(wasm_html, os.path.join(serve_dir, "index.html"))
    shutil.copy2(wasm_js, serve_dir)
    shutil.copy2(wasm_bin, serve_dir)
    # Copy pthread worker JS if present
    wasm_worker = os.path.join(build_dir, "ove_wasm.worker.js")
    if os.path.isfile(wasm_worker):
        shutil.copy2(wasm_worker, serve_dir)

    # Copy dashboard assets
    dash_dir = os.path.join(ws.ove_dir, "sim", "dashboard")
    for f in ("app.js", "style.css"):
        src = os.path.join(dash_dir, f)
        if os.path.isfile(src):
            shutil.copy2(src, serve_dir)

    # Write coi-serviceworker.js for SharedArrayBuffer headers
    coi_path = os.path.join(serve_dir, "coi-serviceworker.js")
    if not os.path.isfile(coi_path):
        _write_coi_serviceworker(coi_path)

    port = 8080
    headless = hasattr(args, "headless") and args.headless

    print(f"=== Serving WASM at http://localhost:{port} ===")
    print(f"  Files: {serve_dir}")

    if not headless:
        # Open browser after a short delay
        import threading
        def open_browser():
            import time
            time.sleep(1)
            webbrowser.open(f"http://localhost:{port}")
        threading.Thread(target=open_browser, daemon=True).start()

    # Start HTTP server with COOP/COEP headers
    _serve_with_coop(serve_dir, port)


def _write_coi_serviceworker(path):
    """Write a minimal COOP/COEP service worker for SharedArrayBuffer."""
    # Based on https://github.com/nicbarker/coi-serviceworker
    content = r'''/* coi-serviceworker - Adds COOP/COEP headers for SharedArrayBuffer */
if (typeof window === 'undefined') {
  self.addEventListener("install", () => self.skipWaiting());
  self.addEventListener("activate", e => e.waitUntil(self.clients.claim()));
  self.addEventListener("fetch", e => {
    if (e.request.cache === "only-if-cached" && e.request.mode !== "same-origin") return;
    e.respondWith(fetch(e.request).then(r => {
      if (r.status === 0) return r;
      const h = new Headers(r.headers);
      h.set("Cross-Origin-Embedder-Policy", "require-corp");
      h.set("Cross-Origin-Opener-Policy", "same-origin");
      return new Response(r.body, {status: r.status, statusText: r.statusText, headers: h});
    }).catch(e => console.error(e)));
  });
} else {
  (async () => {
    if (window.crossOriginIsolated !== false) return;
    const r = await navigator.serviceWorker.register(window.document.currentScript.src);
    if (r.active && !navigator.serviceWorker.controller) window.location.reload();
    else if (!r.active) {
      r.addEventListener("updatefound", () =>
        r.installing.addEventListener("statechange", () => {
          if (r.installing.state === "activated") window.location.reload();
        }));
    }
  })();
}
'''
    with open(path, "w") as f:
        f.write(content)


def _serve_with_coop(directory, port):
    """HTTP server that adds COOP/COEP headers for SharedArrayBuffer."""
    from http.server import HTTPServer, SimpleHTTPRequestHandler

    class COOPHandler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=directory, **kw)

        def end_headers(self):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            super().end_headers()

        def log_message(self, fmt, *a):
            # Suppress per-request logging
            pass

    server = HTTPServer(("127.0.0.1", port), COOPHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


def cmd_run(args):
    """CLI entry point for 'ove run'."""
    ws = Workspace()
    ws.require_config()

    rtos = ws.rtos
    if not rtos:
        print("Error: no RTOS selected in .config")
        sys.exit(1)

    # WASM board: serve in browser
    if _get_config_bool(ws, "CONFIG_OVE_BOARD_WASM"):
        _run_wasm(ws, args)
        return

    firmware = os.path.join(ws.images_dir, "firmware.elf")
    if rtos == "posix":
        posix_bin = os.path.join(ws.images_dir, "ove_posix")
        if not os.path.isfile(posix_bin):
            print("Error: POSIX binary not found. Run 'ove build' first.")
            sys.exit(1)
        # Export OVE_DIR so the sim dashboard can find its static assets.
        os.environ["OVE_DIR"] = ws.ove_dir
        extra = args.extra if hasattr(args, "extra") else []
        os.execv(posix_bin, [posix_bin] + extra)

    # QEMU or hardware
    qemu_script = os.path.join(ws.board_dir, "qemu-run.sh")
    if os.path.isfile(qemu_script):
        if not os.path.isfile(firmware):
            print("Error: firmware.elf not found. Run 'ove build' first.")
            sys.exit(1)
        cmd = [qemu_script, firmware]
        if hasattr(args, "headless") and args.headless:
            cmd.append("--headless")
        if hasattr(args, "extra"):
            cmd.extend(args.extra)
        os.execv(qemu_script, cmd)
    else:
        print(f"Error: no run method for board '{ws.board_name}' "
              f"with RTOS '{rtos}'")
        print("Use 'ove flash' for hardware targets.")
        sys.exit(1)


def cmd_flash(args):
    """CLI entry point for 'ove flash'."""
    ws = Workspace()
    ws.require_config()

    rtos = ws.rtos
    firmware = os.path.join(ws.images_dir, "firmware.elf")

    if rtos == "posix":
        print("POSIX backend doesn't need flashing. Use 'ove run'.")
        sys.exit(1)

    if rtos == "zephyr":
        west = os.path.join(ws.venv_dir, "bin", "west")
        env = ws.toolchain_env()
        zephyr_ws = os.path.join(ws.ws_dl_dir, "zephyr-workspace", "zephyr")
        if os.path.isdir(zephyr_ws):
            env["ZEPHYR_BASE"] = zephyr_ws
        fw_build = os.path.join(ws.build_dir, "firmware")
        print("=== Flashing Zephyr firmware ===")
        subprocess.run(
            [west, "flash", "-d", fw_build, "--runner", "openocd"],
            env=env)
        return

    # FreeRTOS or NuttX: use board flash.sh
    flash_sh = os.path.join(ws.board_dir, rtos, "flash.sh")
    if os.path.isfile(flash_sh):
        print(f"=== Flashing {rtos} firmware ===")
        os.execv(flash_sh, [flash_sh, firmware])
    else:
        print(f"Error: flash.sh not found for {rtos}")
        sys.exit(1)
