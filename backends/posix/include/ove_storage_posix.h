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
#include <time.h>
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sync primitives ──────────────────────────────────────────────── */

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

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	pthread_t tid;
	void (*entry)(void *arg);       /* ove_thread_fn */
	void *arg;
	int state;                      /* ove_thread_state_t */
	sem_t suspend_sem;
	int started;
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
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	timer_t posix_timer;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
	uint32_t period_ms;
	int one_shot;
	int created;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t bits;                  /* ove_eventbits_t */
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

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
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */

struct ove_watchdog {
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
typedef struct ove_dir  ove_dir_storage_t;

/* ── ML inference ─────────────────────────────────────────────────── */

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

/* ── Networking ──────────────────────────────────────────────────── */

#ifdef CONFIG_OVE_NET
struct ove_socket {
	int fd;
};

struct ove_netif {
	int initialized;
};

typedef struct ove_socket ove_socket_storage_t;
typedef struct ove_netif  ove_netif_storage_t;
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
	void        *ssl;
	void        *ssl_ctx;
	void        *conf;
	void        *entropy;
	void        *ctr_drbg;
	void        *cacert;
	void        *client_cert;  /* mbedtls_x509_crt * for mTLS */
	void        *client_key;   /* mbedtls_pk_context * for mTLS */
#ifdef CONFIG_OVE_ZERO_HEAP
	mbedtls_ssl_context      _ssl;
	mbedtls_ssl_config       _conf;
	mbedtls_entropy_context  _entropy;
	mbedtls_ctr_drbg_context _ctr_drbg;
	mbedtls_x509_crt         _cacert;
	mbedtls_x509_crt         _client_cert;
	mbedtls_pk_context       _client_key;
#endif
};

typedef struct ove_tls ove_tls_storage_t;
#endif /* CONFIG_OVE_NET_TLS */

#ifdef CONFIG_OVE_NET_HTTP
struct ove_http_client {
	void *tls;           /* ove_tls_t or NULL for plain HTTP */
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
	void        *tls;      /* ove_tls_t or NULL */
	uint8_t     *rx_buf;
	size_t       rx_size;
	uint8_t     *tx_buf;
	size_t       tx_size;
#ifdef CONFIG_OVE_ZERO_HEAP
	uint8_t      _rx_buf[CONFIG_OVE_NET_MQTT_RX_BUF];
	uint8_t      _tx_buf[CONFIG_OVE_NET_MQTT_TX_BUF];
#endif
	uint16_t     keep_alive_s;
	uint16_t     pkt_id;
	int          connected;
#ifndef CONFIG_OVE_NET_MQTT_MAX_SUBS
#define CONFIG_OVE_NET_MQTT_MAX_SUBS 8
#endif
	char         sub_filters[CONFIG_OVE_NET_MQTT_MAX_SUBS][64];
	unsigned int sub_count;
	void       (*on_message)(const char *topic, size_t topic_len,
				 const void *payload, size_t payload_len,
				 void *user_data);
	void        *user_data;
};

typedef struct ove_mqtt_client ove_mqtt_client_storage_t;
#endif /* CONFIG_OVE_NET_MQTT */

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_POSIX_H */
