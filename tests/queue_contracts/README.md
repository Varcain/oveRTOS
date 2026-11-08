# Native queue contracts

Run `make test-queue-contracts` and `make test-queue-contracts-sanitize`.
CTest runs the provider, context and POSIX integration observers in both heap
modes; each fails through its exit status. The provider observers compile the
actual queue providers against inert native functions; the NuttX observer
models a semaphore count limit of seven. `context_models.py` builds Queue-only
context models whose architectural register reads are substituted inputs.
