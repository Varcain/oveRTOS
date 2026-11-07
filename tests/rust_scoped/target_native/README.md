# Finite native scoped API control

This AN500 test links the actual zero-heap Rust Mutex, RecursiveMutex and
CondVar bindings without a Rust allocator. A native C controller owns three
static 128 KiB worker/coordinator stacks, startup gates and completion checks.

Two workers exercise forgotten access views, a real condition-variable timeout
and nested recursive scopes. Ten ordinary and twelve recursive probes require
current-worker native ownership at the callback boundaries. Before worker exit,
unexpected ownership is recorded as failure and retired on that same worker.
No deleted task is inspected as a lock owner. Zephyr joins each worker; FreeRTOS
uses worker priority above the coordinator so self-deletion precedes the next
coordinator dispatch. A second initialization round checks reopened typed static
storage and stable backing addresses.

The fixture wraps native mutex initialization to observe successful handles.
It also replaces the optional simulation initializer, counting exactly one call,
to exclude global host shared-memory files and its debug worker. Native reset,
board main, application entry and scheduler startup remain active. Zephyr's
separate shared-memory display initializer must be disabled. Compile guards
require single-core SYNC without TIME, ASYNC, CONSOLE or transport consumers.

Build normal and `OVE_SCOPED_NATIVE_CANARY=1` images separately. Both complete
the same finite protocol; the canary retains one early named failure and exits
with status 1. The normal footer requires zero failures and retirements, two
worker returns/completions, four steps, two initialization rounds, exact probe
counts and one isolated-start call. The host controller must check the complete
guest transcript, actual emulator status and bounded process cleanup.

Inspect actual native configuration, linker maps, stack/frame records, startup
roots and output routing before running a new image. Requested stack sizes and
live-stack checks alone do not prove arbitrary reachable workloads fit. The
test permits platform libc heap initialization; its actual Rust scoped body
and linked call paths perform no allocation. It does not qualify general
interrupt delivery, priority inheritance or other boards and RTOS domains.
