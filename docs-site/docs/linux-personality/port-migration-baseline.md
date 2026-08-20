# LXP Port Migration Baseline

This is the Iteration 0 baseline for moving reusable Linux-personality and RTOS
seam logic out of the oveRTOS `linux_interop` application and into LXP. The
iteration changes no firmware source or runtime behavior. It defines ownership,
records the current cost, and adds a migration ledger that later iterations
must update explicitly.

## Baseline identity

The baseline was rebuilt on 2026-08-16 from:

- oveRTOS `1fc497b189bc`;
- LXP `21272d5d3b59`;
- STM32F746G-DISCO `linux_interop` production profile;
- 10 ms guest quantum, QSPI rootfs, networking, `/data`, raw block access,
  netfs execution, PTY, devices, display/input, and RT-scope enabled; and
- nine co-resident program regions and thirteen process slots on every engine.

Each program region is 256 KiB and each dynamic pool is 512 KiB. The generated
configuration hashes are:

| Engine | `.config` SHA-256 |
|---|---|
| FreeRTOS | `0cda1c8ac4def4b1ab730522b3b77ffa3ac1f3511cebcd21572609f5b226a012` |
| NuttX | `12f9308b518044ae8fc018587369c16655c173be8d7e4a64066434b2850bfdd8` |
| Zephyr | `b421f6dcbca1c78b6f2d9232b79858efa8630ba37af2cf1e0ea3211b2bd63e61` |

Generated configurations live under
`output/stm32f746g-discovery/<engine>/linux_interop/.config`.

## Firmware and memory baseline

The figures below come from clean current engine linker reports. Binary size is
the produced flash file; flash footprint is the linker's region accounting and
can differ where the file omits address-space gaps.

| Engine | Flash binary | Flash footprint | Internal static memory | External SDRAM |
|---|---:|---:|---:|---:|
| FreeRTOS | 310,700 B | 308,548 B | 287,992 B across regions | 8,063,824 B |
| NuttX | 352,676 B | 352,676 B | 231,204 B SRAM1 | fixed-address pools not reported by its linker |
| Zephyr | 336,688 B | 355,120 B | 257,024 B RAM + 29,568 B DTCM | 7,912,272 B |

FreeRTOS's internal placement is split across 320 B of Ethernet descriptors,
45,856 B of DSP/DTCM, and 241,816 B of RAM rather than one linker region. Its
external accounting also reserves 1,536 B of the separate Ethernet TX region.
The authoritative per-region values are:

| FreeRTOS region | Used | Capacity | Headroom |
|---|---:|---:|---:|
| DSP/DTCM | 45,856 B | 48,832 B | 2,976 B |
| RAM | 241,816 B | 262,144 B | 18,392 B |
| SDRAM | 8,063,824 B | 8,386,560 B | 59,568 B |
| Ethernet TX | 1,536 B | 2,048 B | 512 B |

Zephyr has about 5 KiB of main-RAM headroom. These narrow margins mean a seam
migration must compare all three link reports even when it is intended to be a
source-only move.

## Current source ownership

The baseline integration was 10,012 lines across the following groups. All
three RTOS portions have since moved to canonical LXP and are no longer
oveRTOS migration exceptions.

| Group | Lines | Current owner | Intended owner |
|---|---:|---|---|
| `app.c` and `rt_scope.c` | 2,463 | oveRTOS app | app behavior remains; generic bootstrap and lifecycle move out |
| five `lxp_ove_*` host adapters | 2,507 | oveRTOS common backend | oveRTOS, narrowed to stable HAL/provider translation |
| FreeRTOS, NuttX, and Zephyr task/trap/MPU seams (baseline) | 4,430 | mixed during migration | LXP `ports/<rtos>/` |
| Cortex-M cache/MPU/memory helpers and LXP metrics | 612 | oveRTOS backends | reusable port mechanics move to LXP; host metrics facade may remain |

The exact ownership inventory is enforced by
`tests/cmake/TestLxpPortOwnership.cmake`. It currently admits:

- the common provider adapters and narrow per-engine host-policy bindings named by
  the test;
- `app.c`, `rt_scope.c`, and `rt_scope.h`; and
- the FreeRTOS, NuttX, and Zephyr implementations under `modules/lxp/ports/`.

This is an enforced ownership boundary: the ledger rejects any reintroduced
consumer-owned task/trap seam or unlisted host adapter.

## Ownership rule

LXP owns Linux semantics and reusable RTOS mechanisms: guest task lifecycle,
trap entry, MPU profiles, executable publication, and SVC accounting. oveRTOS
owns board resources, stable-HAL provider implementations, and translation of
Kconfig into LXP configuration. The application owns workload and diagnostic
policy only.

The canonical rationale and completion test live in LXP's
`docs/port-ownership.md`. In addition to the oveRTOS ledger, LXP's
`scripts/check-decoupled.sh` prevents its portable core from including either
oveRTOS or native RTOS dependencies; those dependencies are allowed only in
explicit ports.

## Iteration acceptance rule

For every later iteration:

1. state which ownership violation is being removed;
2. move one coherent mechanism with no parallel implementation left behind;
3. update the ledger and build inventory in the same commit;
4. run the LXP contract checks and oveRTOS structural tests;
5. build all affected engines and compare flash/RAM against this baseline; and
6. perform hardware validation when task, trap, MPU, cache, or lifecycle code
   changed.

Runtime numbers are intentionally not duplicated here. The existing
[complexity remediation baseline](complexity-baseline.md) contains the detailed
functional and real-time history; this document freezes the current repository
boundary and current build cost for the new port migration.

## Iteration 1: LXP-owned Cortex-M support

Iteration 1 moves the reusable cache geometry, bounded executable-publication,
PMSAv7 descriptor decoding, live MPU snapshot, and CPU-memory-contract
validation helpers into canonical LXP under `include/lxp/arch/`. Their public
names now use the `lxp_cortex_m_*` namespace. The pure host tests moved with the
implementation.

oveRTOS removed its two 420-line duplicate architecture headers and the
188-line pure architecture test. It retains a 37-line host-policy header that
declares the uncached and STM32F746 WBWA contracts, plus a focused 57-line test
that checks those board-owned declarations against LXP's decoder. No forwarding
header or compatibility alias remains.

The generated configurations and all static-memory figures are unchanged.
Production links against LXP `3ca880f` compare with Iteration 0 as follows:

| Engine | Iteration 0 flash | Iteration 1 flash | Delta | Iteration 1 binary |
|---|---:|---:|---:|---:|
| FreeRTOS | 308,548 B | 308,556 B | +8 B | 310,708 B |
| NuttX | 352,676 B | 352,684 B | +8 B | 352,684 B |
| Zephyr | 355,120 B | 355,132 B | +12 B | 336,700 B |

The 8–12 byte text differences are within compiler/link layout noise from the
renamed inline interface. There is no configuration, data, BSS, pool, or stack
change.

All 47 canonical LXP CTest targets and all 46 oveRTOS host/structural targets
pass. The LXP header contract now recursively compiles architecture subdirectory
headers by themselves in both C11 and C++17. The oveRTOS migration guard requires
the three LXP headers and rejects either retired oveRTOS duplicate.

Each production image was then flashed to the same STM32F746G-DISCO and checked
through the Linux guest over SSH. Every engine booted with LXP `3ca880f`, exposed
the expected 13 slots and nine regions, and served both `/proc/lxp_resources`
and `/proc/rt_scope`:

| Engine | RT releases/executions | Missed / late / overrun | Dispatch avg / max |
|---|---:|---:|---:|
| FreeRTOS 11.2.0 | 11,449 / 11,449 | 0 / 0 / 0 | 7.35 / 34.67 us |
| NuttX 12.12.0 | 11,828 / 11,828 | 0 / 0 / 0 | 8.02 / 33.72 us |
| Zephyr 4.4.0 | 131,421 / 131,421 | 0 / 0 / 0 | 7.98 / 57.46 us |

These are boot smoke intervals, not workload benchmarks; they establish that
the relocated cache and MPU logic still validates live silicon and permits
unprivileged Linux guests to execute on every engine.

## Iteration 5: immutable host and rootfs composition

After the three RTOS ports moved, the application still performed one reusable
piece of personality work: it published the external rootfs window, parsed the
newc archive, repeated four rootfs fields in every `lxp_run_config_t`, and
reselected the same five provider tables for every launch.

Canonical LXP now exposes `lxp_host_init_cpio()` and `lxp_host_run()`. The host
object is caller-allocated and zero-heap. Initialization publishes the memory
window before the first archive read, parses the CPIO once into caller-owned
table/name storage, and captures an immutable provider bundle. Each sequential
launch supplies only console, environment, display, exit-diagnostic, and
RT-scope policy through `lxp_launch_config_t`; LXP constructs the complete run
contract internally.

oveRTOS's `lxp_ove_host.c` remains the narrow composition adapter that selects
the configured OS, network, display, filesystem, and block providers. The app
still owns the rootfs image choice and bounded storage sizes. Its RTOS worker,
network smoke test, watchdog/fault demonstrations, latency reporting, and
two-phase workload deliberately remain application policy. FreeRTOS scheduler
startup also remains under the general oveRTOS application-lifecycle contract,
not the Linux-personality API.

The ownership test rejects direct CPIO parsing, raw `lxp_run_config_t`
construction, and the retired pre-facade wrappers in `linux_interop/src/app.c`.

## Iteration 6: storage request ownership

The common oveRTOS storage adapter previously contained two pieces of Linux
personality policy. Its asynchronous completion was correlated through five
independent owner/state/cancel globals, and raw block reader/writer counts lived
below the provider boundary even though the provider API exposes one aggregate
native-media lease rather than a per-open handle.

Canonical LXP now owns a zero-heap asynchronous provider gate. It keys a saved
request by generation-qualified owner and operation tag, and resolves worker
completion versus guest cancellation with one atomic state transition. A
cancelled request is either retired by the worker without publishing a wakeup,
or an already-complete result is discarded before a reused slot can collect it.

The LXP block class now aggregates Linux opens: the first reader acquires the
provider, the last reader releases it, and a writer is exclusive. The block
provider ABI is version 2 to make this lifetime contract explicit. oveRTOS no
longer duplicates those counts; its native `ove_block_t` remains the physical
lease shared with RTOS-native filesystem/raw callers.

The audit deliberately retained media-generation arbitration, DMA-safe staging,
the serialized worker, worker priority, and period/budget admission in oveRTOS.
Those mechanisms protect native callers and express host scheduling or hardware
policy. LXP's rotating blocked-operation scan already owns fairness between
parked guests; moving the RTOS server budget into the personality would invert
that boundary rather than simplify it.

The first NuttX link exposed a toolchain-specific size trap: GCC specialized
the complete sync/async dispatcher into every small filesystem wrapper, adding
about 15 KiB. Keeping that dispatcher out of line restored the intended shared
boundary. Production links against LXP `93fc5ef` compare with Iteration 5 as
follows:

| Engine | Iteration 5 flash | Iteration 6 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 309,660 B | 309,188 B | -472 B |
| NuttX | 354,268 B | 354,804 B | +536 B |
| Zephyr | 357,688 B | 357,464 B | -224 B |

All 46 oveRTOS host and structural tests pass. Focused canonical LXP tests cover
the gate state transitions, generation and operation matching, failed-submit
rollback, aggregate block-reader ownership, writer exclusion, and cancellation
beyond the native handle-slot count. The latter proves that both active and
already-completed cancelled opens reclaim their otherwise orphaned native
handles before the request identity can be reused.

## Iteration 7: network readiness ownership

The common network adapter still reached back into the personality through the
global `lxp_sock_kick()` symbol. That link hid the callback lifetime and made a
host provider choose a core scheduling primitive. The audit found no equivalent
to the storage asynchronous-request state machine: every native socket is
nonblocking, and LXP already owns the sole outstanding state through its
generation-qualified parked wait, retry operation, deadline, and fairness
class. Adding cancellation correlation below that boundary would duplicate
state rather than remove it.

Network-provider ABI version 3 therefore makes readiness explicit and
run-scoped. `run_begin()` receives a callback and context; a provider advertising
`LXP_NET_CAP_SOCKET_READY_EVENT` may retain them only until `run_end()` returns.
LXP translates the callback into its coordinator event, while oveRTOS retains
the backend-sized native socket pool, first/last-socket subscription to the
native readiness channel, and RTOS/network-stack execution policy. FreeRTOS
uses the event path; NuttX and Zephyr deliberately retain the bounded 5 ms
polling fallback because their native stacks do not yet publish the host event.

The same pass closed a latent boundedness defect. `socket()` and `accept()` had
ignored failure while enabling native nonblocking mode, which could expose a
blocking socket to the privileged coordinator. LXP now closes and unpublishes
the native handle on either failure. Host tests force more consecutive failures
than the socket-pool depth and then succeed, proving that native handles, LXP
socket slots, and guest descriptors are all reclaimed. The oveRTOS ownership
ledger rejects any return of the retired global kick dependency.

All 435 oveRTOS stub cases pass with the loopback and adapter-readiness
tests enabled. Production links remain bounded; relative to Iteration 6, the
small callback and fail-closed paths change flash as follows:

| Engine | Iteration 6 flash | Iteration 7 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 309,188 B | 309,428 B | +240 B |
| NuttX | 354,804 B | 354,948 B | +144 B |
| Zephyr | 357,464 B | 356,760 B | -704 B |

## Iteration 8: display and input lifecycle ownership

The display adapter still split one state machine across repositories. LXP
calculated each framebuffer dirty rectangle but delegated the dirty flag to
oveRTOS, then invoked a separate periodic present callback. Touch polling kept
hidden function-static timing and pressed state across sequential runs, while
the FT5336 provider allocated a new I2C instance on every launch without a
matching release. DMA2D initialization was even less self-contained: the demo
application happened to initialize it inside network bring-up before LXP
registered `/dev/dma2d`.

Display-provider ABI version 2 makes those boundaries explicit. LXP now owns
dirty-rectangle union and the 30 Hz presentation cadence, passes one coalesced
rectangle to the provider, resets framebuffer/input timing and event state per
run, and releases the touch provider before clearing its table. The static
device namespace remains reusable across launches, but open instances and tick
subscriptions are run-scoped. `/dev/dma2d` is registered only after the
provider's explicit initializer succeeds; the demo application no longer
contains generic DMA2D lifecycle code.

oveRTOS remains responsible for the physical framebuffer and its cache
publication, synchronous DMA2D hardware/coherency, and FT5336 I2C access. Its
framebuffer layer is now a stateless forwarder, Zephyr flushes the bounded span
covering the coalesced rectangle, and FT5336 uses caller-owned static I2C
storage with a matching deinitializer instead of leaking a heap-backed bus on
the phase-1 to phase-2 launch transition. The ownership test prevents device
tick/input policy or generic display initialization from returning to the host
adapter or demo application.

## Iteration 9: console transport and readiness ownership

The demo application still owned the concrete program-console implementation:
STM32 FIFO lookahead, QEMU UART1 registers, byte-at-a-time input, output newline
translation, and readiness polling. Zephyr also called the global
`lxp_console_kick()` symbol directly from its UART interrupt, coupling a generic
oveRTOS backend to one personality's coordinator.

Canonical LXP `9759a47` replaces that global entry point with a paired,
run-scoped subscription in the launch contract. LXP installs its immutable
coordinator callback only after the OS and memory contracts are ready, removes
it before teardown, and owns whether a parked console wait uses readiness events
or the bounded 5 ms polling fallback. A failed subscription fails the launch
closed, and providers must stop callbacks before unsubscribe returns.

oveRTOS now owns the concrete console transport in `lxp_ove_console.c`. It binds
only the console fields of a caller-owned launch configuration, so the app keeps
exit, workload, diagnostics, and RT-scope policy. FreeRTOS and Zephyr publish
STM32 RX events; NuttX and QEMU retain the polling fallback because their current
console paths do not expose an appropriate readiness source. Zephyr no longer
includes or calls LXP from its generic console backend. FreeRTOS USART1 now runs
at `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`, which is required because its
RX callback can wake the coordinator through an ISR-safe FreeRTOS semaphore.

All seven canonical LXP CTest targets and all 47 oveRTOS host/structural targets
pass. Production links compare with Iteration 8 as follows:

| Engine | Iteration 8 flash | Iteration 9 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 310,244 B | 310,596 B | +352 B |
| NuttX | 355,436 B | 355,908 B | +472 B |
| Zephyr | 356,640 B | 356,640 B | 0 B |

The generated configurations and fixed LXP pools are unchanged. The ownership
ledger rejects console transport mechanics in `linux_interop/app.c` and rejects
any renewed direct dependency from the Zephyr console backend to LXP.

## Iteration 10: immutable network topology and native interface ownership

The application still allocated the native interface, brought it up, waited for
an address, mutated LXP's process-global eth0 selector, parsed a dotted IPv4
string by hand, and mutated a second process-global 9P mount. The mount was
retained across `lxp_netfs_shutdown()`, so a later host or run could silently
inherit stale topology.

Canonical LXP now captures the opaque interface handle and a validated copy of
the optional netfs endpoint in `lxp_host_t`. `lxp_host_run()` forwards both as
part of the complete run contract. The coordinator binds eth0 only after the
network provider enters its run lifecycle and clears it before provider
teardown. Netfs copies topology anew on every run and discards it at shutdown;
NULL explicitly disables the mount. The obsolete provider-table netif member
and public topology setters are removed, and the network-provider ABI is 4.

oveRTOS now exposes one `ove_lxp_host_t` that owns native interface storage,
bring-up, bounded address wait, rollback, teardown, provider selection, and the
LXP host. The app still selects its static IP, gateway, rootfs, and 9P endpoint,
but it no longer owns native lifecycle mechanics or parses transport addresses.
Strict IPv4 validation rejects malformed configuration before touching the
rootfs. The ownership test prevents the retired setters, direct LXP network
headers, and native netif lifecycle calls from returning to `app.c`.

The change preserves two sequential personality launches on one initialized
host while making deinitialization explicit after the final launch. Host tests
cover copied topology, invalid endpoints, and stale-state clearing. Clean
production links compare with Iteration 9 as follows:

| Engine | Iteration 9 flash | Iteration 10 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 310,596 B | 311,540 B | +944 B |
| NuttX | 355,908 B | 356,868 B | +960 B |
| Zephyr | 356,640 B | 358,028 B | +1,388 B |

The fixed guest pools and generated engine configurations are unchanged. The
extra static state is the immutable copied netfs topology; native netif storage
moved from the application into the host object rather than being duplicated.

## Iteration 11: host-scoped observability

The application still included LXP's sizing, latency, and FreeRTOS-port headers.
It walked compile-time slot/class bounds, sampled three process-global
registries independently, called the global coordinator heartbeat directly,
and selected a FreeRTOS-only guest-stack function. Besides coupling the demo to
internal capacity, the piecemeal teardown report had no contract preventing a
snapshot while those registries were changing.

Canonical LXP now exposes one versioned `lxp_host_observation_t`. It copies run
health, exact build sizes, world-checkpoint health, optional latency service and
wake rows, and a normalized guest-task stack result. The snapshot requires an
initialized, quiescent host and returns `LXP_ERR_BUSY` during an active run.
Latency arrays compile out with the recorder, so the normal profile does not
pay their stack cost. Service rows carry stable class identifiers rather than
exporting the registry bounds to consumers.

Native guest-stack introspection is now an optional OS-port operation and the
OS-port ABI is 11. FreeRTOS publishes its existing 768-byte trampoline-stack
high-water mark through that generic operation; its public engine-specific
accessor is removed. NuttX and Zephyr report the metric as unavailable until
their kernels can provide an equivalent bounded aggregate.

oveRTOS translates the live heartbeat and quiescent record through
`ove/lxp_observability.h`. The demo retains watchdog decisions, the host
deadline monitor, text formatting, and its own native-thread/heap accounting,
but it no longer includes LXP diagnostic/latency/configuration or RTOS-port
headers. The ownership ledger rejects those dependencies if they return.

All seven canonical LXP project targets, the sanitizer variants, and all 46
oveRTOS host/structural targets pass. The FreeRTOS diagnostic profile also
links with latency recording enabled, exercising both the copied latency rows
and normalized port stack metric. Production links compare with Iteration 10
as follows:

| Engine | Iteration 10 flash | Iteration 11 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 311,540 B | 311,900 B | +360 B |
| NuttX | 356,868 B | 357,172 B | +304 B |
| Zephyr | 358,028 B | 358,200 B | +172 B |

The facade, snapshot path, and OS-port callback plumbing remain small on all
three engines. The fixed guest pools and generated engine configurations are
unchanged.

## Iteration 12: host-owned rootfs workspace

The application still allocated LXP's 512-entry CPIO index, selected pathname
workspace sizes, carried an STM32 NuttX early-memory exception, and passed four
storage/capacity fields into the host facade. Those are generic host bootstrap
mechanics rather than demo policy.

`ove_lxp_host_t` now owns both fixed workspaces and passes them to canonical
LXP's caller-owned, zero-heap parser contract. The application supplies only
the rootfs image. Hidden generated configuration centralizes the 512-entry and
15 KiB defaults while retaining the measured 12 KiB STM32 NuttX pathname bound.
The general pathname workspace leaves more than 4 KiB beyond the measured
rootfs. Workspace members precede runtime state inside the host object,
preserving the previous BSS order on STM32.

Host initialization and teardown reset only the core and native interface
state. They deliberately do not clear the 20–23 KiB workspace: CPIO parsing
overwrites the live prefix and the published file count bounds every later
read. Unit tests preserve this property, verify the exact workspace passed to
LXP, and the ownership ledger rejects application-side rootfs allocation or
capacity knowledge.

Clean production links compare with Iteration 11 as follows:

| Engine | Iteration 11 flash | Iteration 12 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 311,900 B | 311,844 B | -56 B |
| NuttX | 357,172 B | 357,188 B | +16 B |
| Zephyr | 358,200 B | 358,528 B | +328 B |

The 15 KiB general pathname bound also recovers 1 KiB of FreeRTOS RAM. Zephyr
remains at 251 KiB (98.05%); the smaller bound prevents its address-derived
kernel-object table from pushing the following no-init region across another
4 KiB alignment boundary. NuttX retains the same 231,076-byte SRAM1 use.

## Iteration 13: facade-owned launch policy

The demo still constructed canonical `lxp_launch_config_t` objects, interpreted
`lxp_guest_exit_info_t`, and switched on `LXP_EXIT_REASON_*`. The oveRTOS host
facade therefore owned immutable composition but leaked LXP's per-launch ABI
back into the application.

`ove/lxp_launch.h` now defines the application-facing callback, launch, run
result, and guest-exit contracts. `ove_lxp_host_run()` translates every field
into a local canonical launch configuration. It also copies each exit scalar
and explicitly maps the exit reason before invoking application policy. The
guest command name remains valid only during the callback, matching the source
record's lifetime without allocating.

Canonical LXP now carries a caller context with its guest-exit callback. That
small contract correction lets the facade identify the synchronous launch
without a process-global active-config pointer. Core host and coordinator tests
verify context propagation. oveRTOS adapter tests verify launch-field copying,
exit-record translation, and preservation of unrelated policy when the system
console is bound. The ownership ledger rejects direct canonical launch or exit
types if they return to `app.c`.

Clean production links compare with Iteration 12 as follows:

| Engine | Iteration 12 flash | Iteration 13 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 311,844 B | 312,020 B | +176 B |
| NuttX | 357,188 B | 357,356 B | +168 B |
| Zephyr | 358,528 B | 359,104 B | +576 B |

The translation adds no fixed RAM: FreeRTOS remains at 241,040 bytes of RAM,
NuttX at 231,076 bytes of SRAM1, and Zephyr at 251 KiB. The temporary canonical
launch object and translated exit record live on the coordinator stack only
during their synchronous calls.

## Iteration 14: LXP-owned SVC metrics

The RT-scope application still included canonical Linux syscall numbers and
the Zephyr port header, duplicated a compact syscall-name switch, and consumed
Zephyr and FreeRTOS diagnostic record types directly. More fundamentally, the
three LXP ports sent every SVC sample back through an embedding-owned recorder,
contradicting the ownership rule that LXP owns reusable trap accounting.

Canonical LXP now owns the windowed and lifetime SVC accumulator plus its ARM
EABI diagnostic-name table in `lxp_rt_metrics`. Each production port records
directly after taking its cycle endpoint. The obsolete recorder callback was
removed from all three port configurations and their explicit ABI versions are
2, so an old embedding fails its size/version check rather than silently
retaining split ownership.

The OVE metrics facade copies canonical SVC records into its stable public
contract and supplies the engine-owned counter frequency. It also hides the
optional Zephyr critical-section and FreeRTOS thread-snapshot records behind
capability-returning functions. `rt_scope.c` therefore contains no canonical
LXP include, syscall constant, port metric type, or FreeRTOS-only metrics
header. Its native IRQ attachment and NuttX scheduler-lock probe remain because
they define the board experiment rather than reusable personality mechanics.

Canonical unit and sanitizer suites cover window rotation, coherent lifetime
sampling, maximum attribution, and syscall naming. oveRTOS tests cover facade
translation and unsupported optional metrics, while the ownership ledger
rejects any return of the removed application dependencies.

The canonical name lookup uses a compact descriptor table rather than the
compiler's sparse switch table. Optional native formatters remain selected at
build time, so an engine does not link report code for a capability it lacks.
Clean production links compare with Iteration 13 as follows:

| Engine | Iteration 13 flash | Iteration 14 flash | Delta |
|---|---:|---:|---:|
| FreeRTOS | 312,020 B | 310,940 B | -1,080 B |
| NuttX | 357,356 B | 356,076 B | -1,280 B |
| Zephyr | 359,104 B | 356,996 B | -2,108 B |

FreeRTOS uses 241,048 bytes of RAM (+8 bytes), NuttX uses 231,044 bytes of
SRAM1 (-32 bytes), and Zephyr falls from 251 KiB to 247 KiB. Zephyr's larger
apparent RAM recovery comes from keeping the following address-derived kernel
object table below a 4 KiB alignment boundary after the optional formatter and
its stack frame are removed.

## Iteration 15: opaque host storage

The public `ove_lxp_host_t` still exposed canonical `lxp_file_t` and
`lxp_host_t` members even though applications only allocate the object and pass
its address back to the host facade. That representation leak let application
code acquire canonical host internals and made the public oveRTOS header depend
directly on LXP's host header.

`ove_lxp_host_t` is now pointer-aligned, caller-owned opaque storage. It keeps
the existing zero-heap lifecycle and exact per-configuration footprint: there
is no singleton, allocation, indirection, or conservative maximum-size reserve.
A private common-backend representation contains the rootfs index, pathname
workspace, canonical host, and native interface state. Only the host and
observability adapters may translate the public storage to that representation.

The public size expression records the two canonical storage ABI facts needed
to allocate the object without including canonical types. Private compile-time
assertions compare those facts, the complete representation size, and its
alignment against the real LXP types. A canonical layout change therefore
fails the oveRTOS build instead of silently overflowing opaque storage. Runtime
reset still leaves the large rootfs workspace untouched, and initialization
still overwrites only its live prefix.

Host tests verify that the canonical host and both rootfs workspaces reside
inside the public object, that initialization and launch address the same
private host, and that teardown preserves workspace bytes. The ownership
ledger rejects canonical LXP host types in the public header and rejects any
application inspection of opaque host storage. The separate public
observability aliases remain a visible canonical dependency and are deliberately
left for a later boundary iteration. A production build of the supported
FreeRTOS minimal profile also caught and fixed an existing unguarded network
helper, so the same host facade now compiles with networking disabled.

Clean production links retain the exact host-object, flash, and fixed-RAM
sizes. The representation boundary therefore has no runtime or storage cost:

| Engine | Iteration 14 flash | Iteration 15 flash | Delta | Host object | Fixed RAM |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 310,940 B | 310,940 B | 0 B | 23,816 B | 241,048 B |
| NuttX | 356,076 B | 356,076 B | 0 B | 20,740 B | 231,044 B |
| Zephyr | 356,996 B | 356,996 B | 0 B | 23,812 B | 247 KiB |

## Iteration 16: OVE-owned observability records

The public observability facade still included `lxp/lxp_observe.h` and aliased
all canonical snapshot, diagnostic, and latency structures. Applications did
not call an LXP function directly, but the public OVE contract nevertheless
inherited canonical field layout, feature gates, slot bounds, service-class
bounds, and ABI changes. That was the remaining explicit representation leak
called out by Iteration 15.

`ove/lxp_observability.h` now defines versioned OVE-owned records for run
health, size accounting, diagnostic errors and health, guest stack use, and
optional latency rows. The common backend takes one canonical quiescent
snapshot into private temporary storage, validates every nested record version
and size, and copies fields into the public record. Latency recording similarly
copies an OVE histogram into a private canonical value, invokes LXP's canonical
bucket algorithm, and copies the result back. Neither path relies on compatible
structure layout or a representation cast.

OVE bounds the copied latency contract at eight buckets, fifteen service rows,
and sixteen guest wake rows. Private compile-time assertions bind those limits
to the current canonical ABI, complete service-class set, supported maximum
slot count, and matching latency feature gates. Runtime count checks fail
closed and clear the destination before any oversized canonical record is
exposed. The ownership ledger also rejects canonical includes, type names, or
constants if they return to the public header.

The normal profile compiles latency arrays out, just as before. Diagnostic
collection temporarily holds one additional canonical snapshot on the caller's
stack after a run has become quiescent; it adds no fixed allocation and no work
to the active coordinator path. Host tests cover field-by-field independence,
live-health normalization, canonical latency delegation, busy/error clearing,
ABI rejection, and capacity rejection.

The extra translation text also exposed a Zephyr userspace build instability:
absolute device-object addresses are inputs to the generated gperf hash, and a
small text shift can produce a larger prebuilt table. Zephyr's default 100%
reserve then moved BSS across a 4 KiB MPU-alignment boundary. The STM32 board
now reserves 50% instead. That covers every measured final/prebuilt table ratio
(the worst was 1.23), preserves link-time overflow failure, and removes the
address-dependent 4 KiB RAM jump. Minimal, Full, and Diagnostic all link with
the bounded reserve. Hardened still reaches its pre-existing, independently
reproduced 48,240-byte SDRAM overflow before kobject generation; that profile
budget defect is not caused by the observability boundary or reserve setting.

Clean Full-profile production links show that the owned record contract adds
only translation text. Fixed RAM and the public host object are unchanged;
Zephyr's smaller flash span is the removed excess kobject reserve:

| Engine | Iteration 15 flash | Iteration 16 flash | Delta | Host object | Fixed RAM |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 310,940 B | 311,244 B | +304 B | 23,816 B | 241,048 B |
| NuttX | 356,076 B | 356,388 B | +312 B | 20,740 B | 231,044 B |
| Zephyr | 356,996 B | 356,432 B | -564 B | 23,812 B | 247 KiB |

## Iteration 17: personality-neutral thread snapshots

The generic `ove_thread_info` record still contained an `lxp_slot` member even
though no RTOS backend could populate it: FreeRTOS, NuttX, Zephyr, POSIX, and
WASM all wrote `-1`. The private LXP adapter already resolved each opaque native
thread identity through the run-scoped slot lookup and wrote the result into
LXP's own `lxp_thread_info`. The public member was therefore redundant
personality state in an otherwise engine-neutral API.

The member is removed from the C contract and from the zero-copy C++ alias and
Rust wrapper. RTOS backends now publish only native identity and scheduling
statistics; the private adapter remains the sole owner of identity-to-slot
attribution. A migration-ledger check rejects a future LXP slot field or comment
in the generic header. Adapter tests retain both the owned-slot and no-owner
cases, proving that attribution is derived rather than copied from host data.

Verification also exposed that the C++ and Rust POSIX test stubs linked the
filesystem backend without the common media-ownership implementation it calls.
Both test compositions now include `ove_media.c`. The checked-in docs.rs Rust
FFI stub was regenerated from the public headers, bringing earlier console,
filesystem, media, and block additions back into sync as well as removing the
thread member.

Clean Full-profile STM32 links show no fixed-RAM or host-object growth. The
small flash changes are compiler/layout effects from the narrower copy path:

| Engine | Iteration 16 flash | Iteration 17 flash | Delta | Host object | Fixed RAM |
|---|---:|---:|---:|---:|---:|
| FreeRTOS | 311,244 B | 311,244 B | 0 B | 23,816 B | 241,048 B |
| NuttX | 356,388 B | 356,380 B | -8 B | 20,740 B | 231,044 B |
| Zephyr | 356,432 B | 356,264 B | -168 B | 23,812 B | 247 KiB |

All 46 C tests, 262 C++ tests, 304 Rust tests, and 436 ASan/UBSan tests pass.
Committed images were then flashed and inspected through the Linux guest over
SSH. `top` continued to attribute host workers by native identity on every
engine. The short RT smoke windows recorded zero misses, zero late finishes,
and zero IRQ overruns:

| Engine | Releases / executions | Dispatch average | Dispatch maximum |
|---|---:|---:|---:|
| FreeRTOS | 19,777 / 19,777 | 7.667 us | 74.722 us |
| NuttX | 12,249 / 12,249 | 7.963 us | 49.704 us |
| Zephyr | 25,251 / 25,251 | 7.778 us | 66.852 us |

## Iteration 18: OVE-owned native filesystem capacity

The generic NuttX filesystem backend included `lxp/lxp_config.h` solely to
size its aligned FAT DMA sector pool from `LXP_NHOSTFS_OPEN`. This was an
inverse ownership dependency: a reusable native backend knew which personality
would consume it and how large that personality's descriptor table happened to
be. FreeRTOS and Zephyr expressed the same requirement as independent literal
16-entry FatFs reservations.

`CONFIG_OVE_FS_MAX_OPEN_FILES` now defines the application-required number of
concurrent caller-owned native file handles. It defaults to four for ordinary
filesystem applications. The Full Linux profile explicitly requests sixteen,
matching LXP's external descriptor table, while a private adapter assertion
rejects either side drifting below that requirement. The setting is an OVE
filesystem contract rather than an LXP definition: NuttX uses it for aligned
per-file FAT DMA sectors, FreeRTOS for `_FS_LOCK`, and Zephyr for
`CONFIG_FS_FATFS_NUM_FILES`.

This does not enlarge the separate four-entry `ove_fs_open()` convenience
pool. High-concurrency clients use `ove_fs_open_init()` with caller-owned
storage, as the LXP adapter already does. It also does not size directory
objects: Zephyr's independent eight-directory pool is unchanged. The present
requirement is specifically the worst case in which all sixteen LXP external
descriptors are files.

The ownership ledger now rejects LXP includes or constants in every production
native filesystem backend, requires the adapter capacity assertion, and checks
that the FreeRTOS and Zephyr native reservations derive from the OVE setting.
Host verification passed 435 normal C assertions, 262 C++ tests, 304 Rust
tests, and 436 ASan/UBSan assertions.

The Full profile still reserves exactly the same resources as before: NuttX's
link contains seventeen aligned 512-byte FAT sectors (sixteen open files plus
the mounted volume), FreeRTOS's FatFs lock table contains sixteen entries, and
Zephyr resolves `CONFIG_FS_FATFS_NUM_FILES=16`. Consequently clean committed
STM32 links have no flash or fixed-RAM cost:

| Engine | Iteration 17 flash | Iteration 18 flash | Delta | Fixed RAM |
|---|---:|---:|---:|---:|
| FreeRTOS | 311,244 B | 311,244 B | 0 B | 241,048 B |
| NuttX | 356,380 B | 356,380 B | 0 B | 231,044 B |
| Zephyr | 356,264 B | 356,264 B | 0 B | 247 KiB |

Every committed image was flashed and reached the Linux guest over SSH. A Lua
probe opened sixteen `/data` files simultaneously, wrote and flushed each one,
closed every handle, and removed the test files successfully on all engines.
The accompanying light-load RT windows recorded no misses, late finishes, IRQ
overruns, or pending work:

| Engine | Releases / executions | Dispatch average | Dispatch maximum |
|---|---:|---:|---:|
| FreeRTOS | 114,753 / 114,753 | 7.630 us | 81.111 us |
| NuttX | 45,657 / 45,657 | 7.926 us | 51.389 us |
| Zephyr | 45,502 / 45,502 | 7.778 us | 50.370 us |

## Iteration 19: run-scoped FreeRTOS tick ownership

The generic oveRTOS FreeRTOS tick hook included LXP's port header and called
`lxp_freertos_tick()` whenever `CONFIG_OVE_LINUX` was compiled in. That made a
reusable RTOS backend depend directly on one personality implementation, kept
the personality callback reachable outside a run, and left callback lifetime
implicit rather than part of the FreeRTOS port contract.

oveRTOS now owns a synchronized single-subscriber tick seam. Subscription and
withdrawal run in task context under a FreeRTOS critical section, so the
32-bit callback publication is atomic with respect to SysTick and withdrawal
cannot return while a callback is executing on this single-core target. The
tick ISR loads the optional callback once and invokes it indirectly. It neither
includes an LXP header nor names an LXP symbol, and the ownership ledger rejects
either dependency returning.

Canonical LXP's FreeRTOS configuration ABI is version 3. Its port requires the
embedding host's subscribe/unsubscribe operations, publishes its private
guest-slicing callback from per-run `prepare()`, and withdraws it from
`teardown()`. Quantum state is explicitly cleared at both boundaries, including
failed prepare cleanup, so sequential runs cannot inherit a partly consumed
budget. The standalone QEMU embedding implements the same contract rather than
calling the port callback permanently from its board hook.

The canonical host contracts and multi-process M2 FreeRTOS QEMU fixture pass.
oveRTOS verification passed 435 C assertions, 262 C++ tests, 304 Rust tests,
and the ASan/UBSan suite, while all three STM32 production compositions build.
Only FreeRTOS gains code for the subscriber and indirect dispatch; NuttX and
Zephyr remain byte-identical to Iteration 18 and no engine gains fixed RAM:

| Engine | Iteration 18 flash | Iteration 19 flash | Delta | Fixed RAM |
|---|---:|---:|---:|---:|
| FreeRTOS | 311,244 B | 311,492 B | +248 B | 241,048 B |
| NuttX | 356,380 B | 356,380 B | 0 B | 231,044 B |
| Zephyr | 356,264 B | 356,264 B | 0 B | 247 KiB |

The committed FreeRTOS image was flashed and exercised exclusively through
the Linux guest's SSH service. Two bounded, compute-only Lua processes both
completed correctly with global FreeRTOS time slicing still resolving to zero.
The accompanying RT snapshot recorded 24,442 releases and executions, zero
misses, late finishes, IRQ overruns, or pending work, a 7.463 us average
dispatch, and a 57.000 us maximum.

This validation exposed a separate pre-existing proportional-share limitation.
FreeRTOS's highest-priority selection advances through an equal-priority ready
list on every scheduler selection, independently of `configUSE_TIME_SLICING`.
Consequently a frequently waking higher-priority host task, such as the 1 kHz
RT-scope worker, can rotate guest tasks before LXP's weighted quantum expires;
nice -20 and nice 19 measured 396 and 392 CPU ticks in that condition. The
run-scoped callback preserves forward progress and isolation from unrelated
equal-priority host tasks, but strict weighted shares under host preemption need
a later scheduler-seam correction rather than an ownership-layer workaround.
