#!/usr/bin/env python3

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""
QEMU shared-memory viewer for oveRTOS (display + audio).

Reads the framebuffer from /dev/shm/ove-fb and audio ringbuffer from
/dev/shm/ove-audio (both written by the QEMU guest via semihosting)
and renders display in an SDL2 window while playing/capturing audio.

Usage:
    python3 qemu-display-viewer.py [--headless] [--once] [--no-audio]
                                   [--dump-wav PATH]
"""

import argparse
import ctypes
import mmap
import os
import signal
import struct
import sys
import time

FB_PATH = "/dev/shm/ove-fb"
FB_MAGIC = 0x42465854   # "TXFB" in little-endian
FB_HEADER_SIZE = 16
FORMAT_RGB565 = 0

AUDIO_PATH = "/dev/shm/ove-audio"
AUDIO_SHM_MAGIC = 0x4F564155   # "OVAU"
AUDIO_HDR_SIZE = 64
AUDIO_OFF_MAGIC = 0
AUDIO_OFF_SAMPLE_RATE = 4
AUDIO_OFF_CHANNELS = 8
AUDIO_OFF_BIT_DEPTH = 10
AUDIO_OFF_RING_SIZE = 16
AUDIO_OFF_OUT_WPOS = 20
AUDIO_OFF_OUT_RPOS = 24
AUDIO_OFF_IN_WPOS = 28
AUDIO_OFF_IN_RPOS = 32

DEFAULT_WIDTH = 480
DEFAULT_HEIGHT = 272


def read_header(mm):
    raw = mm[:FB_HEADER_SIZE]
    magic, width, height, fmt, dirty = struct.unpack("<IHHII", raw)
    return magic, width, height, fmt, dirty


def rgb565_to_rgb888(data, width, height):
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


# ── Audio bridge (push model — no callbacks) ─────────────────────────

class AudioBridge:
    """Moves audio between the shm ringbuffer and SDL2 using the push
    API (SDL_QueueAudio / SDL_DequeueAudio).  The main loop calls
    pump() each iteration to transfer data.  pread() is used instead
    of mmap slicing for output reads to avoid coherency issues on WSL2."""

    def __init__(self, mm, sdl2_mod, fd=-1):
        self.mm = mm
        self.sdl2 = sdl2_mod
        self._fd = fd
        self.out_dev = None
        self.in_dev = None
        self.ring_size = 0
        self.out_ring_off = 0
        self.in_ring_off = 0
        self._out_rpos = 0
        self._in_wpos = 0
        self._frame_bytes = 0
        self._sdl_target = 0
        self._wav_file = None

    def start(self):
        magic = struct.unpack_from("<I", self.mm, AUDIO_OFF_MAGIC)[0]
        if magic != AUDIO_SHM_MAGIC:
            return False

        sr = struct.unpack_from("<I", self.mm, AUDIO_OFF_SAMPLE_RATE)[0]
        ch = struct.unpack_from("<H", self.mm, AUDIO_OFF_CHANNELS)[0]
        bd = struct.unpack_from("<H", self.mm, AUDIO_OFF_BIT_DEPTH)[0]
        rs = struct.unpack_from("<I", self.mm, AUDIO_OFF_RING_SIZE)[0]

        if sr == 0 or ch == 0 or rs == 0:
            return False

        self.ring_size = rs
        self.out_ring_off = AUDIO_HDR_SIZE
        self.in_ring_off = AUDIO_HDR_SIZE + rs
        self._frame_bytes = ch * (bd // 8)
        self._sdl_target = sr * self._frame_bytes // 2  # ~500ms

        sdl2 = self.sdl2
        null_cb = sdl2.SDL_AudioCallback()

        want_out = sdl2.SDL_AudioSpec(
            freq=sr, aformat=sdl2.AUDIO_S16LSB, channels=ch,
            samples=2048, callback=null_cb,
        )
        got_out = sdl2.SDL_AudioSpec(
            freq=0, aformat=0, channels=0, samples=0,
            callback=null_cb,
        )
        self.out_dev = sdl2.SDL_OpenAudioDevice(
            None, 0, want_out, ctypes.byref(got_out), 0
        )
        if self.out_dev and self.out_dev >= 2:
            sdl2.SDL_PauseAudioDevice(self.out_dev, 0)
            print(f"Audio output: {sr} Hz, {ch} ch, {bd}-bit")
        else:
            self.out_dev = None

        want_in = sdl2.SDL_AudioSpec(
            freq=sr, aformat=sdl2.AUDIO_S16LSB, channels=ch,
            samples=2048, callback=null_cb,
        )
        got_in = sdl2.SDL_AudioSpec(
            freq=0, aformat=0, channels=0, samples=0,
            callback=null_cb,
        )
        self.in_dev = sdl2.SDL_OpenAudioDevice(
            None, 1, want_in, ctypes.byref(got_in), 0
        )
        if self.in_dev and self.in_dev >= 2:
            sdl2.SDL_PauseAudioDevice(self.in_dev, 0)
        else:
            self.in_dev = None

        return True

    def pump(self):
        self._pump_output()
        self._pump_input()

    def _pump_output(self):
        if not self.out_dev:
            return

        sdl2 = self.sdl2
        mm = self.mm
        rs = self.ring_size
        mask = rs - 1

        wpos = struct.unpack_from("<I", mm, AUDIO_OFF_OUT_WPOS)[0]
        rpos = self._out_rpos

        avail = (wpos - rpos) & 0xFFFFFFFF
        if avail > rs:
            rpos = wpos - rs
            avail = rs

        sdl_q = sdl2.SDL_GetQueuedAudioSize(self.out_dev)
        if sdl_q >= self._sdl_target:
            self._out_rpos = rpos
            struct.pack_into("<I", mm, AUDIO_OFF_OUT_RPOS, rpos)
            return

        to_read = min(avail, self._sdl_target - sdl_q)
        if to_read == 0:
            return
        to_read = (to_read // self._frame_bytes) * self._frame_bytes

        pos_in_ring = rpos & mask
        first = min(rs - pos_in_ring, to_read)

        if first >= to_read:
            data = os.pread(self._fd, to_read,
                            self.out_ring_off + pos_in_ring)
        else:
            rest = to_read - first
            data = (os.pread(self._fd, first,
                             self.out_ring_off + pos_in_ring) +
                    os.pread(self._fd, rest, self.out_ring_off))

        sdl2.SDL_QueueAudio(self.out_dev, data, len(data))

        rpos = (rpos + to_read) & 0xFFFFFFFF
        self._out_rpos = rpos
        struct.pack_into("<I", mm, AUDIO_OFF_OUT_RPOS, rpos)

        if self._wav_file:
            self._wav_file.writeframes(data)

    def _pump_input(self):
        # Capture is available but rarely needed on QEMU (no real mic).
        # SDL2 DequeueAudio provides the data; we write it to the
        # input ring so the guest can read it via semihosting.
        if not self.in_dev:
            return

        sdl2 = self.sdl2
        mm = self.mm
        rs = self.ring_size
        mask = rs - 1

        captured = sdl2.SDL_GetQueuedAudioSize(self.in_dev)
        if captured == 0:
            return

        rpos = struct.unpack_from("<I", mm, AUDIO_OFF_IN_RPOS)[0]
        wpos = self._in_wpos
        used = (wpos - rpos) & 0xFFFFFFFF
        if used > rs:
            used = rs
        free = rs - used
        to_write = min(captured, free)
        to_write = (to_write // self._frame_bytes) * self._frame_bytes
        if to_write == 0:
            return

        buf = (ctypes.c_uint8 * to_write)()
        got = sdl2.SDL_DequeueAudio(self.in_dev, buf, to_write)
        if got <= 0:
            return

        got = (got // self._frame_bytes) * self._frame_bytes
        if got == 0:
            return
        data = bytes(buf[:got])

        pos_in_ring = wpos & mask
        first = min(rs - pos_in_ring, got)

        # Clamp writes to ring boundaries
        end1 = self.in_ring_off + pos_in_ring
        if first >= got:
            if end1 + got <= len(mm):
                mm[end1:end1 + got] = data
        else:
            end2 = self.in_ring_off + first
            if end1 + first <= len(mm) and self.in_ring_off + got - first <= len(mm):
                mm[end1:end1 + first] = data[:first]
                rest = got - first
                mm[self.in_ring_off:self.in_ring_off + rest] = data[first:first + rest]

        wpos = (wpos + got) & 0xFFFFFFFF
        self._in_wpos = wpos
        struct.pack_into("<I", mm, AUDIO_OFF_IN_WPOS, wpos)

    def dump_wav(self, path):
        import wave
        self._wav_file = wave.open(path, 'wb')
        self._wav_file.setnchannels(self._frame_bytes // 2)
        self._wav_file.setsampwidth(2)
        sr = struct.unpack_from("<I", self.mm, AUDIO_OFF_SAMPLE_RATE)[0]
        self._wav_file.setframerate(sr)

    def stop(self):
        if self._wav_file:
            self._wav_file.close()
            self._wav_file = None
        sdl2 = self.sdl2
        if self.out_dev:
            sdl2.SDL_CloseAudioDevice(self.out_dev)
        if self.in_dev:
            sdl2.SDL_CloseAudioDevice(self.in_dev)


# ── Viewer modes ─────────────────────────────────────────────────────

def run_headless(once=False):
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
                        struct.pack_into("<I", mm, 12, 0)
                        if once:
                            print("Framebuffer valid.")
                            return 0
                else:
                    if once:
                        print(f"No valid header (magic=0x{magic:08x})")
                        return 1
                time.sleep(1.0 / 30)
        except KeyboardInterrupt:
            print(f"\n{frames} frames received.")
        finally:
            mm.close()
    return 0


def run_viewer(once=False, enable_audio=True, dump_wav=None):
    try:
        import sdl2
        import sdl2.ext
    except ImportError:
        print("SDL2 not available. Install PySDL2:")
        print("  pip install pysdl2 pysdl2-dll")
        return run_headless(once)

    if not os.path.exists(FB_PATH):
        print(f"Waiting for {FB_PATH}...")
        while not os.path.exists(FB_PATH):
            time.sleep(0.1)

    audio_bridge = None
    audio_mm = None
    audio_file = None
    audio_started = False

    init_flags = sdl2.SDL_INIT_VIDEO
    if enable_audio:
        init_flags |= sdl2.SDL_INIT_AUDIO

    with open(FB_PATH, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 0)
        width, height = DEFAULT_WIDTH, DEFAULT_HEIGHT
        header_valid = False

        sdl2.SDL_SetHint(
            sdl2.SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, b"0")
        sdl2.SDL_Init(init_flags)
        window = sdl2.SDL_CreateWindow(
            b"oveRTOS QEMU Display - waiting for framebuffer",
            sdl2.SDL_WINDOWPOS_CENTERED, sdl2.SDL_WINDOWPOS_CENTERED,
            width * 2, height * 2, sdl2.SDL_WINDOW_SHOWN,
        )
        renderer = sdl2.SDL_CreateRenderer(
            window, -1, sdl2.SDL_RENDERER_ACCELERATED)
        texture = sdl2.SDL_CreateTexture(
            renderer, sdl2.SDL_PIXELFORMAT_RGB24,
            sdl2.SDL_TEXTUREACCESS_STREAMING, width, height,
        )

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

                # Audio bridge: start + pump
                if enable_audio and not audio_started:
                    if os.path.exists(AUDIO_PATH):
                        if audio_file is None:
                            audio_file = open(AUDIO_PATH, "r+b")
                            audio_mm = mmap.mmap(audio_file.fileno(), 0)
                            audio_bridge = AudioBridge(
                                audio_mm, sdl2,
                                fd=audio_file.fileno())
                        audio_started = audio_bridge.start()
                        if audio_started and dump_wav:
                            audio_bridge.dump_wav(dump_wav)

                if audio_bridge and audio_started:
                    audio_bridge.pump()

                # Display
                magic, w, h, fmt, dirty = read_header(mm)
                if magic == FB_MAGIC and w > 0 and h > 0:
                    if not header_valid or w != width or h != height:
                        header_valid = True
                        width, height = w, h
                        sdl2.SDL_SetWindowTitle(
                            window,
                            f"oveRTOS QEMU Display - "
                            f"{width}x{height}".encode())
                        sdl2.SDL_SetWindowSize(window, width*2, height*2)
                        sdl2.SDL_DestroyTexture(texture)
                        texture = sdl2.SDL_CreateTexture(
                            renderer, sdl2.SDL_PIXELFORMAT_RGB24,
                            sdl2.SDL_TEXTUREACCESS_STREAMING,
                            width, height,
                        )

                    if dirty:
                        pixel_data = mm[FB_HEADER_SIZE:
                                        FB_HEADER_SIZE + width*height*2]
                        rgb = rgb565_to_rgb888(pixel_data, width, height)
                        sdl2.SDL_UpdateTexture(texture, None, rgb, width*3)
                        sdl2.SDL_RenderClear(renderer)
                        sdl2.SDL_RenderCopy(renderer, texture, None, None)
                        sdl2.SDL_RenderPresent(renderer)
                        struct.pack_into("<I", mm, 12, 0)
                        frames += 1
                        if once:
                            running = False

                sdl2.SDL_Delay(2)  # ~500 Hz pump rate for smooth audio
        except KeyboardInterrupt:
            pass
        finally:
            print(f"{frames} frames rendered.")
            if audio_bridge:
                audio_bridge.stop()
            if audio_mm:
                audio_mm.close()
            if audio_file:
                audio_file.close()
            sdl2.SDL_DestroyTexture(texture)
            sdl2.SDL_DestroyRenderer(renderer)
            sdl2.SDL_DestroyWindow(window)
            sdl2.SDL_Quit()
            mm.close()

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="QEMU shared-memory viewer for oveRTOS")
    parser.add_argument("--headless", action="store_true",
                        help="Validate without opening a window")
    parser.add_argument("--once", action="store_true",
                        help="Render one frame and exit")
    parser.add_argument("--no-audio", action="store_true",
                        help="Disable audio playback/capture")
    parser.add_argument("--dump-wav", metavar="PATH",
                        help="Dump playback audio to WAV file")
    args = parser.parse_args()

    if args.headless:
        sys.exit(run_headless(args.once))
    else:
        sys.exit(run_viewer(args.once, enable_audio=not args.no_audio,
                            dump_wav=args.dump_wav))


if __name__ == "__main__":
    main()
