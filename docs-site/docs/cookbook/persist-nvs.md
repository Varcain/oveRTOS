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

The storage calls block (an SD card read, a file write), so they run in a thread. On FreeRTOS `ove_main()` runs before the scheduler starts, and a blocking call there crashes; NuttX and Zephyr happen to tolerate it.

NVS storage backing differs by RTOS and board:

- **FreeRTOS, NuttX and Zephyr**: one file per key in `/nvs` on the volume the application mounts with `ove_fs_mount()` (`CONFIG_OVE_NVS` turns on `CONFIG_OVE_FS`). Mount before the first read or write; on NuttX's default SD mount the directory is `/mnt/sd/nvs`. The layout is the same on every engine, so a key written under one engine reads back under another from the same card.
- **QEMU MPS2-AN500 and AN521** (no storage device): a RAM table that lasts until QEMU exits.

The API is identical across all of them.

## Code — boot counter

```c
#include "ove/ove.h"
#include "ove/nvs.h"
#include "ove/log.h"

#define KEY_BOOT_COUNT  "boot_count"

static void boot_counter(void *arg)
{
    (void)arg;
    ove_fs_mount(NULL, NULL);   /* the SD card; RAM-only QEMU boards ignore it */
    ove_nvs_init();

    uint32_t boots = 0;
    size_t len = 0;

    int rc = ove_nvs_read(KEY_BOOT_COUNT, &boots, sizeof(boots), &len);
    if (rc == OVE_OK && len == sizeof(boots)) {
        OVE_LOG_INF("boot #%u (resumed)", (unsigned)boots);
    } else {
        OVE_LOG_INF("first boot");
        boots = 0;
    }

    boots++;
    if (ove_nvs_write(KEY_BOOT_COUNT, &boots, sizeof(boots)) != OVE_OK) {
        OVE_LOG_ERR("failed to persist boot count");
    }
}

void ove_main(void)
{
    static ove_thread_t counter;
    ove_thread_create(&counter, "boot_counter", boot_counter, NULL,
                      OVE_PRIO_NORMAL, 4096);
    ove_run();
}
```

Run it three times — log lines walk `first boot`, `boot #1 (resumed)`, `boot #2 (resumed)`.

## Larger values: a calibration struct

Call `load_calib()` from a thread too, after `ove_nvs_init()`:

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
    size_t len = 0;
    int rc = ove_nvs_read(KEY_CALIB, out, sizeof(*out), &len);

    /* Version mismatch or missing — fall back to defaults and rewrite. */
    if (rc != OVE_OK || len != sizeof(*out) || out->version != CALIB_VERSION) {
        OVE_LOG_WRN("calibration missing or stale, using defaults");
        *out = defaults;
        ove_nvs_write(KEY_CALIB, out, sizeof(*out));
    }
}
```

The `version` field is the migration handle: when you change the struct layout, bump the constant and the old entry triggers the fallback rewrite path. `len` is the stored size even when it exceeds the buffer, so a value written by an older, larger layout is caught too; `ove_nvs_read(key, NULL, 0, &len)` asks for the size alone.

## What NOT to store in NVS

- **Frequently-changing data** — every `ove_nvs_write` rewrites an SD-card file, and cards wear out. Use NVS for occasional writes, not 100 Hz log lines.
- **Secrets** — flash and SD cards are readable with a debug probe or a card reader. Encrypt before storing if confidentiality matters.

## Where else in the tree

- [API: Non-Volatile Storage](../api/nvs.md) — full API surface
