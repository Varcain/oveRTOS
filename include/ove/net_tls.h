/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NET_TLS_H
#define OVE_NET_TLS_H

/**
 * @file net_tls.h
 * @defgroup ove_net_tls TLS
 * @brief Portable TLS layer using mbedTLS.
 *
 * Provides encrypted socket communication.  The TLS layer wraps
 * mbedTLS and delegates I/O to the oveRTOS socket API, making it
 * portable across all backends.
 *
 * Its random generator is seeded from the board's entropy source
 * (@c ove_hal_entropy_fill: the STM32 hardware RNG; on QEMU a test-only
 * source that warns at first use). Certificate dates are checked against
 * UTC from @ref ove_sntp_sync, so peer verification fails until SNTP has
 * synced once.
 *
 * Sessions on different threads may run at the same time: mbedTLS's shared
 * state (the PSA key store, its random generator and, in zero-heap builds,
 * the @c CONFIG_OVE_NET_TLS_HEAP_SIZE pool) is guarded by one oveRTOS mutex,
 * held only inside mbedTLS and never across socket I/O.  It is held for a
 * whole RSA operation, though (verifying an RSA certificate chain, signing
 * with an RSA client key), so those make every other session wait; and on
 * NuttX, built without CONFIG_PRIORITY_INHERITANCE, a high-priority session
 * can wait behind a low-priority one for as long as threads in between
 * run.  One session must not be used from two threads at once.  The guard is set up at startup by
 * this module, so code that calls mbedTLS directly must link ove_net_tls
 * (any ove_tls_* call does).
 *
 * @note Requires @c CONFIG_OVE_NET_TLS (implies @c CONFIG_OVE_NET).
 *       When disabled every function is replaced by a no-op stub
 *       that returns @c OVE_ERR_NOT_SUPPORTED.
 * @{
 */

#include "ove/types.h"
#include "ove/net.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief TLS session configuration.
 *
 * @note If @c ca_cert is NULL the peer certificate is not verified, so the
 *       session is vulnerable to man-in-the-middle. The handshake refuses
 *       this configuration unless @c allow_insecure is explicitly set to
 *       a non-zero value.
 */
typedef struct {
	const unsigned char *ca_cert; /**< PEM or DER CA certificate (NULL to skip verify). */
	size_t ca_cert_len;	      /**< Length of ca_cert in bytes (incl. NUL for PEM). */
	const char *
		hostname; /**< Server name, sent as SNI and checked against the server's certificate; required with @c ca_cert (NULL only when the peer is not verified). */
	const unsigned char *
		client_cert; /**< PEM (may hold a chain, leaf first) or DER (one certificate) client certificate for mTLS, with @c client_key or neither (NULL to skip). Sent in one record, so the chain must fit CONFIG_OVE_NET_TLS_OUT_CONTENT_LEN (4 KB by default). */
	size_t client_cert_len; /**< Length of client_cert in bytes (incl. NUL for PEM). */
	const unsigned char
		*client_key;   /**< PEM or DER unencrypted client private key (NULL to skip). */
	size_t client_key_len; /**< Length of client_key in bytes (incl. NUL for PEM). */
	int allow_insecure; /**< Non-zero to allow NULL @c ca_cert (disables peer verify — do not use in production). */
} ove_tls_config_t;

#include "ove/storage.h"

#ifdef CONFIG_OVE_NET_TLS

/**
 * @brief Initialise a TLS session from caller-supplied storage.
 *
 * @param[out] tls     Handle written on success.
 * @param[in]  storage Caller-allocated storage.
 * @return OVE_OK on success; @c OVE_ERR_NO_MEMORY when the heap cannot
 *         hold the mbedTLS contexts (heap builds); @c OVE_ERR_NOT_SUPPORTED
 *         when the random generator cannot be seeded, from a failing
 *         entropy source or mbedTLS out of memory (it does not say which;
 *         the mbedTLS code is logged), or when the mutex guarding mbedTLS
 *         could not be created.
 */
OVE_NODISCARD int ove_tls_init(ove_tls_t *tls, ove_tls_storage_t *storage);

/**
 * @brief De-initialise a TLS session (frees internal resources, not storage).
 *
 * @param[in] tls Handle returned by ove_tls_init().
 */
void ove_tls_deinit(ove_tls_t tls);

/**
 * @brief Perform the TLS handshake over an established socket.
 *
 * @param[in] tls  TLS handle.
 * @param[in] sock Connected socket to wrap.
 * @param[in] cfg  TLS configuration (certs, hostname).
 * @return OVE_OK on success; @c OVE_ERR_INVALID_PARAM when only one of
 *         @c client_cert and @c client_key is set, when the key does not
 *         match the certificate, without a CA and @c allow_insecure, or
 *         with a CA but no @c hostname;
 *         @c OVE_ERR_NO_MEMORY when mbedTLS runs out of memory (the heap,
 *         or the CONFIG_OVE_NET_TLS_HEAP_SIZE pool in zero-heap builds);
 *         otherwise a negative error code (other mbedTLS failures are
 *         @c OVE_ERR_NOT_SUPPORTED, with the mbedTLS code logged).  Some
 *         shortfalls mbedTLS itself reports as other failures, and they
 *         return @c OVE_ERR_NOT_SUPPORTED too: while parsing the client key
 *         (as a key in no known format), in the TLS 1.3 key schedule (a
 *         generic error), while verifying the server's certificate (as a
 *         certificate that failed verification) and while verifying the
 *         server's handshake signature (as a handshake failure).
 */
OVE_NODISCARD int ove_tls_handshake(ove_tls_t tls, ove_socket_t sock, const ove_tls_config_t *cfg);

/**
 * @brief Send data over an encrypted TLS session.
 *
 * Sends at most one record, CONFIG_OVE_NET_TLS_OUT_CONTENT_LEN bytes
 * (4 KB by default), per call: when @p len is larger, @p sent is less
 * than @p len and the caller sends the rest with further calls.
 *
 * @param[in]  tls  TLS handle (after successful handshake).
 * @param[in]  data Pointer to data to send.
 * @param[in]  len  Number of bytes to send.
 * @param[out] sent Number of bytes actually sent (may be NULL only when
 *                  @p len fits one record).
 * @return OVE_OK on success, negative error code on failure.
 */
OVE_NODISCARD int ove_tls_send(ove_tls_t tls, const void *data, size_t len, size_t *sent);

/**
 * @brief Receive data from an encrypted TLS session.
 *
 * @param[in]  tls      TLS handle (after successful handshake).
 * @param[out] buf      Buffer to receive into.
 * @param[in]  len      Buffer size in bytes.
 * @param[out] received Number of bytes received (may be NULL).
 * @return OVE_OK on success, OVE_ERR_NET_CLOSED if peer closed.
 */
OVE_NODISCARD int ove_tls_recv(ove_tls_t tls, void *buf, size_t len, size_t *received);

/**
 * @brief Shut down the TLS session (sends close_notify).
 *
 * The underlying socket is NOT closed — caller must close it separately.
 *
 * @param[in] tls TLS handle.
 */
void ove_tls_close(ove_tls_t tls);

#ifdef OVE_HEAP_NET_TLS
/**
 * @brief Heap-allocate and initialise a TLS session.
 *
 * @param[out] tls Handle written on success.
 * @return OVE_OK on success, negative error code on failure.
 */
OVE_NODISCARD int ove_tls_create(ove_tls_t *tls);

/**
 * @brief Destroy a heap-allocated TLS session.
 *
 * @param[in] tls Handle returned by ove_tls_create().
 */
void ove_tls_destroy(ove_tls_t tls);
#endif /* OVE_HEAP_NET_TLS */

#else /* !CONFIG_OVE_NET_TLS */

/** @cond INTERNAL */
#ifndef CONFIG_OVE_NET_TLS
typedef struct {
	uint8_t _unused;
} ove_tls_storage_t;
#endif

OVE_NODISCARD static inline int ove_tls_init(ove_tls_t *tls, ove_tls_storage_t *storage)
{
	(void)tls;
	(void)storage;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_tls_deinit(ove_tls_t tls)
{
	(void)tls;
}
OVE_NODISCARD static inline int ove_tls_handshake(ove_tls_t tls, ove_socket_t sock,
						  const ove_tls_config_t *cfg)
{
	(void)tls;
	(void)sock;
	(void)cfg;
	return OVE_ERR_NOT_SUPPORTED;
}
OVE_NODISCARD static inline int ove_tls_send(ove_tls_t tls, const void *data, size_t len,
					     size_t *sent)
{
	(void)tls;
	(void)data;
	(void)len;
	(void)sent;
	return OVE_ERR_NOT_SUPPORTED;
}
OVE_NODISCARD static inline int ove_tls_recv(ove_tls_t tls, void *buf, size_t len, size_t *received)
{
	(void)tls;
	(void)buf;
	(void)len;
	(void)received;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_tls_close(ove_tls_t tls)
{
	(void)tls;
}
/** @endcond */

#endif /* CONFIG_OVE_NET_TLS */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_NET_TLS_H */
