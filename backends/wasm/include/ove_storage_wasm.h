/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Storage types for the WASM/Emscripten backend.
 *
 * Reuses the POSIX pthread-based sync/queue/eventgroup/stream/workqueue
 * types verbatim — they all work under Emscripten pthreads.
 *
 * Differs from the POSIX storage only in:
 *   - ove_thread: cooperative suspend (no SIGUSR1)
 *   - ove_timer: thread-based manager (no timer_create)
 */

#ifndef OVE_STORAGE_WASM_H
#define OVE_STORAGE_WASM_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sync primitives (identical to POSIX) ────────────────────────── */

struct ove_mutex {
	pthread_mutex_t mtx;
};

struct ove_sem {
	sem_t sem;
};

struct ove_event {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int signaled;
};

struct ove_condvar {
	pthread_cond_t cond;
};

typedef struct ove_mutex   ove_mutex_storage_t;
typedef struct ove_sem     ove_sem_storage_t;
typedef struct ove_event   ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread (cooperative suspend — no signals) ───────────────────── */

struct ove_thread {
	pthread_t tid;
	void (*entry)(void *arg);
	void *arg;
	int state;
	sem_t suspend_sem;
	int started;
	volatile int suspend_requested; /* cooperative suspend flag */
	volatile uint64_t last_yield_us; /* timestamp of last yield/sleep/block */
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue (identical to POSIX) ──────────────────────────────────── */

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
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer (thread-based manager — no timer_create) ──────────────── */

struct ove_timer {
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint32_t period_ms;
	int one_shot;
	int created;
	int armed;
	uint64_t next_fire_us;          /* absolute monotonic time */
	struct ove_timer *next_active;  /* intrusive linked list */
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group (identical to POSIX) ────────────────────────────── */

struct ove_eventgroup {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t bits;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue (identical to POSIX) ──────────────────────────────── */

#define OVE_WQ_MAX_PENDING 64

struct ove_work {
	void (*handler)(struct ove_work *);
	uint32_t delay_ms;
	int pending;
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
typedef struct ove_work     ove_work_storage_t;

/* ── Stream (identical to POSIX) ─────────────────────────────────── */

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
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog (identical to POSIX) ───────────────────────────────── */

struct ove_watchdog {
	uint32_t timeout_ms;
	int started;
	volatile uint64_t last_feed_us; /* supervisor checks this */
};

typedef struct ove_watchdog ove_watchdog_storage_t;

/* ── Filesystem (stub — no real FS in browser) ───────────────────── */

struct ove_file {
	int fd;
};

struct ove_dir {
	void *dp;
};

typedef struct ove_file ove_file_storage_t;
typedef struct ove_dir  ove_dir_storage_t;

/* ── ML inference ────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_INFER
struct ove_model {
	const void *model_data;
	size_t      model_size;
	uint8_t    *arena;
	size_t      arena_size;
	void       *interpreter;
	void       *resolver;
	uint64_t    last_invoke_us;
	int         heap_allocated;
};

typedef struct ove_model ove_model_storage_t;
#endif /* CONFIG_OVE_INFER */

/* ── Networking (stub — browser has no BSD sockets) ──────────────── */

#ifdef CONFIG_OVE_NET
struct ove_socket { int fd; };
struct ove_netif  { int initialized; };
typedef struct ove_socket ove_socket_storage_t;
typedef struct ove_netif  ove_netif_storage_t;
#endif

#ifdef CONFIG_OVE_NET_TLS
struct ove_tls {
	ove_socket_t sock;
	void *ssl;
	void *ssl_ctx;
	void *conf;
	void *entropy;
	void *ctr_drbg;
	void *cacert;
	void *client_cert;
	void *client_key;
};
typedef struct ove_tls ove_tls_storage_t;
#endif

#ifdef CONFIG_OVE_NET_HTTP
struct ove_http_client {
	void *tls;
	ove_socket_t sock;
	char host[128];
	uint16_t port;
	int use_tls;
};
typedef struct ove_http_client ove_http_client_storage_t;
#endif

#ifdef CONFIG_OVE_NET_MQTT
struct ove_mqtt_client {
	ove_socket_t sock;
	ove_socket_storage_t sock_storage;
	void        *tls;
	uint8_t     *rx_buf;
	size_t       rx_size;
	uint8_t     *tx_buf;
	size_t       tx_size;
	uint16_t     keep_alive_s;
	uint16_t     pkt_id;
	int          connected;
	char         sub_filters[8][64];
	unsigned int sub_count;
	void       (*on_message)(const char *, size_t, const void *, size_t, void *);
	void        *user_data;
};
typedef struct ove_mqtt_client ove_mqtt_client_storage_t;
#endif

/* ── Bus drivers (stubs for WASM) ────────────────────────────────── */

#ifdef CONFIG_OVE_UART
struct ove_uart {
	unsigned int         instance;
	uint32_t             baudrate;
	ove_stream_storage_t rx_stream_storage;
	ove_stream_t         rx_stream;
	uint8_t             *rx_buf;
	size_t               rx_buf_size;
	ove_mutex_storage_t  tx_mtx_storage;
	ove_mutex_t          tx_mtx;
	int                  fd;
	volatile int         running;
};
typedef struct ove_uart ove_uart_storage_t;
#endif

#ifdef CONFIG_OVE_SPI
struct ove_spi {
	unsigned int        instance;
	uint32_t            clock_hz;
	uint8_t             mode;
	uint8_t             bit_order;
	uint8_t             word_size;
	ove_mutex_storage_t bus_mtx_storage;
	ove_mutex_t         bus_mtx;
	int                 fd;
};
typedef struct ove_spi ove_spi_storage_t;
#endif

#ifdef CONFIG_OVE_I2S
struct ove_i2s {
	unsigned int     instance;
	uint32_t         sample_rate;
	uint8_t          bit_depth;
	uint8_t          channels;
	uint8_t          direction;
	size_t           dma_buf_samples;
	size_t           half_buf_bytes;
	void            *tx_dma_buf;
	void            *rx_dma_buf;
	volatile uint8_t rx_completed_half;
	volatile uint8_t tx_completed_half;
	void           (*rx_cb)(struct ove_i2s *, void *);
	void            *rx_cb_user_data;
	void           (*tx_cb)(struct ove_i2s *, void *);
	void            *tx_cb_user_data;
	int              fd;
};
typedef struct ove_i2s ove_i2s_storage_t;
#endif

#ifdef CONFIG_OVE_I2C
struct ove_i2c {
	unsigned int        instance;
	uint32_t            speed_hz;
	ove_mutex_storage_t bus_mtx_storage;
	ove_mutex_t         bus_mtx;
	int                 fd;
};
typedef struct ove_i2c ove_i2c_storage_t;
#endif

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_WASM_H */
