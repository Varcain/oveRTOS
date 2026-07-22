# Plan: dynamic-linking FDPIC (run ld.so + load real .so libraries)

> Historical design record: dynamic FDPIC support has since landed. Milestone and debugging notes
> below preserve the design-time state rather than documenting current limitations.

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
### D2 part 1 DONE (commit 01b0376)
`ove_loader_load_fdpic(.., is_interp)` auto-detects DT_NEEDED → `is_dynamic`, skips .rel.dyn
for interp + dynamic exec, reports `got`. `ove_flat_t` has is_dynamic/got. (ld.so confirmed:
no DT_NEEDED, 18 .rel.dyn it self-applies → personality must NOT apply, else double-bias.)

### D2 part 2 — launch() wiring (NEXT, worked out — pure execution)
In `launch()` (`modules/lxp/src/lxp_run.c`), after `load_fdpic(&prog, exec, .., is_interp=0)`:
- `pc = prog.entry; at_entry = prog.entry; at_base = 0; prog.interp_loadmap = 0;`
- if `prog.is_dynamic`:
  - **find ld.so**: read the exec's PT_INTERP string (`/usr/lib/ld.so.1`), resolve it through
    the rootfs following symlinks (post-build adds `/usr/lib/ld.so.1`→`/lib/ld-uClibc.so.0`→
    `…so.1`→`ld-uClibc-1.0.58.so`) → the ELF bytes. (Reuse execve's symlink-resolving lookup;
    or hardcode `/lib/ld-uClibc.so.0` + follow links.)
  - `ld_base = region + align(prog.region_used);` (ld.so loads just past the exec in the region)
  - `load_fdpic(&ld, ld_data, ld_len, ld_base, region_size-(ld_base-region), is_interp=1);`
  - **the entry contract**: `pc = ld.entry` (jump to ld.so, NOT the exec); `at_base = ld_base`;
    `prog.interp_loadmap = ld.loadmap` (→ **r8**); `prog.got = ld.got` (→ **r9**). `prog.loadmap`
    stays = exec's (→ **r7**); `prog.phdr/phnum` stay = exec (→ AT_PHDR/PHNUM); `at_entry` stays
    = exec.entry (→ AT_ENTRY — the *program's* entry, not ld.so's).
  - `prog.region_used = (ld_base-region) + ld.region_used;` (so the arena lands past ld.so)
- **arena/pool**: for a dynamic proc, init the arena in the **PSRAM** slice (≥640K) not the
  in-region 96K — ld.so mmaps libc.so (~500K) from it. Static/bFLT keep the in-region arena.
- `setup_stack(.., is_fdpic, prog.phdr, prog.phnum, at_entry, at_base)` — add the at_base
  param → emit AT_BASE; keep AT_ENTRY=at_entry.
- `spawn_launch(.., &prog, (void*)pc, ..)` — the seam jumps to `pc` and sets r7=prog.loadmap,
  **r8=prog.interp_loadmap** (new — FR arg_tramp currently hardcodes r8=0), r9=prog.got.
- New `ove_flat_t` field: `uintptr_t interp_loadmap;` (r8; 0 for static).
- Per-engine seam: FR `arg_tramp` `mov r8, interp_loadmap` (from launch_args); same for NuttX/
  Zephyr later.

### D2 pt2 DONE (commit afb0968 + 8376adc) — ld.so loads + runs
launch() loads ld.so just past the exec + enters it. **Verified on-target**: r7=exec-loadmap,
r8=ld.so-loadmap, r9=GOT are all correct at entry (GDB break arg_tramp). THE r9 BUG (commit
8376adc): arg_tramp set r7/r8 but NOT r9 — ld.so's _start passes the ENTRY r9 to _dl_start as
its _DYNAMIC ptr, so it must be set (the program crt overwrites r9 → 0 fine for static). Wired
r9=prog.got via the launch_args struct (arg_tramp reads fields by offset). prog.got for ld.so
= got_base = ld_base (ld.so has no DT_PLTGOT → falls back to the load base; _dl_parse_dynamic_
info accepts it, so it's "right enough"). FR seam loads r7/r8/r9 from the struct.

### D3 — IN PROGRESS: ld.so runs its bootstrap, faults in _dl_malloc. PINNED PRECISELY.
ld.so self-relocates (__self_reloc on .rofixup) → _dl_start → _dl_parse_dynamic_info →
**`_dl_find_hash` → `_dl_malloc`, HardFaults** at `_dl_malloc+0x16`: `ldr lr,[r9,r3]; ldr
r3,[lr,#24]; ldr r9,[r3,#4]; blx r2`. Root cause: **`lr` (a GOT data-pointer entry) = `0x6008`
— the LINK-TIME vaddr, NOT relocated to `ld_base+0x6008`**. So `[lr+24]` is garbage → faults.
`lr` points at a `_dl_` global (the malloc state). ld.so's 13 `R_ARM_RELATIVE` relocs
(DT_RELCOUNT=13) that rebase these GOT entries **were not applied** before `_dl_find_hash`
needed them.

**VERIFIED CORRECT on-target (all ruled out):** entry r7=exec-loadmap, r8=ld.so-loadmap; the
ld.so loadmap (`ver=0 nsegs=2`, seg0 `addr=ld_base vaddr=0`, seg1 `addr=ld_base+0x5efc
vaddr=0x5efc` → both bias=ld_base ✓); AT_BASE=ld_base (→ `_dl_start` header/load_addr); the
working GOT `r9=0x2004b710=ld_base+0x5fa0` (from __self_reloc ✓); `load_addr` =
`__dl_init_loadaddr_map(got=working-GOT, map=dl_boot_ldsomap=r8)` so it has the right loadmap.
**`dl_boot_ldso_dyn_pointer` (r3 = my entry-r9) is DECLARED BUT NEVER USED** in dl-startup.c —
so the r9 value is a red herring (commit 8376adc moved the fault via the arg_tramp refactor,
not r9; keep r9=GOT anyway, it's conventionally correct + harmless).

**THE remaining question:** why ld.so's bootstrap RELATIVE pass
(`elf_machine_relative(load_addr, rel_addr, relative_count=13)`, dl-startup.c:294-299) doesn't
rebase this GOT entry. `rel_addr` comes from `dynamic_info[DT_REL]`, parsed from the **dpnt**
= `DL_BOOT_COMPUTE_DYN(dpnt, got, header=AT_BASE)`. NEXT: (1) read DL_BOOT_COMPUTE_DYN
(`fdpic/dl-sysdep.h`) — is the dpnt (ld.so's runtime _DYNAMIC) correct from got+AT_BASE? (2)
GDB-verify: break after _dl_parse_dynamic_info, read `tpnt->dynamic_info[DT_REL]`/[DT_RELCONT]
and whether `elf_machine_relative` actually ran + rebased [lr]'s entry (the FDPIC
per-segment rebase in `fdpic/dl-inlines.h` __reloc_pointer). Suspect the dpnt or the FDPIC
elf_machine_relative per-segment lookup. GDB recipe: qemu `-gdb tcp::1234 -S`; `break
arg_tramp`; `set $ldbase=*(unsigned*)($r0+4)-0xab1`; `add-symbol-file ld-uClibc-1.0.58.so
$ldbase`; `break *($ldbase+0x1694)`; read r9/lr/[lr]/[lr+24], `*(sp+28)`=caller. The libc.so
openat/read/pread64/mmap2 load is still downstream of this.

## Syscalls to add/upgrade (`modules/lxp/src/lxp_syscall.c`)
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

## Memory model (analyzed — the pool MUST go in PSRAM on an500)
mmap2(anon) bump-allocates page-aligned from a per-process **mmap pool**; the exec + ld.so
load into the program region, the pool backs ld.so's anon mmaps. Per process ≈ libc.so text
PT_LOAD (~500K — ld.so mmaps anon for the whole memsz then preads it in) + data (~30K) +
ld.so heap (loadmaps/funcdescs/hash/elf_resolve ~50K) ≈ **~580K**.

an500 layout (FR/NuttX): **FLASH 4M @ 0x0** (.text/.rodata — the 864K cpio is `static const`
so it lives HERE, NOT RAM) + **RAM 4M @ 0x20000000** (.bss: the 2MB program regions + ~350K
personality/FreeRTOS heap → only ~1.6MB headroom) + **PSRAM 4M @ 0x60000000**. The boot's ~3
concurrent procs need ~3×580K = ~1.74MB of pool, which does NOT fit the ~1.6MB RAM headroom
beside the 2MB regions. So the mmap pool MUST live in **PSRAM (0x60000000)** — the an500 has
it (NuttX already executes from there); mirror Zephyr an521's NOLOAD-PSRAM region placement
(`backends/zephyr/zephyr_lnx.c` + `ove-psram.overlay`). Per-region pool slices (NREG × ~640K)
fit PSRAM comfortably; reclaim = reset the slice cursor on launch (slot reuse). FR an500 needs
a PSRAM MEMORY region added to `mps2_an500.ld` + the pool placed there via a section attr.
**Later payoff:** libc.so **text-sharing** (one ~500K copy across all procs, not N) — harder
with ld.so's anon+pread model (the personality would have to dedup the mapping by file). Get
one process booting in PSRAM first, then share.

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
