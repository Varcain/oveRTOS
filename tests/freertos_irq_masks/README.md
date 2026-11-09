# FreeRTOS IRQ boundary checks

`make test-freertos-irq-masks` checks the actual C adapter against initialized
scalar register and native-nesting models. Its sanitizer variant adds ASan,
nonrecovering UBSan and unrestricted LSan. The 504 observations cover paired
normal/failure controls, ordinary and MPU V1 configurations, both allocation
configurations, admitted task/external/system callers, exact mask restoration,
transition envelopes and early unsupported-context/configuration rejection.

`make test-freertos-irq-bindings` checks the actual Rust crate's `SpinMutex::with`
and global `critical_section::with` entry paths. The 336 observations use fresh
Rust data and the same scalar C adapter model. The sanitizer variant instruments
Rust and C. Opaque-storage sizes are measured from the host test backend for
bindgen; these tests exercise a u64 IRQ key and a Rust u32, not native object
storage. Unsupported roles exist only as initialized host scalars and exit
through an observer before a successful key or callback.

The `OVE_FREERTOS_IRQ_MASKS_CANARY` and `OVE_FREERTOS_IRQ_RUST_CANARY` environment
variables, when set to `1`, deliberately fail ordinary expected-success rows.
The runner, CLI and Make must propagate that failure after later controls run.

The native fixture is separate. Build without starting any guest:

```sh
python3 tests/freertos_irq_masks/target.py output/tests/freertos_irq_masks/native \
  --freertos <pinned-kernel> --toolchain <arm-toolchain> \
  --qemu <qemu-system-arm> --build-only
```

Before execution, inspect its ELF/disassembly, compilation commands and
`eligibility.json` files. Check the legitimate SVC return addresses, actual
vectors, caller roles, MPU Normal-memory attributes, aligned disjoint backing
and lock-free atomics. Prepare an external JSON object mapping each profile
label to its inspected ELF SHA256. Execute the unchanged artifacts using the
same command with `--skip-build --approved-images <map.json>` instead of
`--build-only`. The controller requires exact hash matches and bounds all owned
stages with a cumulative budget. Also bound the entire controller with a process
owner that cleans up its descendants, including on interruption.

The 20 native observations use real static tasks in ordinary and MPU V1 ports,
both allocation configurations, raw-critical options and syscall enforcement
settings. Heap configuration does not imply dynamic task construction. MPU V1
erratum-workaround code is checked with host models and ARM objects; this QEMU
CPU is not a runtime witness for that workaround. Alternate providers require
`--all-callers-privileged` in both the controller and CMake. Never run an old
unsupported restricted path or a deliberately masked required SVC as a native
negative control.

The harmless above-threshold observer demonstrates the documented native-body
limitation: an unadapted native critical call may relax a stronger BASEPRI to
the kernel threshold. Matching public release must still restore the incoming
mask. This is an exclusion/restoration check, not a physical latency bound or
an all-interrupt exclusion guarantee. See the public `ove/irq.h` contract.
