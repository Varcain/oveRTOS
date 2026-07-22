# Plan: FDPIC ELF loader for the Linux personality

> Historical design record: FDPIC support has since landed. Statements about missing behavior
> describe the design-time baseline, not the current personality.

Goal: load **static FDPIC** binaries (ET_DYN ARM ELF, `EF_ARM_FDPIC`) alongside the current
bFLT loader, so multiple processes can later SHARE one read-only text copy (the NOMMU
footprint win — today each 512K region duplicates BusyBox's ~324K text). This first cut
gets a single FDPIC binary to load + run with correct relocation + the FDPIC register; text
sharing across procs is the follow-on that the format unlocks.

## Toolchain (prerequisite — in progress)

The stock `arm-buildroot-uclinux-uclibcgnueabi` gcc is FLAT-only (`-mfdpic` still emits bFLT;
no `R_ARM_FUNCDESC` relocs; elf2flt hardwired in the link spec). Real FDPIC needs the
`arm-buildroot-uclinuxfdpiceabi` triple → a separate toolchain. Building it out-of-tree in
`buildroot/output-fdpic` (`BR2_BINFMT_FDPIC=y`) so the working bFLT `output/` is untouched.
Test binary once built:
```
output-fdpic/host/bin/arm-buildroot-uclinuxfdpiceabi-gcc -O2 -static m.c -o m.fdpic
readelf -hld -r m.fdpic     # Type DYN, EF_ARM_FDPIC, PT_LOAD x2, PT_DYNAMIC, the relocs
```

## Current loader (from the loader-map) — what we extend

- `include/ove/loader.h:131` `ove_flat_t { region, region_size, region_used, entry, text_base,
  text_size, data_base, data_size, bss_size, stack_size }`.
- `loader/ove_loader.c:654` `ove_loader_load_flat(prog, image, len, region, region_size)` —
  bFLT: single base (text_base==load base), big-endian header, base-add reloc walk.
- `modules/lxp/src/lxp_run.c` `launch()` calls the loader — **the format
  branch goes here** (magic: 'bFLT' vs 0x7f'ELF').
- `modules/lxp/src/lxp_run.c`: after load, `rw = region + align16(region_used)`,
  arena (96K) at `rw`, stack above arena, then `spawn_launch(sidx, ridx, &prog, entry, sp,
  stack_lo)`.
- Per-engine `spawn_launch` (`zephyr_lnx.c:187`, `freertos_lnx.c:163`, `nuttx_lnx_trap.c:153`):
  set SP, r0=0, PC=entry. **No r9/GOT register is set today** — bFLT is single-base. FDPIC's
  defining need is the **FDPIC register r9 = the loaded GOT**.

## FDPIC loader design

### `ove_flat_t` extension
Add `int is_fdpic;` and `uintptr_t got;` (the r9 value at entry; 0 for bFLT). Keep the same
text/data/bss/entry accounting so `launch()`'s region/arena/stack math is unchanged.

### `ove_loader_load_fdpic(prog, image, len, region, region_size)`
1. **Parse** Elf32_Ehdr — require `e_type==ET_DYN`, `e_machine==EM_ARM`, `e_flags &
   EF_ARM_FDPIC (0x10000000)`. Reject (→ NOT_SUPPORTED) PT_INTERP-present (we want static; a
   dynamic FDPIC would need ld-uClibc-fdpic — out of scope for v1).
2. **Load segments** — walk Elf32_Phdr; for each PT_LOAD copy `[p_offset, +p_filesz)` into the
   region and zero `[p_filesz, p_memsz)`. Lay them out contiguously (text at region+0, data
   16-aligned after) — independent placement is possible but contiguous keeps the region/arena
   math identical to bFLT. Record per-seg `(load_addr, p_vaddr, p_memsz)` → the **loadmap**;
   `load_bias[seg] = load_addr - p_vaddr`.
3. **Relocate** from PT_DYNAMIC (DT_REL/DT_RELSZ/DT_RELENT; ARM uses REL not RELA). Handle, per
   `readelf -r` of the real binary (exact set TBD):
   - `R_ARM_RELATIVE` — `*where += load_bias(seg-of-where)`.
   - `R_ARM_FUNCDESC_VALUE` — a 2-word descriptor {func_addr, got}: func_addr += text bias, got
     = the loaded GOT base.
   - `R_ARM_ABS32` — `*where += relocated sym value`.
   - funcdesc GOT relocs as the binary actually uses them.
4. **GOT / r9** — the GOT lives in the data segment; `prog->got` = data load_addr + the GOT
   offset (from DT_PLTGOT / the dynamic `_GLOBAL_OFFSET_TABLE_`). Set `prog->is_fdpic=1`.
5. Fill `prog->entry = e_entry + text bias` (Thumb bit preserved), text/data/bss bases+sizes
   for region accounting.

### Launch wiring
- `launch()`: branch on magic → `load_flat` or `load_fdpic`.
- `spawn_launch`: when `prog->is_fdpic`, set **r9 = prog->got** at entry (extend the per-engine
  `arg_tramp` / reg init to take + set r9). bFLT path leaves r9 as-is.
- Stack/argv/auxv: FDPIC crt1 may want `AT_BASE`/loadmap aux entries — verify against the real
  crt1 (`lxp_setup_stack` in `modules/lxp/src/lxp_syscall.c`).

## Open questions — resolve by inspecting the real binary (once the toolchain builds)
1. **Static FDPIC layout:** does `-static` give a no-PT_INTERP binary that self-contains its
   relocs in DT_REL? (`readelf -l`/`-d`.)
2. **Entry protocol:** disassemble `_start` — what does it expect in r9 / r7 / the stack
   (the loadmap pointer)? Does crt1 self-relocate (needs the loadmap passed in) or rely on the
   loader having applied all relocs (then only r9 matters)? **Decision leaning:** the loader
   applies all relocations; pass r9=GOT; pass the loadmap only if crt1 demands it.
3. **Reloc types** actually emitted (`readelf -r`) and whether any need a symbol table walk.
4. **GOT discovery:** DT_PLTGOT vs the dynamic `_GLOBAL_OFFSET_TABLE_`; the exact r9 value.

## Phasing
F1: toolchain + a minimal `int main(){return 42;}` FDPIC binary; nail the ABI by inspection.
F2: `ove_loader_load_fdpic` (parse + load + relocate) + the launch r9 wiring; run the minimal
binary (exit 42) on one engine. F3: a printf/hello (exercises funcdesc relocs + libc). F4: an
FDPIC BusyBox rootfs; boot the full personality on FDPIC, 3 engines, ASan. F5 (the payoff):
share ONE read-only text copy across procs (map the same text region for N processes; per-proc
data+GOT) → the footprint win.

## Files
- `include/ove/loader.h` — `is_fdpic`/`got` in `ove_flat_t`; `ove_loader_load_fdpic` decl.
- `loader/ove_loader.c` (or new `loader/ove_fdpic.c`) — the FDPIC loader.
- `modules/lxp/src/lxp_run.c` — format branch in `launch()`; pass r9 to spawn_launch.
- `modules/lxp/src/lxp_run_internal.h` + the 3 seams — `spawn_launch` carries r9; set it at entry.
- Buildroot `output-fdpic` (BR2_BINFMT_FDPIC) — the FDPIC rootfs (separate from the bFLT build).
