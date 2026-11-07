# Synchronization compile controls

These four applications retain the ordinary binding constructors and a C
context-query reference for inspection of generated configuration and linked
symbols. A raw checked-wait thunk in each language also checks the generated
function signature and bool-output ABI. Application entry starts the ordinary
runtime; it never invokes either helper. These fixtures are built for inspection
and are not run. Raw ABI thunks are test integration points, not application use
examples.

Use the existing explicit workspace and `.ove-app-path` source override with a
fresh normal `output/<board>/<rtos>/<profile>` workspace. Normalize the copied
configuration to SYNC with ASYNC, TIME, CONSOLE and INFER disabled. Each language
and allocation profile uses its own generated native storage measurements.
Zephyr core variants require their corresponding native Kconfig and size data.

Set the following `LDFLAGS` before the initial CMake configuration:

```
-Wl,--undefined=sync_compile_query_anchor -Wl,--undefined=sync_compile_consumer_anchor -Wl,--undefined=sync_compile_domain_values -Wl,--undefined=sync_compile_checked_wait_anchor
```

Check that all four anchors are defined in the final ELF, the consumer pointer
refers to the language's constructor helper, and the checked-wait pointer retains
its language thunk and the native checked entry. An undefined symbol alone
does not prove retention. A changed environment requires a fresh CMake cache.

The domain object uses complete actual SDK types. Its retained scalar array
records count widths/ceilings and native storage size/alignment without calling
a native initializer. `OVE_SYNC_EXPECTED_BITS` and `OVE_SYNC_EXPECTED_MAX` allow
separate compile-negative controls with named assertion diagnostics.
