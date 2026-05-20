/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_NUTTX_H
#define OVE_STORAGE_NUTTX_H

#include <stdint.h>
#include <stddef.h>
#include <nuttx/config.h>
#include <pthread.h>
#include <semaphore.h>
#include <nuttx/mutex.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>

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
	union {
		mutex_t mtx;
		rmutex_t rmtx;
	};
};

struct ove_sem {
	sem_t sem;
};

struct ove_event {
	sem_t sem;
};

struct ove_condvar {
	sem_t waiter;
	mutex_t guard;
	int nwaiters;
};

typedef struct ove_mutex ove_mutex_storage_t;
typedef struct ove_sem ove_sem_storage_t;
typedef struct ove_event ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	pid_t pid;
	void (*entry)(void *arg);
	void *arg;
	int state;
	sem_t suspend_sem;
	sem_t done_sem;
	int suspend_inited;
	int started;
	const char *name;	 /* caller-owned from desc->name */
	struct ove_thread *next; /* intrusive enumeration list */
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
	void *buffer;
	size_t item_size;
	unsigned int max_items;
	unsigned int head;
	unsigned int tail;
	sem_t not_full;
	sem_t not_empty;
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	timer_t posix_timer;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint64_t period_ns;
	int one_shot;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	sem_t waiter;
	ove_eventbits_t bits;
	int nwaiters;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

#define OVE_WQ_QUEUE_DEPTH 16

struct ove_work {
	void (*handler)(struct ove_work *);
	volatile int cancelled;
	uint32_t delay_ms;
	int pending;
	/* Completion synchronization — same shape as POSIX/FreeRTOS:
	 * worker sets in_progress around handler invocation and posts
	 * completion_sem on every iteration; cancel/free wait on the
	 * sem while in_progress is observed.  Closes the use-after-free
	 * window where ove_work_free could reclaim the struct while
	 * wq_task_fn was still inside w->handler. */
	int in_progress;
	sem_t completion_sem;
	int completion_sem_inited;
};

struct ove_workqueue {
	pid_t worker_pid;
	struct ove_work *ring[OVE_WQ_QUEUE_DEPTH];
	unsigned int head;
	unsigned int tail;
	mutex_t lock;
	sem_t not_full;
	sem_t not_empty;
	sem_t delay_sem;
	volatile int running;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	unsigned char *buffer;
	size_t size;
	size_t head;
	size_t tail;
	size_t count;
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */

struct ove_watchdog {
	int fd;
	uint32_t timeout_ms;
	int started;
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
	int fd; /* NuttX POSIX socket descriptor */
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
};

typedef struct ove_i2c ove_i2c_storage_t;
#endif /* CONFIG_OVE_I2C */

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_NUTTX_H */
