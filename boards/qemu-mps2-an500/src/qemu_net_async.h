/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU MPS2-AN500 shared-memory Ethernet — transport primitives shared
 * between the lwIP backend (qemu_net.c) and the embassy-net driver
 * (bindings/rust/ove/src/async_net/qemu_shm.rs).
 *
 * The two IP stacks are mutually exclusive at compile time via
 * CONFIG_OVE_NET (lwIP) vs CONFIG_OVE_ASYNC_NET (embassy-net) — they
 * share the same /dev/shm/ove-net file but only one stack owns the
 * ring positions at runtime.
 */

#ifndef OVE_QEMU_NET_ASYNC_H
#define OVE_QEMU_NET_ASYNC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the SHM ring file and announce ourselves to the bridge.
 *
 * Writes the 64-byte header (magic, MAC, MTU, ring size) so the host
 * bridge can discover us, then polls @c link_up for up to 5 seconds.
 * Subsequent calls reset ring positions to zero.
 *
 * @param[in] mac  6-byte MAC address to announce.
 * @return OVE_OK on success, negative error code if semihosting open
 *         fails (no bridge running / headless).
 */
int ove_qemu_net_async_init(const uint8_t mac[6]);

/**
 * @brief Send an Ethernet frame onto the TX ring.
 *
 * Returns @c OVE_ERR_NO_MEMORY if the ring is full (host bridge
 * hasn't drained yet).
 *
 * @param[in] frame  Frame data.
 * @param[in] len    Bytes (must be ≤ 1518).
 * @return OVE_OK / OVE_ERR_NO_MEMORY / OVE_ERR_BUS_ERROR.
 */
int ove_qemu_net_async_tx(const void *frame, uint32_t len);

/**
 * @brief Try to dequeue one frame from the RX ring (non-blocking).
 *
 * @param[out] buf       Output buffer.
 * @param[in]  buf_size  Capacity (should be ≥ 1518).
 * @param[out] out_len   Frame length written on success.
 * @return OVE_OK if a frame was produced, @c OVE_ERR_NOT_FOUND if
 *         empty, negative on error.
 */
int ove_qemu_net_async_rx(void *buf, uint32_t buf_size, uint32_t *out_len);

/**
 * @brief Read the bridge-set link_up flag.
 *
 * @return non-zero if the host bridge has signalled link-up.
 */
int ove_qemu_net_async_link_up(void);

#ifdef __cplusplus
}
#endif

#endif /* OVE_QEMU_NET_ASYNC_H */
