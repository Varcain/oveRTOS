# Native inference admission

`make test-infer-admission` runs heap and zero-heap builds. The real executable
uses the manifest-pinned TFLM/common backend and a complete aligned sine model.
Sequential controls invoke and close valid models. Five pair cases retain the
first session and all its backing forever, then require Busy from the second
constructor. Only an accepted second is ever invoked/destroyed; the first is
never used again. Those isolated processes intentionally retain resources.

The controlled executable compiles the same native backend with narrow TFLM
collaborators. A reservation in GetModel stops a wrongly admitted mutant before
placement can overwrite live objects. It tests candidate/output preservation,
allocation/setup failures, 16 competing constructors, marker consumption,
closed-header reuse and destructor/free tails. Nested heap probes fail their
first allocation if erroneously admitted; zero-heap probes use the guarded
setup boundary. Normal allocation paths really free memory. Invalid/untracked
free attempts are recorded and suppressed before free. This does not guard
arbitrary premature-header-free/read or double-free mutations.

`make test-infer-admission-sanitize` instruments both native/backend and TFLM
with ASan/UBSan. Leak detection stays enabled for ordinary cases and disabled
for the five intentional-retention processes. The committed runner enforces a
30-second deadline per executable and inherits the CLI's owned process group.

The atomic check/store mutation barrier is initialized only by the parallel
case; that mutation must run only there. Missing marker-null mutations must
stop before another session is opened, as the dedicated cases do. Mutant early
exits are named assertion observations, not leak-free controls.

These checks establish native singleton ownership only. They do not validate
arbitrary model graphs/arena budgets or repair the Rust tensor/lifetime API.
