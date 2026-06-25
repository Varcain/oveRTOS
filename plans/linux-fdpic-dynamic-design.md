# Plan: dynamic-linking FDPIC (run ld.so + load real .so libraries)

Goal: boot a **dynamically-linked** FDPIC busybox (DT_NEEDED=libc.so.0, PT_INTERP=ld.so) with
feature parity to the static FDPIC build — i.e. actually run uClibc-ng's FDPIC `ld-uClibc.so`
so it loads `libc.so` from the VFS and relocates across objects. Buildroot config:
`configs/overtos_fdpic_defconfig` (BR2_BINFMT_FDPIC + BR2_SHARED_LIBS).

The full uClibc-ng protocol is spec'd (agent study of uClibc-ng-1.0.58 source). Highlights
that shape the design:
- **Loadmaps go in REGISTERS, never auxv.** `AT_*_FDPIC_LOADMAP` is unused by uClibc-ng ARM.
- The personality must **load BOTH** the executable AND the interpreter (ld.so), build a
  loadmap for each, and **jump to ld.so's e_entry** (not the program's).
- The killer new NOMMU syscall is **`pread64`**: `MAP_FIXED`-file `mmap` fails on NOMMU, so
  ld.so `mmap`s anonymous RWX memory then `pread64`s each PT_LOAD's file bytes into it.

## Entry contract (what the seam must set up to launch ld.so)
Jump to **ld.so's** `e_entry` with:
- **r7** = the executable's `elf32_fdpic_loadmap*`
- **r8** = ld.so's `elf32_fdpic_loadmap*`
- **r9** = ld.so's GOT base (its data-segment GOT, = loadmap-relocated `DT_PLTGOT`)
- **sp** = standard ELF stack `[argc][argv..][NULL][envp..][NULL][auxv][AT_NULL]`
- **auxv** (min): `AT_PHDR`=exec phdrs runtime addr, `AT_PHNUM`, `AT_PHENT`=32,
  `AT_PAGESZ`=4096, `AT_ENTRY`=program entry, `AT_BASE`=ld.so base, `AT_NULL`. (Already have
  the standard-inline-stack + AT_PHDR machinery from static FDPIC — extend it.)

ld.so `_start` self-relocates from r8's loadmap (`__self_reloc` on its own .rofixup), then
`_dl_start(got, r7, r8, r9, &funcdesc_out, args)` loads the libs and writes the program's
`{entry, got}` funcdesc; the asm then `bx`es into the program with r9=program GOT. We do NOT
manage funcdescs — ld.so allocates them via `_dl_malloc` (our mmap2/brk).

## Loader changes (loader/ove_loader.c)
- Factor `ove_loader_load_fdpic` so it can load an arbitrary FDPIC ELF object (exec or ld.so)
  into a given region/base + return its loadmap + got. For a DYNAMIC exec (has PT_INTERP, has
  DT_NEEDED) we DON'T apply .rel.dyn / build descriptor pools (ld.so does that) — we only
  load the PT_LOADs + build the loadmap. (Static path stays as-is.)
- launch(): if the exec has PT_INTERP → also load `/lib/ld-uClibc.so.1` (resolve the interp
  path from PT_INTERP, fall back to the rootfs `/lib/ld*.so*`) into a 2nd region, build its
  loadmap+got, set the entry = ld.so entry, r7/r8/r9 accordingly.

## Syscalls to add/upgrade (linux/ove_linux_syscall.c)
- **`pread64` (180)** NEW: read `count` bytes at `offset` (64-bit, ARM passes hi/lo + a pad)
  from an fd into the user buffer — like read but positioned, no fd offset change.
- **`mmap2` (192)** UPGRADE: an anonymous `MAP_ANONYMOUS` mapping must return REAL,
  page-aligned, writable+executable memory from a **dedicated mmap pool** (not the tiny
  96K arena — libc.so's segments + ld.so's heap need hundreds of KB). A `MAP_FIXED` file
  mapping may return MAP_FAILED (ld.so recovers via pread64). Treat MAP_EXECUTABLE/DENYWRITE
  as no-ops.
- **`mprotect` (125)** NEW: no-op success (NOMMU; ld.so uses it for RELRO, must not hard-fail).
- Ensure present + correct: `openat`, `read`, `close`, `fstat64`, `stat64`/`statx`,
  `readlinkat`, `write`, `exit_group`, `getuid/euid/gid/egid`, `getpid`, `gettimeofday`
  (or a `/dev/urandom` that opens+reads — for the SSP canary, if SSP is on).

## Memory model
A per-run **mmap pool** (a contiguous block) that mmap2(anon) bump-allocates page-aligned.
Sizing: ld.so + libc.so segments + ld.so heap (loadmaps, funcdescs, hash, elf_resolve) ≈
0.6–1 MB for one process. Engine budget: FR/NuttX an500 4MB (regions 2MB + pool); Zephyr
an521 16MB PSRAM. The exec + ld.so can live in regions; the pool backs ld.so's anon mmaps.
(Text-sharing across procs is a later optimization — first get one process booting.)

## elf32_fdpic_loadmap (confirmed)
`{u16 version=0; u16 nsegs; seg[]{u32 addr; u32 p_vaddr; u32 p_memsz}}`, segs ordered by
ascending p_vaddr (ld.so's `__reloc_pointer` scans linearly). version=0/nsegs>0 enforced.

## Phasing
D1: add pread64 + mprotect-noop + the mmap pool (mmap2 anon returns real memory). D2: load
ld.so as a 2nd object + the r7/r8/r9 + auxv entry (jump to ld.so). D3: boot — GDB-iterate
through ld.so's lib load (openat/read/pread64/mmap2 of libc.so) until it reaches busybox
main. D4: feature parity (pipes/ps/bg) + verify it loaded /lib/libc.so (not static). FR first.

## Spec reference (uClibc-ng-1.0.58, extracted /tmp/uclibc-src)
- entry/_dl_start proto: ldso/ldso/arm/dl-startup.h:21-303
- loadmap struct: libc/sysdeps/linux/arm/bits/elf-fdpic.h:40-114
- segment mapping (NOMMU pread path): ldso/ldso/dl-elf.c:403-512, 520-834
- syscall numbers/forms: ldso/include/dl-syscall.h:96-318
- auxv usage: ldso/ldso/dl-startup.c:124-391, ldso.c:424-770
