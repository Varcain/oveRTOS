The host fixture compiles the pinned NuttX initializer and the production CMake
integration. Its callback bounds use one real array and its one-past end. Nested
entry is finite even without the repair: it records duplicate calls and returns.
The controls check callback order, NULL entries, an empty table and repeated calls.
The concurrent test uses a synchronized native-body substitute, so removing the
guard exposes duplicate admission without racing the original plain native flag.

Run `ove test nuttx-startup` for host tests and a dedicated ARM QEMU image that
creates one ordinary task from a constructor. Its duplicate callback returns
before reusing any native storage or stack. The fixture lives outside the normal
Thread suite to preserve that suite's first-TLS-creation test.

The repair covers static initialization of a flat image with
`CONFIG_HAVE_CXXINITIALIZE`. A child started by a constructor may run before later
constructors; applications retain their existing synchronization requirements.
This does not test loadable ELF constructor handling, protected/kernel images,
native SMP lifecycle behavior, physical hardware or keyword recognition accuracy.

The same target also cross-links small ARM artifacts for the AN500 and F746
board scripts with Make and Ninja. It checks a valid idle-stack size, rejects
an invalid size, changes back to a different valid size, and verifies an
unchanged build. Zero, negative, nonmultiple and oversized idle values must fail
normally; F746 stackcheck reserve sizes are also checked without execution.
The fixture uses the selected native preprocessing invocation
and its generated configuration dependency. These ARM artifacts are inspected
only; none is executed, and their tiny BSS does not establish runtime capacity.

Native ARM test builds use the shared global patches and check their real ELF
before returning to QEMU, Renode or hardware callers. The artifact check covers
the BSS end, vector stack word, idle-stack constant, representable idle interval
and writable primary SRAM sections and loads. Admission separately requires the
known reset transfer: a defined Thumb `start` clears LR and branches directly to
defined `__start` without a compiler frame. Geometry-only assembly fixtures use
the geometry inspection operation and never establish firmware eligibility.
The supported flat, non-PIC,
single-core board providers define the shared heap boundary. SIM uses host
startup and keeps its existing source path. The AN500 native test linker retains
its 16 MiB primary region and existing 2 MiB configured aperture; evidence records
both separately. This does not change stack sizes or qualify other image modes.

The reset regression compiles the actual native vector translation unit with
real generated headers and compiler flags. It covers `-O0`/`-Os`, retained and
omitted frame pointers, forward/backward transfers and entry instrumentation on
both supported boards. Tiny assembly definitions supply only link dependencies;
no generated image is executed. Separate assembler controls verify narrow/wide
branch encodings and their signed displacements. The startup target builds one
F746 test image for these checks without starting Renode.

The CLI runs each reset matrix in a dedicated process group. Compiler, linker
and disassembler children inherit that group, so cancelling the matrix also
stops its tools. The standalone matrix script requires that caller-owned group
and records each tool's group identity; its per-tool deadline remains 30 seconds.
