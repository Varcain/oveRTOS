/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_ZEPHYR_H
#define OVE_STORAGE_ZEPHYR_H

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/atomic.h>

#include "ove/thread_state_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Storage-layout invariant
 * ------------------------
 * Every `ove_*_storage_t` below is a plain typedef of the corresponding
 * `struct ove_X` declared in this header. Backend `.c` files must NOT
 * redefine these structs locally — see the full rationale and enforcement
 * details at the top of `backends/freertos/include/ove_storage_freertos.h`.
 */

/* ── Sync primitives ──────────────────────────────────────────────── */

struct ove_mutex {
	struct k_mutex mtx;
};

struct ove_sem {
	struct k_sem sem;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

struct ove_event {
	struct k_sem sem;
};

struct ove_condvar {
	struct k_condvar cv;
};

typedef struct ove_mutex ove_mutex_storage_t;
typedef struct ove_sem ove_sem_storage_t;
typedef struct ove_event ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	struct k_thread thread;
	k_thread_stack_t *stack;
	size_t stack_size;
	int heap_stack;
	int state;		 /* ove_thread_state_t last observed */
	const char *name;	 /* caller-owned from desc->name */
	struct ove_thread *next; /* intrusive registry list */
#ifdef CONFIG_OVE_THREAD_STATE_STATS
	struct ove_state_tracker st;
#endif
	/* Cooperative cancellation flag.  Set by ove_thread_request_stop,
	 * polled by the worker via ove_thread_should_stop. */
	volatile int stop_requested;
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue ────────────────────────────────────────────────────────── */

struct ove_queue {
	struct k_msgq msgq;
	char *buffer;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	struct k_timer timer;
	struct k_work work;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint64_t period_ns;
	int one_shot;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	struct k_event event;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

struct ove_work {
	struct k_work_delayable dwork;
	void (*handler)(struct ove_work *);
};

struct ove_workqueue {
	struct k_work_q work_q;
	k_thread_stack_t *stack;
	size_t stack_size;
	int heap_stack;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	struct k_pipe pipe;
	unsigned char *buffer;
	size_t size;
	atomic_t bytes_count;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */

struct ove_watchdog {
	const struct device *dev;
	int channel_id;
	uint32_t timeout_ms;
	int started;
};

typedef struct ove_watchdog ove_watchdog_storage_t;

/* ── Filesystem ───────────────────────────────────────────────────── */

struct ove_file {
	struct fs_file_t file;
};

struct ove_dir {
	struct fs_dir_t dir;
};

typedef struct ove_file ove_file_storage_t;
typedef struct ove_dir ove_dir_storage_t;

/*
 * Zephyr thread stacks require K_THREAD_STACK_DEFINE for MPU alignment.
 * Override the generic OVE_THREAD_DEFINE / OVE_THREAD_DEFINE_STATIC
 * stack allocation to use the Zephyr macro.
 */
#define OVE_THREAD_STACK_DEFINE_(name, size) K_THREAD_STACK_DEFINE(name, size)

/* K_THREAD_STACK_DEFINE has external linkage; the comment in
 * thread_stack.h explicitly sanctions a leading 'static' to make it
 * file-local.  Used by OVE_TEST_STACK so per-TU stacks with reused names
 * (s_th_stack, s_wq_stack, …) don't collide at link time. */
#define OVE_THREAD_STACK_DEFINE_STATIC_(name, size) static K_THREAD_STACK_DEFINE(name, size)

/* Block-scope thread stack on Zephyr.  K_KERNEL_STACK_DEFINE is the
 * kernel-only stack flavour (no userspace, which we don't use) — it uses
 * Z_KERNEL_STACK_OBJ_ALIGN (a flat constant identifier) instead of
 * Z_THREAD_STACK_OBJ_ALIGN's MAX(...) ternary, which GCC's __aligned()
 * front-end won't accept as an integer-constant-expression at function
 * scope.  Replicating the K_KERNEL_STACK_DEFINE expansion locally with
 * `static` storage class lets the zero-heap public ove_thread_create /
 * ove_workqueue_create macros allocate stacks without touching
 * k_thread_stack_alloc, removing the CONFIG_DYNAMIC_THREAD dependency.
 *
 * GCC accepts the __kstackmem section attribute on function-scope
 * statics — the variable still has static storage duration, it just
 * lives in .kstackmem instead of .bss.  The Cortex-M MPU only requires
 * base alignment + a properly-sized buffer, both of which
 * Z_KERNEL_STACK_OBJ_ALIGN / K_KERNEL_STACK_LEN already enforce; works
 * under CONFIG_HW_STACK_PROTECTION=y. */
#define OVE_THREAD_STACK_BLOCK_STATIC_(name, size)                                           \
	static struct z_thread_stack_element __kstackmem __aligned(Z_KERNEL_STACK_OBJ_ALIGN) \
		name[K_KERNEL_STACK_LEN(size)]

#define OVE_THREAD_STACK_MEMBER_(name, size) K_KERNEL_STACK_MEMBER(name, size)

/* ── ML inference ─────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_INFER
struct ove_model {
	const void *model_data;
	size_t model_size;
	uint8_t *arena;
	size_t arena_size;
	void *interpreter;
	void *resolver;
	uint64_t last_invoke_us;
	int heap_allocated;
};

typedef struct ove_model ove_model_storage_t;
#endif /* CONFIG_OVE_INFER */

/* ── Networking ──────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_NET
struct ove_socket {
	int fd; /* Zephyr BSD socket descriptor */
};

struct ove_netif {
	int initialized;
};

typedef struct ove_socket ove_socket_storage_t;
typedef struct ove_netif ove_netif_storage_t;
#endif /* CONFIG_OVE_NET */

#ifdef CONFIG_OVE_NET_TLS
#ifdef CONFIG_OVE_ZERO_HEAP
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#endif
struct ove_tls {
	ove_socket_t sock;
	void *ssl;
	void *ssl_ctx;
	void *conf;
	void *entropy;
	void *ctr_drbg;
	void *cacert;
	void *client_cert; /* mbedtls_x509_crt * for mTLS */
	void *client_key;  /* mbedtls_pk_context * for mTLS */
#ifdef CONFIG_OVE_ZERO_HEAP
	mbedtls_ssl_context _ssl;
	mbedtls_ssl_config _conf;
	mbedtls_entropy_context _entropy;
	mbedtls_ctr_drbg_context _ctr_drbg;
	mbedtls_x509_crt _cacert;
	mbedtls_x509_crt _client_cert;
	mbedtls_pk_context _client_key;
#endif
};

typedef struct ove_tls ove_tls_storage_t;
#endif /* CONFIG_OVE_NET_TLS */

#ifdef CONFIG_OVE_NET_HTTP
struct ove_http_client {
	void *tls;
	ove_socket_t sock;
	char host[128];
	uint16_t port;
	int use_tls;
#ifdef CONFIG_OVE_ZERO_HEAP
	char _resp_buf[CONFIG_OVE_NET_HTTP_MAX_RESPONSE];
#endif
};

typedef struct ove_http_client ove_http_client_storage_t;
#endif /* CONFIG_OVE_NET_HTTP */

#ifdef CONFIG_OVE_NET_MQTT
struct ove_mqtt_client {
	ove_socket_t sock;
	ove_socket_storage_t sock_storage;
	void *tls;
	uint8_t *rx_buf;
	size_t rx_size;
	uint8_t *tx_buf;
	size_t tx_size;
#ifdef CONFIG_OVE_ZERO_HEAP
	uint8_t _rx_buf[CONFIG_OVE_NET_MQTT_RX_BUF];
	uint8_t _tx_buf[CONFIG_OVE_NET_MQTT_TX_BUF];
#endif
	uint16_t keep_alive_s;
	uint16_t pkt_id;
	int connected;
#ifndef CONFIG_OVE_NET_MQTT_MAX_SUBS
#define CONFIG_OVE_NET_MQTT_MAX_SUBS 8
#endif
	char sub_filters[CONFIG_OVE_NET_MQTT_MAX_SUBS][64];
	unsigned int sub_count;
	void (*on_message)(const char *topic, size_t topic_len, const void *payload,
			   size_t payload_len, void *user_data);
	void *user_data;
};

typedef struct ove_mqtt_client ove_mqtt_client_storage_t;
#endif /* CONFIG_OVE_NET_MQTT */

/* ── Bus drivers ─────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_UART
struct ove_uart {
	unsigned int instance;
	uint32_t baudrate;
	ove_stream_storage_t rx_stream_storage;
	ove_stream_t rx_stream;
	uint8_t *rx_buf;
	size_t rx_buf_size;
	ove_mutex_storage_t tx_mtx_storage;
	ove_mutex_t tx_mtx;
	const struct device *dev;
};

typedef struct ove_uart ove_uart_storage_t;
#endif /* CONFIG_OVE_UART */

#ifdef CONFIG_OVE_SPI
struct ove_spi {
	unsigned int instance;
	uint32_t clock_hz;
	uint8_t mode;
	uint8_t bit_order;
	uint8_t word_size;
	ove_mutex_storage_t bus_mtx_storage;
	ove_mutex_t bus_mtx;
	const struct device *dev;
#ifdef CONFIG_OVE_ASYNC
	ove_dma_complete_cb pending_cb;
	void *pending_ud;
	const void *pending_tx;
	void *pending_rx;
	size_t pending_len;
	const struct ove_spi_cs *pending_cs;
	struct k_work async_work;
	volatile int async_busy;
#endif
};

typedef struct ove_spi ove_spi_storage_t;
#endif /* CONFIG_OVE_SPI */

#ifdef CONFIG_OVE_I2S
struct ove_i2s {
	unsigned int instance;
	uint32_t sample_rate;
	uint8_t bit_depth;
	uint8_t channels;
	uint8_t direction;
	size_t dma_buf_samples;
	size_t half_buf_bytes;
	void *tx_dma_buf;
	void *rx_dma_buf;
	volatile uint8_t rx_completed_half;
	volatile uint8_t tx_completed_half;
	void (*rx_cb)(struct ove_i2s *, void *);
	void *rx_cb_user_data;
	void (*tx_cb)(struct ove_i2s *, void *);
	void *tx_cb_user_data;
	const struct device *dev;
};

typedef struct ove_i2s ove_i2s_storage_t;
#endif /* CONFIG_OVE_I2S */

#ifdef CONFIG_OVE_I2C
struct ove_i2c {
	unsigned int instance;
	uint32_t speed_hz;
	ove_mutex_storage_t bus_mtx_storage;
	ove_mutex_t bus_mtx;
	const struct device *dev;
#ifdef CONFIG_OVE_ASYNC
	ove_dma_complete_cb pending_cb;
	void *pending_ud;
	uint16_t pending_addr;
	const void *pending_tx;
	size_t pending_tx_len;
	void *pending_rx;
	size_t pending_rx_len;
	struct k_work async_work;
	volatile int async_busy;
#endif
};

typedef struct ove_i2c ove_i2c_storage_t;
#endif /* CONFIG_OVE_I2C */

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_ZEPHYR_H */
