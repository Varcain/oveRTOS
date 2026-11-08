# Event group construction and context contracts

Run `make test-eventgroup-contracts` and
`make test-eventgroup-contracts-sanitize` for both heap modes. The gates verify
complete native output and exit status, preserve individual observations and
include an early failing canary. Set `OVE_EVENTGROUP_CONTRACTS_CANARY=1` to check
that a deliberately failed gate reaches the CLI and Make.

Provider observers compile the actual initialization and common heap helper
against inert SDK/result spies. They check publication, primary error mapping,
staged cleanup, allocation failure and reuse without creating kernel objects.
POSIX initialization is genuinely fallible. The FreeRTOS NULL and NuttX error
cases are declared-interface controls: the pinned implementations do not fail
for these valid arguments. Zephyr initialization is void and has no invented
error path. Model SDK record sizes are complete host objects, not target ABI
measurements. Ordinary POSIX integration uses real mutexes/condition variables,
valid task operations, notification and quiescent repeated destruction.

The context matrix uses EventGroup alone, EventGroup plus TIME and feature-off
headers in both modes. It reuses the native context observer and substitutes
only the explicit architectural register reads. Unsupported-domain observations
remain distinct from actual target interrupt evidence. Disabled profiles test
header fallbacks without referencing private provider state that is compiled
out. Provider source selection is also checked separately by real target builds.

The gates do not exercise actual deferred FreeRTOS ISR work or certify its
lifetime, scalar validation, Rust storage ownership or async registration.
Those require separate native and binding repairs. No duplicate native init,
stale queued pointer, uninitialized pthread operation or field race is used as
a regression witness. Sanitizers cover the actual host C artifacts; target
firmware and hardware claims require their own validation.
