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
