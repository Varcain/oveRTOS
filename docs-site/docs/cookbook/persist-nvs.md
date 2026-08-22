# Persist a value across reboots

**Pattern** — keep a small piece of state (counter, last-used setting, calibration value) across power cycles using `ove_nvs`, a key-value store backed by board-specific non-volatile storage.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_LOG_LEVEL_INF=y
  - CONFIG_OVE_NVS=y
```

NVS storage backing differs by RTOS and board:

- **FreeRTOS / STM32**: internal flash sector
- **NuttX**: a `littlefs` mount
- **Zephyr**: the Zephyr `nvs` subsystem
- **POSIX**: a file under `output/<board>/<rtos>/<app>/nvs.bin`

The API is identical across all of them.

## Code — boot counter

```c
#include "ove/ove.h"
#include "ove/nvs.h"
#include "ove/log.h"

#define KEY_BOOT_COUNT  "boot_count"

void ove_main(void)
{
    ove_nvs_init();

    uint32_t boots = 0;
    size_t len = sizeof(boots);

    int rc = ove_nvs_get(KEY_BOOT_COUNT, &boots, &len);
    if (rc == OVE_OK && len == sizeof(boots)) {
        OVE_LOG_INF("boot #%u (resumed)", boots);
    } else {
        OVE_LOG_INF("first boot");
        boots = 0;
    }

    boots++;
    if (ove_nvs_set(KEY_BOOT_COUNT, &boots, sizeof(boots)) != OVE_OK) {
        OVE_LOG_ERR("failed to persist boot count");
    }

    ove_run();
}
```

Run it three times — log lines walk `first boot`, `boot #1 (resumed)`, `boot #2 (resumed)`.

## Larger values: a calibration struct

```c
typedef struct {
    int16_t offset_mv;
    float   gain;
    uint32_t version;   /* bump when layout changes */
} calib_t;

#define KEY_CALIB     "calib"
#define CALIB_VERSION 2

static calib_t defaults = { .offset_mv = 0, .gain = 1.0f,
                            .version = CALIB_VERSION };

static void load_calib(calib_t *out)
{
    size_t len = sizeof(*out);
    int rc = ove_nvs_get(KEY_CALIB, out, &len);

    /* Version mismatch or missing — fall back to defaults and rewrite. */
    if (rc != OVE_OK || len != sizeof(*out) || out->version != CALIB_VERSION) {
        OVE_LOG_WRN("calibration missing or stale, using defaults");
        *out = defaults;
        ove_nvs_set(KEY_CALIB, out, sizeof(*out));
    }
}
```

The `version` field is the migration handle: when you change the struct layout, bump the constant and the old entry triggers the fallback rewrite path.

## What NOT to store in NVS

- **Frequently-changing data** — `ove_nvs_set` writes to flash, which has a wear budget (typically 10⁵ erase cycles). Use NVS for occasional writes, not 100 Hz log lines.
- **Large blobs** — backing storage is small (typically tens of KB). For arbitrary files, mount `ove_fs` instead.
- **Secrets** — internal flash is readable with a debug probe. Encrypt before storing if confidentiality matters.

## Listing keys

For debugging:

```c
ove_nvs_iter_t it;
ove_nvs_iter_open(&it);
const char *key;
while ((key = ove_nvs_iter_next(&it)) != NULL) {
    OVE_LOG_INF("key: %s", key);
}
ove_nvs_iter_close(&it);
```

## Where else in the tree

- [API: Non-Volatile Storage](../api/nvs.md) — full API surface
- [`apps/c/heap/example_pm/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_pm) — power-management example uses NVS to remember battery state.
