# Native condition-variable boundary tests

Run with `make test-sync-condvar` and `make test-sync-condvar-sanitize`.
The host models compile the production FreeRTOS and Zephyr adapters and the
C++ binding against initialized SDK models, in both allocation modes. Each
model case is a CTest case and fails through its exit status. The
compile-only `errno_domain` objects check optional quota errno handling.
