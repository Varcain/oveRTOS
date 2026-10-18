# POST telemetry to HTTPS

**Pattern** — periodically push a JSON document to an HTTPS endpoint, using oveRTOS's `ove_net_http` client over `ove_net_tls` (mbedTLS). Works against any TLS server that does not ask for a client certificate.

**What to enable** (in your `app.yaml`'s `defconfig:` list):

```yaml
defconfig:
  - CONFIG_OVE_CONSOLE=y
  - CONFIG_OVE_LOG=y
  - CONFIG_OVE_NET=y
  - CONFIG_OVE_NET_TLS=y
  - CONFIG_OVE_NET_HTTP=y
  - CONFIG_OVE_NET_SNTP=y    # UTC timestamps for the payload
```

## Why SNTP?

A freshly booted board only knows the time since boot. `ove_sntp_sync()` asks an NTP server once and keeps the offset; `ove_sntp_get_utc()` then returns UTC seconds, which is what a telemetry backend wants in each record.

TLS needs it too: certificate dates are checked against that clock, so until SNTP has synced once every `https://` request fails verification.

## Code

All the network work runs in a worker thread: on FreeRTOS `ove_main()` runs before the scheduler, where nothing may block.

```c
#include "ove/ove.h"
#include <stdio.h>

#define POST_URL        "https://telemetry.example.com/ingest"
#define POST_PERIOD_MS  10000

/* Your server's CA certificate, PEM with its terminating NUL (or DER). */
extern const unsigned char server_ca[];
extern const size_t        server_ca_len;

int32_t read_sensor(void);   /* your driver */

static ove_netif_t               netif;
static ove_netif_storage_t       netif_storage;
static ove_http_client_t         http;
static ove_http_client_storage_t http_storage;
static ove_thread_t              worker;

static int net_up(void)
{
    ove_netif_config_t cfg = {0};   /* static address, as in apps/c/heap/example_net */
    ove_sockaddr_ipv4(&cfg.static_ip, 172, 1, 1, 2, 0);
    ove_sockaddr_ipv4(&cfg.gateway, 172, 1, 1, 1, 0);
    ove_sockaddr_ipv4(&cfg.netmask, 255, 255, 255, 0, 0);
    ove_sockaddr_ipv4(&cfg.dns, 8, 8, 8, 8, 0);

    int rc = ove_netif_init(&netif, &netif_storage);
    if (rc == OVE_OK)
        rc = ove_netif_up(netif, &cfg);
    return rc;
}

static void post_one(void)
{
    uint32_t utc = 0;
    ove_sntp_get_utc(&utc);

    char body[96];
    int len = snprintf(body, sizeof(body), "{\"ts\":%lu,\"value\":%ld}",
                       (unsigned long)utc, (long)read_sensor());

    ove_http_response_t resp;
    int rc = ove_http_post(http, POST_URL, "application/json", body, (size_t)len, &resp);
    if (rc != OVE_OK) {
        OVE_LOG_ERR("POST failed: %d", rc);
        return;
    }
    if (resp.status != 200)
        OVE_LOG_WRN("POST returned %d", resp.status);
    ove_http_response_free(&resp);
}

static void worker_fn(void *arg)
{
    (void)arg;
    if (net_up() != OVE_OK) {
        OVE_LOG_ERR("network did not come up");
        return;
    }
    ove_thread_sleep_ms(3000);   /* let the link come up on hardware */

    if (ove_sntp_sync(NULL) != OVE_OK)   /* pool.ntp.org, 5 s timeout */
        OVE_LOG_WRN("SNTP failed; timestamps count from boot");

    ove_http_set_tls(server_ca, server_ca_len, 0);
    if (ove_http_client_init(&http, &http_storage) != OVE_OK)
        return;

    while (1) {
        post_one();
        ove_thread_sleep_ms(POST_PERIOD_MS);
    }
}

void ove_main(void)
{
    /* TLS handshakes are stack-hungry: give the worker 16 KB. */
    ove_thread_create(&worker, "telemetry", worker_fn, NULL,
                      OVE_PRIO_NORMAL, 16 * 1024);
    ove_run();
}
```

## Trust store

There is no built-in CA bundle. Until `ove_http_set_tls()` installs a trust anchor, every `https://` request fails closed, because the server cannot be verified. Pass your server's CA (or the CA that signed it) as PEM including the terminating NUL, or as DER. The third argument, `allow_insecure`, skips verification for bring-up on a test bench only; never ship with it set.

## Memory and stack

The handshake runs on the calling thread, which is why the worker gets 16 KB above. mbedTLS takes its working memory from the heap, or from a `CONFIG_OVE_NET_TLS_HEAP_SIZE` static pool in zero-heap builds.

## Where else in the tree

- [API: Networking](../api/net.md) — sockets, DNS, TLS, HTTP, MQTT, HTTPD, SNTP.
- [`apps/c/heap/example_net/`](https://github.com/Varcain/oveRTOS/tree/main/apps/c/heap/example_net) — sockets, DNS, HTTP and SNTP; brings the interface up the same way.
