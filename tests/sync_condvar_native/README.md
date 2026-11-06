# Native condition-variable boundary tests

Run the guarded models with `make test-sync-condvar` and
`make test-sync-condvar-sanitize`. Setting `OVE_SYNC_CONDVAR_CANARY=1`
selects explicit failure propagation; that invocation must fail.

The host tests compile the production FreeRTOS and Zephyr adapters against
complete, initialized SDK model objects. They exercise both allocation modes.
They do not execute either native kernel or establish target storage layouts.

The FreeRTOS model schedules selected single-core notification and timeout
interleavings. It records whether a waiter could return while a notifier still
needs its node or task, then retains the live frame until those uses finish.
A separate check observes and removes any still-linked live waiter before its
frame returns. These interventions are counted independently; they do not
turn a failing adapter into a passing test. Notification counts, committed
zero results, critical nesting, caller-mutex ownership and last-use ordering
are modeled explicitly. A notifier-held caller mutex is a negative control.

The Zephyr model distinguishes provider and adapter mutex acquisitions. Clean
timeout must return with one held level; successful wake must not add another
level. Unexpected error cases deliberately model one unheld state. They check
error classification without claiming every native error has that lock state.
Ownership is observed before valid cleanup restores any missing held level.

The C++ observers use initialized external tokens and logical ownership only;
they do not access a native mutex or protected payload. A recording-only
predicate observes attempted callback dispatch on the old header. On a repaired
header, the linked abort call reaches an observer endpoint that checks its
armed state and completes the fixture with `exit`, without invoking abort or
resuming the uncertain call. Ordinary success, clean timeout and Result-error
controls precede that final case. Both a release build and a returning custom
assertion handler exercise the unconditional fallback. Sanitizer shutdown still
runs at the observer endpoint.

Every failed check increments a process-wide counter that survives scenario
resets. A compiled fail-then-reset canary must return exactly one named failure.
Unexpected setup, synchronization or cleanup errors reject the entire process
with status 2; a fault, timeout or sanitizer diagnostic is never a regression
witness. Successful FreeRTOS host runs join every worker before resetting or
releasing backing.

`OVE_FREERTOS_SYNC_SOURCE` and `OVE_ZEPHYR_SYNC_SOURCE` select alternative
adapter source files for guarded parent or mutation comparisons. Any alternative
must preserve the source-to-model last-use annotations and be checked against
its emitted instruction order before execution. In particular,
`OVE_TEST_NEXT_BEFORE_NOTIFY` is only valid for a source whose broadcast reads
next before notification. It must not be enabled for the ordinary adapter.

Compiler-only objects also check shared error conversion with an absent,
aliased or distinct optional quota errno. `OVE_ERRNO_HEADER` selects an
alternative common header for compiler comparisons; these objects never link
into a model executable.

The separate `targets` programs use the actual Cortex-M7 FreeRTOS or Zephyr
kernel. Zephyr also provides a native_sim build configuration. They have persistent, statically backed higher-priority
workers, timeout and wake controls, notifier-held mutex controls and repeated
broadcast/reuse. The FreeRTOS program also tests an unrelated pending direct
notification. These programs require their own startup, whole-stack, backing
and launch qualification; host model results do not supply that evidence.
