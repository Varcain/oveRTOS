# FreeRTOS condition-variable context validation

Run `make test-freertos-cv-context`, `make test-freertos-cv-context-sanitize`
and `make test-freertos-cv-context-arm` from the repository root.

The host observer includes the actual adapter and private record type. All
task tokens, mutex states and foreign automatic records are initialized and
retained. Scalar spies count illegal raw-critical attempts and model V1 option1's
return to caller mode. They check notification/trace nesting, caller mode before
release/take/reacquisition, checked ownership, retirement, status and continuous
effective mask preservation, including the CM7 entry sequence. Host reads are
not native MPU evidence; source and emitted instruction ordering establish which
field accesses belong to each protection scope. The foreign records model two
newer publications; existing joined CV tests separately cover actual invocation
lifetime interleavings. Every live-frame repair is a failure, with snapshots
cleared before expiry. Whole-retirement-omission mutations are ineligible on the
release-failure path because they remove its required pre-return checkpoint.

The native gate uses ordinary CM7 and V1 kernels, both allocation modes,
option0/1 and syscall enforcement0/1. Higher-priority native workers reach a
verified blocked state after publication and mutex release. The CV and mutex
use static `_init` backing in both configurations; the heap-enabled profiles
do not exercise heap constructors. Each restricted
task receives only its own stack and the shared CV/mutex/state region. A distinct
restricted producer signals/broadcasts; an older waiter retires through a newer
still-live private stack. Native caller-side linker wrappers observe mode and
enabled masks before semaphore give/take and notification take. Privileged
mask controls use a harmless above-threshold interrupt marker, then explicitly
unmask outside the subject call. Cleanup records failures before legitimately
waking remaining workers, waits for public returns and suspension, and only
then deletes tasks and deinitializes objects. Stack headroom is a bounded
instrumented observation, not a whole-call-chain guarantee.

Actual trace controls keep all workers privileged. Restricted trace-storage
access, arbitrary V2, native64, physical interrupt latency and fault recovery
are not qualified. The V1 runtime uses workaround0 as required by QEMU's CPU
revision; workaround1 requires separate source/model/object qualification.
SDK and production configuration bytes are unchanged.

Alternative native provider/header paths require `--all-callers-privileged`
and the corresponding CMake role flag. Every old CV native caller, including
waiters and recovery paths, must stay privileged after independent source
eligibility review. Never run old restricted CV paths or native lifetime
mutations as a failing witness. Faults, setup rejection, invalid exit or an
incomplete footer stop the cohort immediately; they are never semantic evidence.

Both host and native runners retain complete log/status/binary identities and
use owned process deadlines. The host gate completes normal and harmless early
failure controls for every profile. `OVE_FREERTOS_CV_CONTEXT_CANARY=1` and
`OVE_FREERTOS_CV_CONTEXT_TARGET_CANARY=1` make the corresponding gate fail while
still completing its later controls. Alternative host providers are selected
with `--provider`; they execute only against the inert initialized spies.
