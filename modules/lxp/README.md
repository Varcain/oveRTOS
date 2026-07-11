# lxp — the OS-agnostic Linux personality

`lxp` is a self-contained, NOMMU-friendly **Linux/uClinux personality**: an FDPIC
process model (`vfork`/`exec`/`wait`, signals, pipes, pty), a Linux syscall core,
an FDPIC ELF loader, an optional socket bridge, a framebuffer/input device layer,
and an optional 9P remote filesystem — with **zero dependency on any particular
OS**. It was extracted from [oveRTOS](https://github.com/) and made host-agnostic.

Any RTOS or bare-metal host runs it by implementing a small **port interface**
(function-pointer structs, the lwIP `sys_arch` / LVGL / FatFs `diskio` pattern)
and calling `lxp_run()`. No `lxp` source `#include`s a host header.

## The port interface (`include/lxp/lxp_port.h`)

The host fills three ops-structs and passes them to `lxp_run()`:

| Struct | What the host provides |
|---|---|
| `lxp_os_ops_t` | program-memory placement (MPU regions / dyn pool), task spawn/abort, critical section, run-loop event wait/post, monotonic time, optional thread introspection + cache maintenance + remote-exec staging. |
| `lxp_net_ops_t` | handle-based sockets — the host **owns socket storage** and returns opaque handles (17 socket + 5 netif calls). `NULL` if built without `LXP_ENABLE_NET`. |
| `lxp_display_ops_t` | framebuffer get/flush/present + optional touch read. `NULL` if built without `LXP_ENABLE_DEV`. |

```c
#include "lxp/lxp.h"

int rc = lxp_run(&my_os_ops, &my_net_ops, &my_display_ops,
                 &my_config, &my_run_config, "/sbin/init", argc, argv);
```

## Configuration (`include/lxp/lxp_config.h`)

Feature gates (`LXP_ENABLE_NET`, `LXP_ENABLE_NETFS`, `LXP_ENABLE_PTY`,
`LXP_ENABLE_DEV*`, …) and sizing/placement knobs (`LXP_PROG_REGION_SIZE`,
`LXP_NREG`, `LXP_NPIPE`, `LXP_PIPE_BUF`, `LXP_FAR_BSS`, …) are set either on the
compiler command line, via a drop-in `lxp_config_user.h` on the include path, or
left at their safe defaults.

## Building

* **Standalone** (against the bundled POSIX reference port):
  `cmake -S modules/lxp -B build -DLXP_PORT_POSIX=ON && cmake --build build`
* **Embedded**: `add_subdirectory(modules/lxp)` and
  `target_link_libraries(app PRIVATE lxp::lxp)`, then set the `LXP_ENABLE_*`
  and sizing defines from your own config and supply the ops adapters.

## Reference ports

* `ports/posix/lxp_port_posix.c` — POSIX sockets + monotonic clock; the
  standalone build + host test target (proves the decoupling).
* The oveRTOS consumer wires the ops to its own HALs in a thin adapter and its
  per-engine (FreeRTOS / NuttX / Zephyr) seams.

## License

GPL-3.0-or-later.
