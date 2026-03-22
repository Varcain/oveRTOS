#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
QEMU shared-memory framebuffer viewer for oveRTOS.

Reads the framebuffer from /dev/shm/ove-fb (written by the QEMU guest
via semihosting file I/O) and renders it in an SDL2 window.

Usage:
    python3 qemu-display-viewer.py [--headless] [--once]

Options:
    --headless  Validate framebuffer without opening a window (CI mode).
    --once      Render one frame and exit (screenshot mode).

The framebuffer format is defined by a header struct:

    struct qemu_fb_header {
        uint32_t magic;       // 0x42465854 ("TXFB" little-endian)
        uint16_t width;
        uint16_t height;
        uint32_t format;      // 0 = RGB565
        uint32_t dirty;       // Non-zero when guest has flushed
    };
    // Followed by width * height * 2 bytes of RGB565 pixel data.
"""

import argparse
import mmap
import os
import signal
import struct
import sys
import time

FB_PATH = "/dev/shm/ove-fb"
FB_MAGIC = 0x42465854   # "TXFB" in little-endian
HEADER_SIZE = 16         # magic(4) + width(2) + height(2) + format(4) + dirty(4)
FORMAT_RGB565 = 0

# Default dimensions (from board.yaml) used before guest writes header
DEFAULT_WIDTH = 480
DEFAULT_HEIGHT = 272


def read_header(mm):
    """Read and validate the framebuffer header."""
    raw = mm[:HEADER_SIZE]
    magic, width, height, fmt, dirty = struct.unpack("<IHHII", raw)
    return magic, width, height, fmt, dirty


def rgb565_to_rgb888(data, width, height):
    """Convert RGB565 pixel data to RGB888 bytes for SDL2."""
    out = bytearray(width * height * 3)
    for i in range(width * height):
        pixel = data[i * 2] | (data[i * 2 + 1] << 8)
        r = ((pixel >> 11) & 0x1F) << 3
        g = ((pixel >> 5) & 0x3F) << 2
        b = (pixel & 0x1F) << 3
        out[i * 3] = r
        out[i * 3 + 1] = g
        out[i * 3 + 2] = b
    return bytes(out)


def run_headless(once=False):
    """Validate framebuffer in headless mode (no SDL2 needed)."""
    if not os.path.exists(FB_PATH):
        print(f"Waiting for {FB_PATH}...")
        while not os.path.exists(FB_PATH):
            time.sleep(0.1)

    with open(FB_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        frames = 0
        try:
            while True:
                magic, width, height, fmt, dirty = read_header(mm)
                if magic == FB_MAGIC:
                    if dirty:
                        frames += 1
                        print(f"Frame {frames}: {width}x{height} "
                              f"format={fmt} dirty={dirty}")
                        # Clear dirty flag
                        struct.pack_into("<I", mm, 12, 0)
                        if once:
                            print("Framebuffer valid.")
                            return 0
                else:
                    if once:
                        print(f"No valid framebuffer header (magic=0x{magic:08x})")
                        return 1
                time.sleep(1.0 / 30)  # ~30 fps poll
        except KeyboardInterrupt:
            print(f"\n{frames} frames received.")
        finally:
            mm.close()
    return 0


def run_viewer(once=False):
    """Render framebuffer in an SDL2 window."""
    try:
        import sdl2
        import sdl2.ext
    except ImportError:
        print("SDL2 not available. Install PySDL2:")
        print("  pip install pysdl2 pysdl2-dll")
        print("Falling back to headless mode.")
        return run_headless(once)

    if not os.path.exists(FB_PATH):
        print(f"Waiting for {FB_PATH}...")
        while not os.path.exists(FB_PATH):
            time.sleep(0.1)

    with open(FB_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)

        # Open window immediately with default dimensions
        width, height = DEFAULT_WIDTH, DEFAULT_HEIGHT
        header_valid = False

        # Prevent compositor bypass — avoids black flash on WSL2/WSLg
        sdl2.SDL_SetHint(sdl2.SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, b"0")
        sdl2.SDL_Init(sdl2.SDL_INIT_VIDEO)
        window = sdl2.SDL_CreateWindow(
            b"oveRTOS QEMU Display - waiting for framebuffer",
            sdl2.SDL_WINDOWPOS_CENTERED,
            sdl2.SDL_WINDOWPOS_CENTERED,
            width * 2, height * 2,  # 2x scale for visibility
            sdl2.SDL_WINDOW_SHOWN,
        )
        renderer = sdl2.SDL_CreateRenderer(
            window, -1, sdl2.SDL_RENDERER_ACCELERATED
        )
        texture = sdl2.SDL_CreateTexture(
            renderer,
            sdl2.SDL_PIXELFORMAT_RGB24,
            sdl2.SDL_TEXTUREACCESS_STREAMING,
            width, height,
        )

        # Show dark gray background while waiting
        sdl2.SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255)
        sdl2.SDL_RenderClear(renderer)
        sdl2.SDL_RenderPresent(renderer)

        event = sdl2.SDL_Event()
        running = True
        frames = 0

        def _stop(sig, frame):
            nonlocal running
            running = False
        signal.signal(signal.SIGTERM, _stop)

        try:
            while running:
                while sdl2.SDL_PollEvent(event):
                    if event.type == sdl2.SDL_QUIT:
                        running = False
                    elif (event.type == sdl2.SDL_KEYDOWN and
                          event.key.keysym.sym == sdl2.SDLK_ESCAPE):
                        running = False

                magic, w, h, fmt, dirty = read_header(mm)

                if magic == FB_MAGIC and w > 0 and h > 0:
                    # Header became valid — resize if dimensions changed
                    if not header_valid or w != width or h != height:
                        header_valid = True
                        width, height = w, h
                        sdl2.SDL_SetWindowTitle(
                            window,
                            f"oveRTOS QEMU Display - {width}x{height}".encode()
                        )
                        sdl2.SDL_SetWindowSize(window, width * 2, height * 2)
                        sdl2.SDL_DestroyTexture(texture)
                        texture = sdl2.SDL_CreateTexture(
                            renderer,
                            sdl2.SDL_PIXELFORMAT_RGB24,
                            sdl2.SDL_TEXTUREACCESS_STREAMING,
                            width, height,
                        )

                    if dirty:
                        pixel_data = mm[HEADER_SIZE:HEADER_SIZE + width * height * 2]
                        rgb = rgb565_to_rgb888(pixel_data, width, height)

                        sdl2.SDL_UpdateTexture(texture, None, rgb, width * 3)
                        sdl2.SDL_RenderClear(renderer)
                        sdl2.SDL_RenderCopy(renderer, texture, None, None)
                        sdl2.SDL_RenderPresent(renderer)

                        # Clear dirty flag
                        struct.pack_into("<I", mm, 12, 0)
                        frames += 1

                        if once:
                            running = False

                sdl2.SDL_Delay(16)  # ~60fps cap
        except KeyboardInterrupt:
            pass
        finally:
            print(f"{frames} frames rendered.")
            sdl2.SDL_DestroyTexture(texture)
            sdl2.SDL_DestroyRenderer(renderer)
            sdl2.SDL_DestroyWindow(window)
            sdl2.SDL_Quit()
            mm.close()

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="QEMU shared-memory framebuffer viewer for oveRTOS"
    )
    parser.add_argument(
        "--headless", action="store_true",
        help="Validate framebuffer without opening a window"
    )
    parser.add_argument(
        "--once", action="store_true",
        help="Render one frame and exit"
    )
    args = parser.parse_args()

    if args.headless:
        sys.exit(run_headless(args.once))
    else:
        sys.exit(run_viewer(args.once))


if __name__ == "__main__":
    main()
