/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_POSIX_H
#define OVE_STORAGE_POSIX_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include "ove/thread_state_stats.h"
#include <time.h>
#include <dirent.h>

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
	pthread_mutex_t mtx;
};

struct ove_sem {
	sem_t sem;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

struct ove_event {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int signaled;
};

struct ove_condvar {
	pthread_cond_t cond;
};

typedef struct ove_mutex ove_mutex_storage_t;
typedef struct ove_sem ove_sem_storage_t;
typedef struct ove_event ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	pthread_t tid;
	void (*entry)(void *arg); /* ove_thread_fn */
	void *arg;
	int state; /* ove_thread_state_t */
	sem_t suspend_sem;
	int started;
	const char *name;	      /* thread name (from desc) */
	size_t stack_size;	      /* allocated stack size */
	void *stack_base;	      /* caller-allocated stack (painted) */
	size_t stack_headroom;	      /* final worker-owned coloration snapshot */
	uint8_t stack_headroom_valid; /* published with TERMINATED state */
	struct ove_thread *next;      /* linked list for enumeration */
	struct ove_state_tracker st;  /* per-state time tracking */
	uint8_t priority;	      /* ove_prio_t; tracked for reporting */
	/* CPU usage sampling: last observed CPU ns + last computed %.
	 * ove_thread_list recomputes at most every 100 ms so rapid
	 * callers don't collapse the delta window to near-zero. */
	uint64_t cpu_prev_ns;
	uint32_t cpu_pct_x100;
	uint8_t cpu_pct_valid;
#ifdef CONFIG_OVE_PROFILER
	/* 1 = a SIGRTMIN is in flight for this thread and the handler
	 * hasn't consumed it yet. Sampler uses this to avoid queuing
	 * multiple signals on a long-blocked thread. Single-byte atomic
	 * access is fine on all supported arches. */
	volatile int profiler_pending;
#endif
	/* Cooperative cancellation flag.  Set by ove_thread_request_stop,
	 * polled by the worker via ove_thread_should_stop.  Accessed via
	 * __atomic_* builtins so the same field works in C and C++ TUs
	 * (the C++ binding compiles this header). */
	volatile int stop_requested;
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue ────────────────────────────────────────────────────────── */

struct ove_queue {
	void *buffer;
	size_t item_size;
	unsigned int max_items;
	unsigned int count;
	unsigned int head;
	unsigned int tail;
	pthread_mutex_t lock;
	pthread_cond_t not_full;
	pthread_cond_t not_empty;
	/* Async wake hook — see ove_queue_set_notify in include/ove/queue.h. */
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint64_t period_ns;
	int one_shot;
	int created;

	/* Owned dispatcher thread + its control state.  We run our own timing
	 * thread (pthread_cond_timedwait on a MONOTONIC cond) instead of
	 * SIGEV_THREAD so that teardown can pthread_join the thread — closing
	 * the use-after-free window where a glibc-spawned notification thread
	 * could run after timer_delete()/free().  Flags are guarded by `lock`. */
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int armed;     /* timer running; should fire after `period_ns` */
	int reprogram; /* start/reset/set_period asked for a fresh deadline */
	int stop;      /* teardown asked the dispatcher to exit */
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t bits; /* ove_eventbits_t */
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

#define OVE_WQ_MAX_PENDING 64

struct ove_work {
	void (*handler)(struct ove_work *);
	uint32_t delay_ms;
	int pending;
	/* in_progress: set by the worker between dequeue and end-of-handler.
	 * Read by ove_work_cancel / ove_work_deinit under wq->lock to wait
	 * for any in-flight execution of this work item to finish before
	 * the caller frees it.  Closes the use-after-free TSan flagged
	 * when test_wq_cancel_work raced cancel+free against the worker. */
	int in_progress;
	/* wq: set by submit() under wq->lock; cleared on init.  Cancel /
	 * deinit read it to find the wq->cond on which the worker broadcasts
	 * completion. The worker clears it before publishing completion. */
	struct ove_workqueue *wq;
};

struct ove_workqueue {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct ove_work *queue[OVE_WQ_MAX_PENDING];
	int count;
	int running;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	uint8_t *buffer;
	size_t size;
	size_t trigger;
	size_t head;
	size_t tail;
	size_t count;
	pthread_mutex_t lock;
	pthread_cond_t data_avail;
	pthread_cond_t space_avail;
	/* Async wake hook — see ove_stream_set_notify in include/ove/stream.h.
	 * NULL when no callback is registered. */
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */

struct ove_watchdog {
	uint32_t timeout_ms;
	int started;
	volatile uint64_t last_feed_us;
};

typedef struct ove_watchdog ove_watchdog_storage_t;

/* ── Filesystem ───────────────────────────────────────────────────── */

struct ove_file {
	int fd;
};

struct ove_dir {
	DIR *dp;
};

typedef struct ove_file ove_file_storage_t;
typedef struct ove_dir ove_dir_storage_t;

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
	int fd;
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
	void *tls; /* ove_tls_t or NULL for plain HTTP */
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
	void *tls; /* ove_tls_t or NULL */
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
	int fd;
	pthread_t rx_thread;
	volatile int running;
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
	int fd;
#ifdef CONFIG_OVE_ASYNC
	/* Pending async transfer; valid between submit and completion. */
	ove_dma_complete_cb pending_cb;
	void *pending_ud;
	const void *pending_tx; /* worker reads these to avoid recapture */
	void *pending_rx;
	size_t pending_len;
	const struct ove_spi_cs *pending_cs;
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
	int fd;
};

typedef struct ove_i2s ove_i2s_storage_t;
#endif /* CONFIG_OVE_I2S */

#ifdef CONFIG_OVE_I2C
struct ove_i2c {
	unsigned int instance;
	uint32_t speed_hz;
	ove_mutex_storage_t bus_mtx_storage;
	ove_mutex_t bus_mtx;
	int fd;
#ifdef CONFIG_OVE_ASYNC
	ove_dma_complete_cb pending_cb;
	void *pending_ud;
	uint16_t pending_addr;
	const void *pending_tx;
	size_t pending_tx_len;
	void *pending_rx;
	size_t pending_rx_len;
	volatile int async_busy;
#endif
};

typedef struct ove_i2c ove_i2c_storage_t;
#endif /* CONFIG_OVE_I2C */

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_POSIX_H */
