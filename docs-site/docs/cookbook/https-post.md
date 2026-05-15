# POST telemetry to HTTPS

**Pattern** — periodically push a JSON document to an HTTPS endpoint. The code below works against any TLS-terminating server (no client cert), using oveRTOS's bundled `ove_net_http` over `ove_net_tls`.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_NET=y
  - CONFIG_OVE_NET_TLS=y
  - CONFIG_OVE_NET_HTTP=y
  - CONFIG_OVE_NET_SNTP=y    # for clock; TLS cert validation needs a valid time
```

## Why SNTP?

TLS certificate validation rejects certificates outside their `notBefore`/`notAfter` window. A freshly-booted board has no idea what year it is. `ove_net_sntp` syncs the system clock before the first HTTPS call.

## Code

```c
#include "ove/ove.h"
#include "ove/log.h"
#include "ove/net.h"
#include "ove/net_http.h"
#include "ove/net_sntp.h"
#include <stdio.h>

OVE_LOG_MODULE_REGISTER(telemetry);

#define POST_PERIOD_MS  60000   /* once per minute */

static void post_one(void)
{
    char body[128];
    snprintf(body, sizeof(body),
             "{\"ts\":%llu,\"value\":%d}",
             (unsigned long long)(ove_time_now_us() / 1000),
             read_sensor());

    struct ove_http_request req = {
        .method   = OVE_HTTP_POST,
        .url      = "https://your-server.example.com/ingest",
        .headers  = (const char *[]){
            "Content-Type: application/json",
            "Authorization: Bearer YOUR_TOKEN",
            NULL,
        },
        .body     = (const uint8_t *)body,
        .body_len = strlen(body),
        .timeout_ms = 10000,
    };

    struct ove_http_response resp = {0};
    int rc = ove_http_request(&req, &resp);
    if (rc != OVE_OK) {
        OVE_LOG_WRN("POST failed: %d", rc);
        return;
    }

    OVE_LOG_INF("POST %d (%zu bytes returned)", resp.status, resp.body_len);
    ove_http_response_free(&resp);
}

void ove_main(void)
{
    /* Block until the network interface is up (DHCP / static IP). */
    while (!ove_net_is_up()) {
        ove_thread_sleep_ms(200);
    }

    /* Sync wallclock before our first TLS call. */
    if (ove_sntp_sync(/* server */ "pool.ntp.org",
                      /* timeout_ms */ 5000) != OVE_OK) {
        OVE_LOG_ERR("SNTP failed — TLS cert validation will reject everything");
    }

    while (1) {
        post_one();
        ove_thread_sleep_ms(POST_PERIOD_MS);
    }
}
```

## Trust store

Server cert validation uses a baked-in CA bundle (Mozilla's, regenerated from `ca-certificates`). For self-signed servers, either:

- Provide a custom CA via `ove_tls_set_trust_store(pem_data, pem_len)` before the first HTTPS call, or
- Pin the server certificate's SHA-256: `req.tls_pin_sha256 = pin_bytes;`

Don't disable validation entirely — there is no `tls_verify = false` knob on purpose.

## Memory and stack

HTTPS over the bundled mbedTLS needs a substantial thread stack — bump the calling thread to **at least 16 KB** for handshakes:

```c
ove_thread_create(&worker, "telemetry", worker_fn, NULL,
                  OVE_PRIO_NORMAL, 16 * 1024);
```

If you build with `CONFIG_OVE_NET_TLS_MEM_PROFILE=tiny`, mbedTLS is configured for ECC-only curves and 8 KB stacks; expect handshake speed to drop.

## Where else in the tree

- [API: Networking](../api/net.md) — sockets, DNS, TLS, HTTP, MQTT, HTTPD, SNTP.
- [`apps/c/heap/example_net/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_net) — TCP/UDP + DNS demo with the same `ove_net_is_up()` startup pattern.
