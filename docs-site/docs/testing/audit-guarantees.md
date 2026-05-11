# Audit Guarantees

oveRTOS makes four substrate claims that distinguish it from typical
RTOS wrapper layers.  This page lists, per build configuration, what
is mechanically enforced versus what is honestly out of scope.

Every "yes" in the matrix below links to the script or CMake macro
that performs the check.  The point is not to claim purity — it is to
make the boundary visible.  Several cells are deliberately "no"
because the enforcement either isn't built yet (Phase 4–6 of the
[moat-hardening plan](#plan-status)) or because the underlying
RTOS-level invariant is irreducible (e.g. NuttX `g_mmheap`).

## The four moats

1. **Zero dispatch overhead.** No C++ vtables, no Rust trait-object
   dispatch, no function-pointer tables for `ove_*` APIs in any final
   ELF.  Wrapper code compiles to the same instructions as a direct
   backend call.
2. **Differential behavior across bindings.** The same test, executed
   through C/C++/Rust/Zig bindings against the same backend, produces
   the same observable state.  *(In progress — Phase 4.)*
3. **Zero-heap mode.** Builds with `CONFIG_OVE_ZERO_HEAP=y` refuse to
   compile direct allocator calls AND refuse to link binaries that
   reach `malloc` indirectly via libc or third-party code.
4. **Cross-language ABI pinning.** Every `OVE_ERR_*` numeric value
   is asserted at compile time in C, C++, Rust, and Zig; drift causes
   every translation unit to fail to build with a clear message.

## Per-config coverage matrix

Each row is a `(rtos, binding, mode)` tuple.  Each column is one
audit.  Cells link to the enforcement code; `n/a` marks a column
that does not apply to that row (heap-mode rows have no zero-heap
checks); `[planned]` marks a column whose enforcement is part of the
ongoing moat-hardening plan and is not yet wired.

| Build config | Symbol audit | Hotpath disasm | Heap-poison TU | Heap-wrap link | Heap-region nm | Differential | ABI pin |
|---|---|---|---|---|---|---|---|
| posix / c            | [yes][sym-posix] | [yes][hot-posix] | n/a              | n/a              | n/a              | [planned] | [yes][abi] |
| posix / c++          | [yes][sym-posix] | [yes][hot-posix] | n/a              | n/a              | n/a              | [planned] | [yes][abi] |
| posix / rust         | [yes][sym-posix] | [yes][hot-posix] | n/a              | n/a              | n/a              | [planned] | [yes][abi] |
| posix / zig          | [yes][sym-posix] | [yes][hot-posix] | n/a              | n/a              | n/a              | [planned] | [yes][abi] |
| stm32 / freertos / c            | [yes][sym-fr] | [yes][hot-fr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / freertos / c++          | [yes][sym-fr] | [yes][hot-fr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / freertos / rust         | [yes][sym-fr] | [yes][hot-fr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / freertos / zig          | [yes][sym-fr] | [yes][hot-fr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / freertos / zeroheap c   | [yes][sym-fr] | [yes][hot-fr] | [yes][heap-tu] | [yes][heap-wrap-fr] | [yes][heap-nm-fr] | [planned] | [yes][abi] |
| stm32 / freertos / zeroheap c++ | [yes][sym-fr] | [yes][hot-fr] | [yes][heap-tu] | [yes][heap-wrap-fr] | [yes][heap-nm-fr] | [planned] | [yes][abi] |
| stm32 / freertos / zeroheap rust| [yes][sym-fr] | [yes][hot-fr] | [partial][caveat-bindgen] | [yes][heap-wrap-fr] | [yes][heap-nm-fr] | [planned] | [yes][abi] |
| stm32 / freertos / zeroheap zig | [yes][sym-fr] | [yes][hot-fr] | [partial][caveat-bindgen] | [yes][heap-wrap-fr] | [yes][heap-nm-fr] | [planned] | [yes][abi] |
| stm32 / nuttx / c               | [yes][sym-nx] | [yes][hot-nx] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / nuttx / c++             | [yes][sym-nx] | [yes][hot-nx] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / nuttx / rust            | [yes][sym-nx] | [yes][hot-nx] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / nuttx / zig             | [yes][sym-nx] | [yes][hot-nx] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / zephyr / c              | [yes][sym-zr] | [yes][hot-zr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / zephyr / c++            | [yes][sym-zr] | [yes][hot-zr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / zephyr / rust           | [yes][sym-zr] | [yes][hot-zr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| stm32 / zephyr / zig            | [yes][sym-zr] | [yes][hot-zr] | n/a | n/a | n/a | [planned] | [yes][abi] |
| qemu/renode sim variants        | [planned][caveat-sim] | n/a (test ELFs) | inherits parent config | inherits | inherits | [planned] | [yes][abi] |

[sym-posix]: https://github.com/varcain/oveRTOS/blob/main/boards/host/posix/CMakeLists.txt#L100
[hot-posix]: https://github.com/varcain/oveRTOS/blob/main/boards/host/posix/CMakeLists.txt#L101
[sym-fr]: https://github.com/varcain/oveRTOS/blob/main/boards/stm32f746g-discovery/freertos/CMakeLists.txt#L126
[hot-fr]: https://github.com/varcain/oveRTOS/blob/main/boards/stm32f746g-discovery/freertos/CMakeLists.txt#L128
[sym-nx]: https://github.com/varcain/oveRTOS/blob/main/boards/stm32f746g-discovery/nuttx/CMakeLists.txt#L75
[hot-nx]: https://github.com/varcain/oveRTOS/blob/main/boards/stm32f746g-discovery/nuttx/CMakeLists.txt#L78
[sym-zr]: https://github.com/varcain/oveRTOS/blob/main/boards/stm32f746g-discovery/zephyr/CMakeLists.txt#L60
[hot-zr]: https://github.com/varcain/oveRTOS/blob/main/boards/stm32f746g-discovery/zephyr/CMakeLists.txt#L63
[heap-tu]: https://github.com/varcain/oveRTOS/blob/main/include/ove/heap_assert.h
[heap-wrap-fr]: https://github.com/varcain/oveRTOS/blob/main/cmake/OveCommon.cmake#L623
[heap-nm-fr]: https://github.com/varcain/oveRTOS/blob/main/cmake/OveCommon.cmake#L624
[abi]: #abi-pin
[caveat-bindgen]: #known-caveats
[caveat-sim]: #known-caveats

## What each column proves

### Symbol audit

POST_BUILD step that runs `<cross>nm` on the linked ELF and fails the
build if any of the following appear:

  - C++ vtable / typeinfo symbols (`_ZTV*`, `_ZTI*`, `_ZTS*`, or
    demangled `vtable for ` / `typeinfo for `).
  - Rust trait-object `<dyn …>::vtable` entries.
  - `ove_*` symbols of *data* type (D/B/R).  All `ove_*` APIs must be
    code symbols (T) or undefined externals (U) resolved by the backend
    at link time — a data-type `ove_*` symbol indicates a function-pointer
    dispatch table.

The audit also enumerates the `ove_*` text symbols actually present in
the ELF and writes them to `output/audit/symbols/<target>.txt`.

Script: `scripts/zero_overhead_audit.py`.
CMake macro: `ove_assert_no_dispatch_overhead()` in
`cmake/OveZeroOverheadAudit.cmake`.

### Hotpath disasm

POST_BUILD step that disassembles named benchmark-case symbols via
`<cross>objdump`, counts instructions, lists every `call`/`jmp` target,
resolves GOT-slot indirects for PIE binaries, and checks every entry
against `tests/audit/hotpath_expected.yaml`.

Golden expectations are pinned per target × per binding.  As of the
moat-hardening Phase 1, the file has 20 top-level target keys and 284
hotpath patterns covering POSIX and STM32F746 across FreeRTOS, NuttX,
Zephyr (incl. zeroheap variants for FreeRTOS).

Script: `scripts/dump_hotpaths.py`.
CMake macro: `ove_dump_hotpaths()` in
`cmake/OveZeroOverheadAudit.cmake`.

### Heap-poison TU

Zero-heap-only.  `include/ove/heap_assert.h` redeclares
`malloc`/`calloc`/`realloc`/`zalloc`/`memalign`/`aligned_alloc` with
`__attribute__((error("..." )))` under `CONFIG_OVE_ZERO_HEAP`.  The
header is pulled in via `ove/ove.h`, so every TU that includes the
public API also gets the poison.  `free()` is intentionally not
poisoned — calling `free(NULL)` is legitimate in cleanup paths.

This catches *direct* allocator calls at compile time, in user code
and in oveRTOS internals.  It does NOT catch third-party allocations
(lwIP, mbedTLS, …) — those are caught by the link-time wrap.

### Heap-wrap link

Zero-heap-only.  Final-exe link applies `-Wl,--wrap=malloc` (and
`calloc`/`realloc`/`free`/`zalloc`/`memalign`, plus NuttX `kmm_*`
family).  Calls to libc malloc — from any library or third-party
code — route through `__wrap_malloc` in
`backends/common/ove_heap_lock.c`, which checks a runtime gate
flag and either traps the build with `DEBUGASSERT(0)` (NuttX) or
`abort()`, OR forwards to `__real_malloc` during the bounded window
when allocation is permitted (early RTOS boot).

This is what makes the zero-heap claim hold against the entire link
graph, not just oveRTOS source.  Examples: an mbedTLS handshake that
calls `malloc` for a session buffer trips this trap; a `printf("%g",
…)` from newlib that pulls in `__d2b` allocation trips this trap.

CMake macro: `ove_apply_zero_heap_wrap()` in
`cmake/OveZeroHeapAudit.cmake`.

### Heap-region nm check

Zero-heap-only.  POST_BUILD `nm` check that the RTOS kernel heap
*region* symbols aren't instantiated in the final ELF.  Region
symbols are RTOS-specific:

  - **Zephyr**: forbids `_system_heap`, `z_malloc_heap`.  `STRICT` mode
    also forbids `__HeapBase`, `__HeapLimit` (newlib/picolibc heap).
  - **FreeRTOS**: forbids `xHeap`, `ucHeap`, `pucAlignedHeap` (heap_*.c
    globals; should be absent when
    `configSUPPORT_DYNAMIC_ALLOCATION=0`).  STRICT also forbids
    `__malloc_av_`, `__HeapBase`, `__HeapLimit`.
  - **NuttX**: forbids `g_kmmheap` (`CONFIG_MM_KERNEL_HEAP` split-mode
    pool).  See [known caveats](#known-caveats) for the `g_mmheap`
    irreducibility.

CMake macro: `ove_zero_heap_assert_no_kernel_alloc()` in
`cmake/OveZeroHeapAudit.cmake`.

### Differential

In progress (Phase 4 of the moat-hardening plan).  The substrate
exists: the same test sources compile against all four bindings via
`tests/{suites,cpp/suites,rust,zig}/`, and `tests/framework/suites.inc`
registers a common set.  The remaining work is wiring a JSON-emitting
runner that captures per-test observables (return codes, counters,
fairness metrics) across bindings and diffs them with a small
allowlist for legitimate divergence.

Until that lands, the binding tests run independently and divergence
between bindings on the same backend is *not* automatically caught.

### ABI pin

Every `OVE_ERR_*` numeric value is asserted at compile time across
all four bindings.  The master list is in
`include/ove/types.h` (lines 101–116).  The pinning blocks:

  - **C** (`include/ove/types.h:101-116`): `OVE_STATIC_ASSERT` per code.
  - **C++** (`bindings/cpp/ove/types.hpp:71-85`): `static_assert` per code.
  - **Rust** (`bindings/rust/ove/src/error.rs:136-154`): `const fn
    _assert_codes_match` invoked via `const _: () =
    _assert_codes_match();`.
  - **Zig** (`bindings/zig/ove/src/error.zig:83-100`): `comptime {
    std.debug.assert(...) }`.

If any binding's block omits a code, the lint
`config/ove-cli/ove/lint_error_codes.py` fails — the count of
asserted codes must equal the count of `#define OVE_ERR_*` lines in
`include/ove/types.h`.

If a code's value is renumbered in `types.h`, every TU that includes
the public API fails to compile with a clear "OVE_ERR_X drifted"
message.

Storage-size pinning (the Rust/Zig zero-heap `[byte; N]` arrays
sized from the backend's `sizeof(struct ove_X)`) is generated at
build time by `config/scripts/extract_storage_sizes.py` from a C
sentinel object's `nm --print-size` output.  Backend `.c` files
must not redeclare `struct ove_*` — enforced by the regex lint
`config/ove-cli/ove/lint_backend_struct.py` (the rule that caught the
20-vs-8-byte watchdog bug).

## Known caveats

This section documents what the audits do NOT catch.  Hiding gaps
makes the engineering claim worse, not better; surfacing them is the
honest version.

### NuttX `g_mmheap` is irreducible

The primary NuttX kernel `mm` region (`g_mmheap`) is the kernel-heap
carve-out used by `task_create`, `pthread_create`, `mq_open`, etc.
during boot.  It cannot be removed without rewriting NuttX core
primitives.  The zero-heap audit catches `g_kmmheap` (the split-mode
pool) and traps post-init allocations via the `--wrap` mechanism, but
the boot-time region itself stays.

"Zero-heap on NuttX" therefore means "zero *additional* heap traffic
after RTOS boot."  Boot-time allocations are bounded, predictable,
and a fixed cost.  Production builds running for hours/days have a
flat heap watermark from the moment `ove_main` runs.

### `bindgen` / `Emscripten` / Zig `cImport` bypass `heap_assert.h`

The `__attribute__((error(...)))` poison in
`include/ove/heap_assert.h` is a clang/gcc compiler attribute.  Code
generated by:

  - `bindgen` (Rust): scans header types into `extern "C"`
    declarations, dropping function attributes.
  - `Emscripten`: WASM target uses an Emscripten-bundled libc whose
    `malloc` declarations come from system headers, not ours.
  - Zig `@cImport` / translate-c: re-translates declarations and
    drops non-portable attributes.

In all three cases the *direct call check* doesn't fire.  Mitigations:

  - The Rust binding's `bindings_stub.rs` does not re-export `malloc`,
    so user-written `unsafe { malloc(...) }` is the only path that
    reaches it — and that path is still trapped at link time by
    `--wrap=malloc`.
  - Zig zero-heap builds depend on the same link-time `--wrap` trap.
  - WASM has its own separate heap model; the zero-heap claim does
    not extend to WASM and the docs do not assert that it does.

The link-time wrap is the durable backstop.  The compile-time poison
is the helpful-error layer for code that does compile via the C
front-end.

### NuttX flat-build `--wrap` propagation

When NuttX is built as a *flat* binary (`CONFIG_BUILD_FLAT=y`,
which is the default for most boards), the `--wrap=malloc` LDFLAGS
attached to oveRTOS's CMake target may not propagate cleanly to the
kernel-level link step that NuttX performs through its own
makefile-based build harness.  In *protected* and *kernel* build
modes, the wrap propagates correctly.

If you need a strong zero-heap claim on NuttX, build in protected
mode (`CONFIG_BUILD_PROTECTED=y`) — the user/kernel boundary makes
the wrap apply cleanly to both halves.  Phase 7 of the moat-hardening
plan investigates whether the flat-build path can be fixed.

### Sim/test ELFs not yet audited

The audits above run on benchmark targets (`boards/host/posix` and
`boards/stm32f746g-discovery/*`).  The test ELFs in `tests/sim/*` —
QEMU + Renode — currently do not invoke the audit POST_BUILD steps.
Phase 3 of the moat-hardening plan wires the symbol audit (but not
the hotpath disasm — test code is not the production hot path) into
those CMakeLists.

### Public ABI surface is not yet allowlist-enforced

The exported `ove_*` symbol surface is not yet captured in an
allowlist.  Adding a new public `extern void ove_foo(void)` does not
fail any audit today.  Phase 6 of the moat-hardening plan authors
`include/ove/ove-abi.txt` and a corresponding POST_BUILD nm check.

## Plan status

This page is part of an ongoing eight-phase moat-hardening effort.
Phase 1 (this doc + error-code ABI sync + drift lint) is complete.
Phases 2 through 7 are tracked in `plans/what-s-already-in-the-tidy-stearns.md`:

  - **Phase 2** — `audit.yml` CI workflow, manual benchmark runner.
  - **Phase 3** — sim/QEMU/Renode symbol-audit coverage.
  - **Phase 4** — differential harness foundation.
  - **Phase 5** — differential harness expansion + `diff-tests.yml`.
  - **Phase 6** — ABI surface allowlist + `OveAbiSurface.cmake`.
  - **Phase 7** — NuttX flat-build wrap investigation, polish,
    `audit_diff.py` symbol regression detection.

Each cell in the coverage matrix above will either become "yes" with
a link, or stay grey with an honest disclosure here.
