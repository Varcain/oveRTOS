# Add a custom shell command

**Pattern** — `ove_shell` is a small serial-console command line: it splits each line into words and calls the handler registered under the first word. Register your own commands to drive the app, inspect state, or flip runtime flags without recompiling.

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

static void cmd_hello(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    OVE_LOG_INF("Hello, world!");
}

static const struct ove_shell_cmd hello_cmd = {
    .name    = "hello",
    .help    = "Print a greeting",
    .handler = cmd_hello,
};

/* The shell only sees what you feed it: read the console, pass each
 * character on. A line is dispatched when Enter arrives. */
static void shell_fn(void *arg)
{
    (void)arg;
    while (1) {
        int c = ove_console_getchar();
        if (c >= 0)
            ove_shell_process_char(c);
        else
            ove_thread_sleep_ms(10);
    }
}

void ove_main(void)
{
    static ove_thread_t shell;
    /* ove_shell_init() also registers the built-in "help" */
    if (ove_shell_init() != OVE_OK || ove_shell_register_cmd(&hello_cmd) != OVE_OK)
        return;
    if (ove_thread_create(&shell, "shell", shell_fn, NULL, OVE_PRIO_LOW, 4096) != OVE_OK)
        return;
    ove_run();
}
```

The descriptor must outlive the registration, hence `static`. The table holds 16 commands, `help` included; registering a name twice replaces the earlier handler.

Then over the console:

```
> hello
[I] Hello, world!
>
```

Handlers print with `OVE_LOG_*` (or raw text with `ove_console_write()`). While a handler runs, the log lines it emits from its own thread also reach the hook set with `ove_shell_set_output_hook()`, or the one passed to `ove_shell_process_line_with_hook()` for that command alone, which is how the HTTP dashboard's terminal shows command output.

## Commands with arguments

```c
#include <stdlib.h>

void set_gain(float g);   /* your code */

static void cmd_set_gain(int argc, const char *argv[])
{
    if (argc != 2) {
        OVE_LOG_WRN("usage: set_gain <value>");
        return;
    }
    char *end;
    float g = strtof(argv[1], &end);
    if (*end != '\0') {
        OVE_LOG_WRN("not a number: %s", argv[1]);
        return;
    }
    set_gain(g);
    OVE_LOG_INF("gain = %d.%02d", (int)g, (int)(g * 100) % 100);
}

static const struct ove_shell_cmd set_gain_cmd = {
    .name    = "set_gain",
    .help    = "Set DSP gain (linear, e.g. 1.5)",
    .handler = cmd_set_gain,
};
```

`argv[0]` is the command name; `argv[1..]` are the words that followed, split at spaces and tabs. There is no quoting, a line holds at most 127 characters, and words past the eighth (`OVE_SHELL_MAX_ARGS`) are dropped. Register `set_gain_cmd` with `ove_shell_register_cmd()` as above.

## Subcommands

For grouping related commands, use one top-level command with a manual dispatch:

```c
#include <string.h>

int  sensor_last_reading(void);   /* your code */
void sensor_calibrate(void);

static void cmd_sensor(int argc, const char *argv[])
{
    if (argc < 2) {
        OVE_LOG_WRN("usage: sensor read|calibrate");
        return;
    }
    if (strcmp(argv[1], "read") == 0) {
        OVE_LOG_INF("last reading: %d", sensor_last_reading());
    } else if (strcmp(argv[1], "calibrate") == 0) {
        sensor_calibrate();
        OVE_LOG_INF("calibrated");
    } else {
        OVE_LOG_WRN("unknown subcommand: %s", argv[1]);
    }
}

static const struct ove_shell_cmd sensor_cmd = {
    .name    = "sensor",
    .help    = "Sensor tools (read|calibrate)",
    .handler = cmd_sensor,
};
```

## Built-in commands

Only `help`, which lists every registered command with its help text. Anything else (versions, thread lists, reboot) is yours to add the same way.

## Where else in the tree

- [API: Shell & Console](../api/shell.md) — the full shell and console API.
- [`tests/suites/test_shell.c`](https://github.com/Varcain/oveRTOS/tree/main/tests/suites/test_shell.c) — registration, dispatch and argument splitting exercised by the test suite.
