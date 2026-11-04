# Keyword model qualification

Run `make test-infer-target` (or `ove test infer-target`) to build and qualify
both original keyword models with the reference kernels on QEMU Cortex-M7.
The lane uses the normal in-tree FreeRTOS workspaces, leaves the active
configuration unchanged, and bounds each emulator process to 45 seconds.
It uses a separate finite entry without the dashboard, audio, networking or
shared simulation files. This is model qualification, not a whole-app or
physical-board test.

For each seed 0 and 17, a new preprocessor session receives 49 complete windows
of 480 initialized int16 samples. Sample `i` in frame `f` is
`100 + ((i + f + seed) % 31)`. The 49 output rows of 40 int8 features become
the classifier's complete `[1,1960]` input. The test compares every feature
byte and all four int8 scores exactly. This small deterministic corpus checks
implementation consistency; it does not measure speech-recognition accuracy.
Caller-owned initialization runs in both heap configurations. The heap build
also repeats the sequences through native creation and destruction.

The configured arena stays 32768 bytes. Output records report maximum queried
usage, capacity and front padding separately for each model/session. Usage must
be positive and leave at least 16 bytes. Final usage does not bound every
preparation temporary; retain the separately qualified preparation capacity.
The reference Cortex-M7 build with GCC 15.2.1 measured the following values
for both seeds and both allocation modes (including heap-backed creation):

| Model | Capacity | Queried usage | Remaining bytes |
| --- | ---: | ---: | ---: |
| Audio preprocessor | 32768 | 8464 | 24304 |
| Keyword classifier | 32768 | 6804 | 25964 |

Front padding was zero. These are observations for this target/compiler/provider;
physical-board execution and other supplied CMSIS-NN revisions need their own
qualification. All eight shipped language/mode manifests retain the same 32768
budget. Compiler allocation layouts and model bytes are checked separately
across their FreeRTOS, NuttX and Zephyr builds.

Never shrink an arena until initialization fails. Requalify on the actual target
compiler and provider after changing models, TFLM, configuration or toolchain.
Language bindings share the native allocator; their lifetime/asset correctness
requires separate checks. In particular this fixture does not start the deferred
Zig asset-pointer path.

Before launching QEMU, the controller checks original model hashes, schema,
actual SignalRfft options and shapes, the clean manifest-pinned SDK, source
selection, linked model bytes/alignment, ARM ELF, compiler and input leases.
The shipped FFT length is 512; only factors 2, 3 and 5 are permitted, and the
real FFT additionally needs even length. Negative tests only change checker
metadata or expected output; they never execute unsupported FFT programs or
use undersized arenas.

## Updating goldens

After reviewing a trusted asset/toolchain change and qualifying its preparation
capacity, update the manifest identities and fixture descriptors as necessary,
then explicitly capture reference outputs:

```sh
.venv/bin/python tests/infer_target/run.py --regenerate-goldens
make test-infer-target
```

Capture uses the same firmware as verification. It requires complete records,
clean exit, headroom and agreement across init/create and both heap modes before
writing `goldens.json`. Review that diff. CI never regenerates goldens. The
manifest records original model and TFLM identities, input recipe, exact bytes
and reference compiler provenance. A model/SDK mismatch stops before execution.

For staged inspection, `--build-only` writes `qualification-artifacts.json` in
each workspace; `--skip-build` requires those exact leased inputs. Build logs,
`qualification.log` and `qualification-result.json` remain in the workspaces.
Semihosted guest records go to a dedicated file chardev. QEMU diagnostics go to
`qualification-qemu.log`; only its exact disconnected-board-NIC warning is
allowed. The guest protocol remains strict even when host diagnostics occur.
Golden comparison reports the seed, frame and byte index of a mismatch.

## Optional CMSIS-NN on ARM

No shipped configuration enables CMSIS-NN, so the CI lane uses the reference
provider. Users enabling it can supply compatible existing, clean CMSIS-NN
sources and CMSIS-Core headers explicitly:

```sh
.venv/bin/python tests/infer_target/run.py \
  --cmsis-nn-dir /path/to/CMSIS-NN \
  --cmsis-core-include /path/to/CMSIS/Core/Include
```

This target compiles upstream NN through its own CMake project, links the
unmodified pinned TFLM wrappers, and compares the same goldens without tolerance.
It requires an actual Cortex-M7 DSP build and verifies that the classifier's
fully-connected call reaches a linked vector/matrix implementation containing
DSP instructions. Some operations can still select target scalar paths.
It is not a host fallback test. Caches are separate from reference builds.
No CMSIS manifest entry, automatic NN download, private SDK patch or native
CMSIS CI profile is added. Each supplied revision/provider needs its own
preparation-capacity and output qualification. Capture is reference-only.
