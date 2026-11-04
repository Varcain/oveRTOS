# Inference runtime

oveRTOS compiles TensorFlow Lite Micro directly from the revision in
`manifest.yaml`. The selected reference, signal and optional CMSIS-NN sources
come from that unchanged checkout. The platform port, session wrapper and
arena/planner extensions are oveRTOS sources with project diagnostics enabled.

## Trusted model assets

Models are trusted build-time assets containing valid, supported programs.
TensorFlow's [model trust policy](https://github.com/tensorflow/tensorflow/blob/master/SECURITY.md#tensorflow-models-are-programs)
treats model graphs as executable programs. Alignment and schema checks address
integration mistakes; they do not make malformed programs safe.

Keeping the engine unchanged avoids maintaining and re-auditing private changes
to kernels, CMSIS glue and signal/kissfft internals whenever the manifest commit
changes. Model qualification instead covers the assets, inputs, provider and
memory budget together. TFLM publishes no packaged
[releases](https://github.com/tensorflow/tflite-micro/releases), so dependency
updates follow the manifest's commit pin; each revision needs a fresh source
and qualification check.

`models/convert.py` emits `const` arrays aligned to 16 bytes. Pass an array's
address and complete size and retain immutable bytes until the session closes.
External assets need the same guarantees. Unaligned bytes return
`OVE_ERR_INVALID_PARAM`; a schema mismatch returns `OVE_ERR_NOT_SUPPORTED`.
The backend neither copies nor generally validates model programs.

## Arena configuration

| Arena configuration | Behavior |
| --- | --- |
| Below the startup precheck minimum, including front padding | `OVE_ERR_NO_MEMORY`, before any SDK object is constructed |
| Persistent/preparation requirements met, but activation or planner capacity exhausted | `OVE_ERR_NO_MEMORY` |
| Between the startup minimum and the model's persistent/preparation requirement | Configuration error with undefined behavior |

In the third region, upstream kernels can use a failed allocation. A null-based
write can reach mapped RAM on targets such as STM32F7, whose ITCM is mapped at
address zero. A fault is not a reliable sizing diagnostic.

Start with a generously sized, qualified configuration on the actual target and
provider. Record `ove_model_arena_used()`: the interpreter's `arena_used_bytes()`
plus the original arena's front alignment padding. Keep the configured arena at
least that usage plus 16 bytes, while retaining its qualified preparation
capacity. Final usage can exclude temporary preparation peaks; it is not a
universal minimum. Do not shrink until setup fails. Re-measure and re-qualify
after model, provider or TFLM changes.

Both allocation modes use this contract. The original arena base is retained
for cleanup. Each admitted setup clears the effective aligned arena before
constructing SDK objects, at O(arena_size) cost, preserving its front padding.
This initializes unvisited node records for partial setup cleanup and provides
deterministic arena backing. Admission remains held through complete teardown
and owned frees. A nested constructor observes `OVE_ERR_BUSY`.

`OVE_ERR_NO_MEMORY` covers prechecks and detected resource failures. Other
`AllocateTensors()` failures return `OVE_ERR_ML_FAILED`. This distinction does
not make the undefined configuration region safe.

## Shipped-model qualification

`make test-infer-target` runs both original keyword models with reference kernels
on QEMU Cortex-M7 in both heap configurations. It compares all feature bytes and
all four classifier scores for fixed initialized input sequences, and prints
arena usage and front padding against each unchanged 32768-byte budget. It also
checks the actual model's RFFT options before execution. This finite corpus
checks implementation consistency, not recognition accuracy or arbitrary inputs.

The reference QEMU Cortex-M7 build with GCC 15.2.1 reports 8464 bytes for the
audio preprocessor and 6804 bytes for the classifier, with zero front padding.
Their remaining capacities are 24304 and 25964 bytes respectively. Both seeds,
caller-owned sessions in both heap modes and heap-backed creation agree. These
measurements qualify this build; other target/compiler/provider combinations
retain separate preparation-capacity and execution checks.

No shipped configuration currently enables CMSIS-NN. An opt-in ARM qualification
target accepts existing CMSIS-NN sources and Core headers and compares against
the same reference goldens without tolerance. It verifies the classifier's
linked DSP path; host scalar fallbacks do not qualify it. See the
[qualification instructions](https://github.com/Varcain/oveRTOS/tree/main/tests/infer_target)
for input recipes, explicit golden regeneration and supplied-provider commands.
Recheck allocation layouts and preparation capacity across target compiler/ABI
profiles; physical-board checks remain manual.

## Owned extension points

`OveArenaAllocator` derives from `SingleArenaBufferAllocator`. Before persistent
allocation it checks the request and worst-case alignment waste against
`GetAvailableMemory(1)`, which includes live temporaries. All allocation entry
points record failure in a monotonic exhaustion flag.

`OveMemoryPlanner` derives from `GreedyMemoryPlanner`. It checks entry capacity
before both `AddBuffer` overloads and checks the completed maximum plan against
arena capacity minus persistent usage before returning an offset from
`GetOffsetForBuffer`. This stops `CommitPlan` from forming an out-of-arena tensor
pointer before final reservation. Planner exhaustion sets the same flag.
Planner scratch occupies the available gap, so this comparison uses total
capacity and persistent usage rather than instantaneous available memory.

The allocator is tail-placed by copying a fully initialized temporary. The
planner is directly placement-constructed in tail storage. The wrapper then
uses the allocator-taking `MicroAllocator::Create` and `MicroInterpreter`
constructors. These extensions add arena footprint, included in reported usage;
they add no static RAM or separately owned heap allocation.

The conservative startup minimum charges each unchecked placement and its
worst-case alignment waste: the owned allocator, owned planner, `MicroAllocator`
and the builtin-data allocator in `StartModelAllocation`. The private builtin
footprint is recovered from `GetDefaultTailUsage(true)` by subtracting its
public summands; its alignment follows the pinned base/member layout. The next
SDK allocation, `InitScratchBufferData`, checks its result. This minimum protects
startup placement; it does not estimate a model's memory budget.

## Pinned upstream behavior

Recheck these behaviors whenever the manifest revision changes.

| Upstream behavior | Effect | oveRTOS handling |
| --- | --- | --- |
| Persistent tail is bounded by `head_` instead of `temp_` | Can overlap live temporaries near exhaustion | Owned allocator checks the live gap first |
| Allocator creation and `StartModelAllocation` use placement-new without a null check | Tiny arenas can reach unchecked construction | Minimum derived from all preceding unchecked placements |
| `NodeAndRegistration` records are not initialized and cleanup visits all records | Unvisited registrations can be stale after partial setup failure | Clear the effective arena before construction |
| Scratch handles are unchecked, some scratch-request statuses are ignored, and kernels use failed persistent allocations | Failure can crash or corrupt memory in the undefined configuration region | Qualified arena configuration and sizing procedure |
| Greedy bookkeeping can run out without an allocation failing | An allocation-only flag misses that resource failure | Owned planner capacity checks and shared exhaustion flag |
| `CommitPlan` forms pointers before final activation reservation | Oversized plans can publish addresses before capacity rejection | Check maximum plan size before returning offsets |
| CMSIS-NN results are consumed by `DCHECK` | No normal failure propagation; debug assertions may fire | Target output qualification for enabled providers |
| CMSIS convolution gets its temporary bias record with `AllocateTempOutputTensor` | Actual bias validation is skipped, while Eval reads the real bias | Trusted-model contract and qualification |
| Scalar multiplier/shift fields remain unset for int8/int16 | Unused scalar fields retain indeterminate arena contents | Arena clearing supplies deterministic backing |
| Generic kissfft radix requests scratch through the pinned null-returning allocation macro | Unchecked scratch use can cause a null-based write | Qualify RFFT lengths with prime factors only 2, 3 and 5 |
| Some Prepare error paths leave temporary tensors allocated | Temporary ownership remains on failed setup | Discard the failed session's arena |

The RFFT restriction applies to the pinned default build without SIMD or alloca
overrides. The original audio preprocessor uses length 512. Future signal
programs need their own qualification. There is no general protection against
malformed models or invalid numeric inputs.

## Updating TFLM

Before changing the manifest revision:

1. Recheck every behavior in the table against the new source.
2. Re-derive all unchecked placements and the minimum, including private
   footprint recovery and actual native/ARM alignments.
3. Recheck the public virtual methods used by both extensions, their placement,
   lifetimes and error behavior.
4. Measure final usage and preparation capacity for every shipped target and
   provider configuration, retaining configured headroom.
5. Re-run original-model target output qualification, both allocation modes,
   partial-failure cleanup and bounded sanitizer lanes.

See the [inference API](../api/infer.md) and
[binding contracts](binding-semantics.md#inference) for public interfaces.
