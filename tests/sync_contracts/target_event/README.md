# Event native integration controls

These applications are build-only. Each language defines two typed function
pointer roots: an ordinary task consumer that constructs, signals, waits on
and closes an Event, and a separate ISR consumer for an already live Event.
Application entry calls neither helper. Keeping the ISR helper separate
preserves the C++ requirement that its ISR method runs only in an ISR.

The linker must retain `event_compile_task_anchor`, `event_compile_isr_anchor`
and `event_native_domain_values`. Audit that every root is defined, points to
the actual language helper, and reaches the real native Event implementation.
An undefined linker root or constructor-only link is insufficient.

The common C values record the actual storage layout, native word size, port,
wrapper, interrupt-priority threshold and tick rate. Each workspace must retain
its generated configuration and binding/storage provenance. Compile success
does not qualify arbitrary interrupt priorities, unprivileged MPU calls,
stronger application masks, Linux behavior or runtime ownership.

The Rust static-storage ownership API and other binding lifecycle guarantees
require their own review. These unexecuted helpers make no new such claim.
