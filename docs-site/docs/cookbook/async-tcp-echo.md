# Async TCP heartbeat (embassy-net)

**Pattern** — run an `embassy_net::Stack` on top of a board-specific Ethernet
Driver, spawn a few concurrent async tasks (link watcher, periodic TCP
heartbeat, etc.) and let the executor schedule them on a single thread.

This is the **async** alternative to [`example_net`](../api/net.md) — picks
the same hardware path (Ethernet MAC → PHY → cable) but uses
[embassy-net](https://github.com/embassy-rs/embassy/tree/main/embassy-net)
instead of lwIP, and `async fn` / `.await` instead of one-thread-per-socket.

## What to enable

`app.yaml`:

```yaml
defconfig:
  - CONFIG_OVE_SYNC=y
  - CONFIG_OVE_TIMER=y
  - CONFIG_OVE_TIME=y
  - CONFIG_OVE_ASYNC=y
  - CONFIG_OVE_ASYNC_NET=y
```

`Cargo.toml`:

```toml
ove = { path = "...", features = ["alloc", "async", "async-net", "async-net-qemu-shm", "async-net-stm32f7-eth"] }
embassy-executor = { version = "0.7", default-features = false }
embassy-time     = { version = "0.4", default-features = false, features = ["tick-hz-1_000_000"] }
embassy-net      = { version = "0.6", default-features = false, features = ["proto-ipv4", "tcp", "udp", "dns", "medium-ethernet"] }
embedded-io-async = "0.6"
static_cell      = "2"
```

`CONFIG_OVE_ASYNC_NET` is mutually exclusive with `CONFIG_OVE_NET` (both
want the MAC). The `async-net-*` Cargo features enable the right Driver
per board; pick the correct one in code with `#[cfg(board_<name>)]`.

## The app

```rust
#![cfg_attr(not(feature = "std"), no_std)]

use ove_allocator as _;

use embassy_executor::Spawner;
use embassy_net::tcp::TcpSocket;
use embassy_net::{Config, IpAddress, IpEndpoint, Ipv4Address, Ipv4Cidr, Runner, Stack, StaticConfigV4};
use embassy_time::{Duration, Timer};
use static_cell::StaticCell;

use ove::async_net::StackResources;

// One Driver per board — built-in transports.
#[cfg(board_qemu_mps2)]
use ove::async_net::QemuShmDriver as NetDriver;
#[cfg(board_stm32f746g_disco)]
use ove::async_net::Stm32f7EthDriver as NetDriver;

const MAC_ADDR: [u8; 6] = [0x02, 0x00, 0x00, 0xBE, 0xEF, 0x01];

#[embassy_executor::task]
async fn net_task(mut runner: Runner<'static, NetDriver>) -> ! {
    runner.run().await
}

#[embassy_executor::task]
async fn app_task(stack: Stack<'static>) {
    while !stack.is_link_up() {
        Timer::after(Duration::from_millis(200)).await;
    }

    let mut rx_buf = [0u8; 1024];
    let mut tx_buf = [0u8; 1024];
    let mut counter: u32 = 0;

    loop {
        let mut socket = TcpSocket::new(stack, &mut rx_buf, &mut tx_buf);
        socket.set_timeout(Some(Duration::from_secs(5)));
        let endpoint = IpEndpoint::new(IpAddress::v4(172, 1, 1, 1), 9999);
        match socket.connect(endpoint).await {
            Ok(()) => {
                let msg = b"oveRTOS async-net tick\n";
                let _ = embedded_io_async::Write::write_all(&mut socket, msg).await;
                socket.close();
            }
            Err(e) => log::warn!("connect failed: {e:?}"),
        }
        counter = counter.wrapping_add(1);
        Timer::after(Duration::from_secs(1)).await;
    }
}

#[ove::main]
async fn app_main(spawner: Spawner) {
    let _ = ove::log::try_init();

    let driver = NetDriver::new(MAC_ADDR);
    let config = Config::ipv4_static(StaticConfigV4 {
        address: Ipv4Cidr::new(Ipv4Address::new(172, 1, 1, 2), 24),
        gateway: Some(Ipv4Address::new(172, 1, 1, 1)),
        dns_servers: Default::default(),
    });
    let seed: u64 = 0x0123_4567_89ab_cdef;

    // StaticCell keeps the 3 KB StackResources out of the FreeRTOS
    // heap — important on tight-RAM targets like STM32F7.
    static RESOURCES: StaticCell<StackResources<3>> = StaticCell::new();
    let resources = RESOURCES.init(StackResources::new());
    let (stack, runner) = embassy_net::new(driver, config, resources, seed);

    spawner.must_spawn(net_task(runner));
    spawner.must_spawn(app_task(stack));
}
```

## Build + run

### STM32F746G-Discovery (real hardware)

```bash
ove defconfig-fragments stm32f746g-discovery.freertos.example_async_net_rust
ove configure && ove build
ove flash
```

Plug an Ethernet cable into the disco board's RJ45 jack and connect the
other end to a switch / router / Pi serving `172.1.1.1`. On the host
that owns `172.1.1.1`:

```bash
nc -lk 9999
```

Then watch the heartbeats arrive once per second:

```
oveRTOS async-net tick
oveRTOS async-net tick
```

### QEMU MPS2-AN500 (no hardware needed)

```bash
ove defconfig-fragments qemu.freertos.example_async_net_rust
ove configure && ove build
sudo config/scripts/qemu-net-setup.sh   # one-time TAP + NAT setup
nc -lk 9999 &
ove run
```

The QEMU SHM transport bridges Ethernet frames through `/dev/shm/ove-net`
to a TAP interface on the host. See
[`apps/rust/heap/example_async_net/src/lib.rs`](https://github.com/k1lulko/oveRTOS/tree/main/apps/rust/heap/example_async_net)
for the in-tree app this recipe is distilled from.

## What's happening under the hood

1. `NetDriver::new()` initialises the MAC + PHY (STM32F7 ETH + LAN8742A,
   or the QEMU SHM transport's announce-header).
2. `embassy_net::new()` constructs the smoltcp-backed `Stack` and a
   `Runner` future that drives it.
3. `net_task` runs the `Runner` forever — receives frames, dispatches
   TCP/UDP/DNS, transmits queued frames.
4. On STM32F7 the ETH IRQ wakes the `Runner` via a notify callback
   that fires `AtomicWaker::wake()` — no polling needed for RX. On
   QEMU SHM there's no interrupt, so a small `poll_task` ticks every
   ~10 ms to keep the `Runner` polling.
5. `app_task` uses the standard `embassy_net::TcpSocket` API. It blocks
   only on `.await` — while it's waiting for a connect/send/recv, the
   `Runner` can drive other tasks (or other sockets) on the same
   thread.

## Picking the right async protocol crate

Layer something from crates.io on top of the raw TCP socket above:

| Protocol | Crate |
|---|---|
| TLS  | [`embedded-tls`](https://crates.io/crates/embedded-tls) |
| HTTP | [`reqwless`](https://crates.io/crates/reqwless) |
| MQTT | [`rust-mqtt`](https://crates.io/crates/rust-mqtt) |
| SNTP | [`sntpc`](https://crates.io/crates/sntpc) (with `embassy-socket` feature) |
| HTTPD | [`picoserve`](https://crates.io/crates/picoserve) |

See [`ove::async_net`'s module docs](../rust/ove/async_net/)
for code snippets pairing each of these with the embassy-net Stack.

## Cookbook map

- For the **blocking** version of this pattern — sockets in a dedicated
  thread, in-tree mbedTLS + HTTP + MQTT — see [`example_net`](../api/net.md).
- For HTTPS upload specifically, see [POST telemetry to HTTPS](https-post.md).
