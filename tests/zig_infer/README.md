# Zig inference assets and storage

`make test-zig-infer` builds the real binding in heap and zeroheap modes under
Debug, ReleaseSafe, ReleaseFast and ReleaseSmall. Recording-only C fakes check
arena extent, exact config forwarding, first-use errors, Busy preservation,
closed-object cleanup and reuse. They never parse a model, traverse advertised
arena spans, allocate native resources or start workers. Ordinary checks remain
active in release modes and accumulate across fake session resets; every reset
checks that earlier failures survive. Each case fails by exit status.

The asset consumer checks linked addresses, complete physical sizes and actual
16-byte alignment before forming bounded slices. It reads all bytes of both
original generated models and a small aligned control. Complete-array C
declarations supply independent allocation truth; generated payloads are also
compared to their `.tflite` inputs. No model program is invoked.

These are ordinary C builds plus the selected Zig mode's checks, without a
whole-Zig ASan claim. A separate private returning-Tracker observer establishes
the parent Debug close ordering without running its known panic. Permanent
fixed tests use the real binding and Tracker. Compiler-only linkage controls
and exact firmware artifact checks complement runtime tests; firmware builds
remain build coverage until their startup, stack and input domains are qualified.
