# Debugging FDPIC guest programs

Turnkey source-level GDB debugging of a Linux-personality guest program — plus
the `ld.so` and shared libraries it runs on — over the firmware's gdbstub.

## Why it needs a helper

An FDPIC object loads at **runtime** addresses: the loadmap relocates each
`PT_LOAD` segment independently, so an object has **two** biases (text + data),
and a dynamic program has three objects (the exec, `ld-uClibc.so.0`,
`libc.so.0`). Text is shared **in place from the board's rootfs backing store**.
On hardware that is normally internal flash or external QSPI NOR. So a plain
`file prog.elf` gives wrong line/symbol mappings, and a software breakpoint
cannot be written there.

## Use it

Attach GDB to the gdbstub (QEMU exposes `-gdb tcp::1234` by default; on hardware,
via ST-Link), then:

```gdb
(gdb) source config/scripts/ove-fdpic-gdb.py
(gdb) ove-fdpic-auto dbgdemo /path/to/dbgdemo   # wait for the next exec of "dbgdemo",
                                                # then load exec + ld.so + libc with source
(gdb) hbreak main        # HARDWARE breakpoint — safe for shared XIP text
(gdb) continue
```

`ove-fdpic-auto` runs until `<comm>` execs, reads the runtime bases the
personality publishes in `g_lxp_dbg[]`, runs to the exec's entry (by when
`ld.so` has linked everything), then walks the standard FDPIC `DT_DEBUG →
r_debug → link-map` rendezvous and `add-symbol-file`s every loaded object at its
own text bias. From there, breakpoints, `bt`, `next`, and locals all work.

!!! tip "Two rules"
    - Use **`hbreak`**, not `break` — guest text is shared XIP and is normally
      flash/QSPI-resident on hardware, where a software breakpoint cannot be written.
    - Use a **Python-enabled, ARM-capable GDB**. The distro `/usr/bin/gdb` 17.x
      (`--enable-targets=all` + FDPIC BFD) works directly; the bare
      `arm-none-eabi-gdb` toolchain ships **without** Python and can't `source`
      the helper.

## `--userspace`: frames like real Linux

By default the firmware objfile stays loaded, so a bare libc syscall-stub frame
(e.g. `write`) shows `?? ()` — GDB's frame lookup is section-filtered and lands
in the firmware's `.text`. Add `--userspace`:

```gdb
(gdb) ove-fdpic-auto dbgdemo /path/to/dbgdemo --userspace
```

After the walk it **drops the firmware/kernel symbols**, mirroring how real Linux
debugs userspace (the kernel is never a userspace objfile). Then every
shared-XIP address belongs to exactly one objfile and frames resolve cleanly:

```
#0  0x00092848 in write ()
#1  0x000e1ff4 in main () at dbgdemo.c:19
#2  0x000b203e in __uClibc_main ()
#3  0x000e1e74 in _start ()
```

Trade-off: kernel frames become `?? ()` (re-`add-symbol-file <firmware>` to
inspect the kernel side). Omit the flag to keep the firmware for
cross-boundary/kernel debugging.

## Other commands

- `ove-fdpic-map <comm> <elf>` — map an already-loaded program's symbols.
- `ove-fdpic-debug <comm> <elf>` — run to a program's exec, map just its symbols.
