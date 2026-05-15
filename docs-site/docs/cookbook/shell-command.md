# Add a custom shell command

**Pattern** — `ove_shell` provides a serial-console CLI with line editing and command dispatch. Register your own commands to drive the app, inspect state, or flip runtime flags without recompiling.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_SHELL=y
```

## Hello, command

```c
#include "ove/ove.h"
#include "ove/shell.h"
#include "ove/log.h"
#include <stdio.h>

OVE_LOG_MODULE_REGISTER(myshell);

static int cmd_hello(int argc, char **argv, ove_shell_t sh)
{
    (void)argc; (void)argv;
    ove_shell_printf(sh, "Hello, world!\n");
    return 0;
}

OVE_SHELL_CMD_REGISTER(hello, cmd_hello, "Print a greeting");

void ove_main(void)
{
    ove_shell_start();   /* spawns the shell on the default console */
    ove_run();
}
```

`OVE_SHELL_CMD_REGISTER` emits a static descriptor + a constructor that links the command into the shell's table before `ove_main` runs. No central registry to keep in sync.

Then over the console:

```
ove> hello
Hello, world!
ove>
```

## Commands with arguments

```c
static int cmd_set_gain(int argc, char **argv, ove_shell_t sh)
{
    if (argc != 2) {
        ove_shell_printf(sh, "usage: set_gain <value>\n");
        return -1;
    }
    char *end;
    float g = strtof(argv[1], &end);
    if (*end != '\0') {
        ove_shell_printf(sh, "not a number: %s\n", argv[1]);
        return -1;
    }
    set_gain(g);
    ove_shell_printf(sh, "gain = %.2f\n", (double)g);
    return 0;
}

OVE_SHELL_CMD_REGISTER(set_gain, cmd_set_gain,
                       "Set DSP gain (linear, e.g. 1.5)");
```

`argv[0]` is the command name; `argv[1..]` are the typed tokens. Quoting follows POSIX-ish rules — `set_gain "1.5"` works.

## Subcommands

For grouping related commands, use one top-level command with a manual dispatch:

```c
static int cmd_net(int argc, char **argv, ove_shell_t sh)
{
    if (argc < 2) {
        ove_shell_printf(sh, "usage: net status|reconnect|stats\n");
        return -1;
    }
    if (strcmp(argv[1], "status") == 0) {
        ove_shell_printf(sh, "link: %s  ip: %s\n",
                         ove_net_is_up() ? "up" : "down",
                         ove_net_ip_str());
        return 0;
    }
    if (strcmp(argv[1], "reconnect") == 0) {
        return ove_net_reconnect();
    }
    if (strcmp(argv[1], "stats") == 0) {
        ove_shell_printf(sh, "rx_bytes=%llu tx_bytes=%llu\n",
                         (unsigned long long)ove_net_stats_rx_bytes(),
                         (unsigned long long)ove_net_stats_tx_bytes());
        return 0;
    }
    ove_shell_printf(sh, "unknown subcommand: %s\n", argv[1]);
    return -1;
}

OVE_SHELL_CMD_REGISTER(net, cmd_net,
                       "Network status (status|reconnect|stats)");
```

## Logging from a shell command

`OVE_LOG_INF` goes to the log sink — usually the same console, so it interleaves with the shell prompt. For shell-only output, use `ove_shell_printf(sh, ...)`. For "this should appear both as a log line and in the shell", call both.

## Built-in commands

`ove_shell` ships with a handful by default:

| Command | What it does |
|---|---|
| `help` | List commands |
| `version` | Print oveRTOS + backend versions |
| `tasks` | List threads with priority, stack high-water mark |
| `mem` | Heap statistics (heap mode only) |
| `reboot` | Trigger a soft reset |
| `log <level>` | Change runtime log level |

Disable any of these by setting `CONFIG_OVE_SHELL_BUILTIN_<NAME>=n`.

## Where else in the tree

- [API: Shell & Console](../api/shell.md) — registration macros, return codes, completion hooks.
- [`apps/c/heap/example_pm/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_pm) — registers `pm_status` and `pm_sleep` shell commands.
