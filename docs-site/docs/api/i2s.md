# I2S — Audio Bus Driver

Header: `ove/i2s.h` | Kconfig: `CONFIG_OVE_I2S`

## Overview

I2S / SAI master driver for DMA-based audio streaming. Uses double-buffered
(ping-pong) DMA with half-buffer completion callbacks delivered from ISR
context, enabling continuous low-latency capture and playback.

The codec attached to the bus is **not** initialised by this driver — codec
bring-up is board-specific and is normally done from the board BSP (e.g. via
I2C register writes).

## Configuration

```c
struct ove_i2s_cfg {
    unsigned int  instance;         // Peripheral index (0, 1, ...)
    uint32_t      sample_rate;      // Hz (e.g. 44100, 48000)
    uint8_t       bit_depth;        // 16, 24, or 32
    uint8_t       channels;         // 1 (mono) or 2 (stereo)
    ove_i2s_dir_t direction;        // TX, RX, or TXRX
    size_t        dma_buf_samples;  // Total samples in DMA buffer (both halves)
};
```

`ove_i2s_dir_t` values: `OVE_I2S_DIR_TX`, `OVE_I2S_DIR_RX`, `OVE_I2S_DIR_TXRX`.

## API

| Function | Description |
|---|---|
| `ove_i2s_init()` | Initialise with static storage and caller-supplied DMA buffers |
| `ove_i2s_create()` / `ove_i2s_destroy()` | Heap or zero-heap per-call-site storage |
| `ove_i2s_set_rx_callback()` | Register RX half-buffer-complete ISR callback |
| `ove_i2s_set_tx_callback()` | Register TX half-buffer-complete ISR callback |
| `ove_i2s_start()` / `ove_i2s_stop()` | Start / stop circular DMA streaming |
| `ove_i2s_pause()` / `ove_i2s_resume()` | Pause and resume a running stream |
| `ove_i2s_rx_buf()` | Pointer to the completed RX half-buffer (call from RX callback) |
| `ove_i2s_tx_buf()` | Pointer to the TX half-buffer safe to refill |
| `ove_i2s_half_buf_size()` | Size of one half-buffer in bytes |

Callback signature:

```c
typedef void (*ove_i2s_cb_t)(ove_i2s_t i2s, void *user_data);
```

Callbacks run in ISR context. Keep them short — typically they just signal
a worker thread to process the buffer.

## Example: Capturing Microphone Audio

```c
static void rx_ready(ove_i2s_t i2s, void *user_data)
{
    ove_sem_t *sem = user_data;
    ove_sem_give_from_isr(*sem);
}

struct ove_i2s_cfg cfg = {
    .instance        = 2,
    .sample_rate     = 16000,
    .bit_depth       = 16,
    .channels        = 1,
    .direction       = OVE_I2S_DIR_RX,
    .dma_buf_samples = 1024,        /* two halves of 512 samples */
};

ove_i2s_t i2s;
ove_i2s_create(&i2s, &cfg);
ove_i2s_set_rx_callback(i2s, rx_ready, &buf_ready_sem);
ove_i2s_start(i2s);

for (;;) {
    ove_sem_take(buf_ready_sem, OVE_WAIT_FOREVER);
    const int16_t *samples = ove_i2s_rx_buf(i2s);
    size_t bytes           = ove_i2s_half_buf_size(i2s);
    /* process samples ... */
}
```

## Backend Notes

| Backend | Implementation |
|---|---|
| FreeRTOS/STM32 | HAL SAI / I2S with `HAL_SAI_Transmit/Receive_DMA` and half/full-complete ISR hooks |
| Zephyr | `i2s_configure()` + `i2s_write()` / `i2s_read()` with ping-pong buffers |
| NuttX | Audio subsystem `/dev/audio*` with DMA ring buffers |
| POSIX | Routed through the sim framework to the browser dashboard (Web Audio API) |
| WASM | Browser Web Audio API output; input capture depends on `getUserMedia` permission |

## Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_OVE_I2S` | `n` | Enable the I2S / SAI driver |

`ove_i2s` is the low-level transport used by the higher-level
[audio graph engine](audio.md) when the graph is connected to I2S device
nodes.
