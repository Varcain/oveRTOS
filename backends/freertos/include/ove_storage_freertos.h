/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_STORAGE_FREERTOS_H
#define OVE_STORAGE_FREERTOS_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "event_groups.h"
#include "stream_buffer.h"
#ifdef CONFIG_OVE_FS
#include "ff.h"
#endif

#include "ove/thread_state_stats.h"

/* The STM32 FreeRTOS-MPU coordinator sees guest SDRAM through privileged
 * Device attributes.  Its compiler and exception stack must therefore stay
 * in the linker's internal-DTCM host-stack region. */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define OVE_THREAD_STACK_DEFINE_HOST_(name, size) \
	static uint8_t name[(size)] __attribute__((section(".host_stacks"), aligned(32)))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Storage-layout invariant
 * ------------------------
 * Every `ove_*_storage_t` below is a plain typedef of the corresponding
 * `struct ove_X` declared in this header. Consumers — C apps, C++/Rust/Zig
 * bindings, and the Rust/Zig storage-size probes — size their backing
 * storage from this single source of truth.
 *
 * Backend `.c` files must NOT redefine `struct ove_X` locally. A local
 * redefinition diverges from what every other translation unit sees, so
 * `_init()` silently writes past the caller's declared storage slot and
 * corrupts adjacent memory (see the watchdog fix in commit history —
 * 20-byte STM32 IWDG struct overflowing an 8-byte stub).
 *
 * Enforcement lives in `ove lint`'s backend-struct guard
 * (`config/ove-cli/ove/lint_backend_struct.py`). The canary test suite
 * `tests/suites/test_storage_bounds.c` catches the runtime side.
 *
 * Exceptions: a new vendor driver whose handle type can't reasonably be
 * exposed in this header (e.g. a large board-specific HAL descriptor)
 * may be added to the `ALLOWLIST` in `lint_backend_struct.py` with a
 * one-line justification. The struct it declares must still match the
 * size of the `ove_X_storage_t` exposed here so consumers don't
 * over-allocate or under-allocate.
 */

/* ── Sync primitives ──────────────────────────────────────────────── */

struct ove_mutex {
	StaticSemaphore_t static_sem;
	SemaphoreHandle_t sem;
};

struct ove_sem {
	StaticSemaphore_t static_sem;
	SemaphoreHandle_t sem;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

struct ove_event {
	/* Level-triggered single-token latch + at-most-one registered waiter.
	 * `signaled` is the sticky latch (a signal posted before a wait is
	 * consumed by the next wait — keeps signal-then-wait and
	 * signal_from_isr working, which an earlier *edge-triggered*
	 * task-notification attempt broke); `waiter` is the blocked task to
	 * notify.  Accessed via __atomic_* (SEQ_CST) on plain fields so the
	 * C++ binding — which includes this header — isn't fed the C-only
	 * _Atomic keyword.  Replaces a binary semaphore, which cost ~2.5× the
	 * raw FreeRTOS task-notification path on signal+wait.  Single-waiter
	 * is within the API contract (ove_event_signal wakes "one waiting
	 * thread"); ove_event_wait re-checks the latch on every wake so a
	 * stale notification from another event sharing this task's single
	 * notification slot can never return a false wake. */
	unsigned int signaled;
	TaskHandle_t waiter;
};

struct ove_condvar {
	/* Waiter list head; updates are guarded by taskENTER_CRITICAL/EXIT,
	 * not a mutex.  Replaces the earlier (StaticSemaphore_t static_guard,
	 * SemaphoreHandle_t guard) pair which added 6× xSemaphoreTake/Give
	 * round trips per signal+wait (~5 µs on Cortex-M7). */
	struct condvar_waiter *head;
};

typedef struct ove_mutex ove_mutex_storage_t;
typedef struct ove_sem ove_sem_storage_t;
typedef struct ove_event ove_event_storage_t;
typedef struct ove_condvar ove_condvar_storage_t;

/* ── Thread ───────────────────────────────────────────────────────── */

struct ove_thread {
	TaskHandle_t task;
	StaticTask_t static_task;
	void (*entry)(void *);
	void *arg;
	/* Cooperative join: worker sets `exited=1` + DMB + reads `destroyer`
	 * after `entry()` returns; destroyer publishes `destroyer` + DMB +
	 * reads `exited`.  Whichever side wins the race notifies the other
	 * via the destroyer task's built-in 32-bit notification slot —
	 * replaces the earlier (StaticSemaphore_t + SemaphoreHandle_t) pair
	 * with no kernel object and a fast drain on the typical case
	 * (worker already done by destroy time). */
	volatile uint32_t exited;
	TaskHandle_t destroyer;
#ifdef CONFIG_OVE_THREAD_STATE_STATS
	/* Embedded per-thread state tracker. Required so the sim trace view
	 * can emit state transitions with sub-tick resolution. Updated from
	 * the traceTASK_SWITCHED_IN/OUT hooks in freertos_trace.c. */
	struct ove_state_tracker st;
#endif
	/* Cooperative cancellation flag.  Set by ove_thread_request_stop,
	 * polled by the worker via ove_thread_should_stop.  Accessed via
	 * __atomic_* builtins so the same field works in C and C++ TUs. */
	volatile int stop_requested;
#ifndef CONFIG_OVE_ZERO_HEAP
	/* When the port provides a separate stack heap, retain the independently allocated stack so
	 * ove_thread_destroy() can release it after deleting the static task.  Otherwise keep the
	 * single-allocation flexible-array layout used by ordinary heap builds.  Both forms are omitted
	 * in zero-heap mode because the heap-create API is absent and a FAM breaks C++ class layout when
	 * this storage struct is embedded as a non-last member. */
#if (configSTACK_ALLOCATION_FROM_SEPARATE_HEAP == 1)
	StackType_t *stack;
#else
	StackType_t stack[];
#endif
#endif
};

typedef struct ove_thread ove_thread_storage_t;

/* ── Queue ────────────────────────────────────────────────────────── */

struct ove_queue {
	StaticQueue_t static_queue;
	QueueHandle_t queue;
	ove_notify_cb notify_cb;
	void *notify_ud;
	/* Pointer to the queue's data buffer.  Init/zero-heap path: caller
	 * supplies `buffer` and we record it here.  Heap-create path: points
	 * at the inline_storage[] FAM tail so the wrapper struct + queue
	 * data live in one OVE_BACKEND_MALLOC block (was two — see the
	 * +3.5 µs delta tracked under the wrapper-overhead audit). */
	uint8_t *storage;
#ifndef CONFIG_OVE_ZERO_HEAP
	/* FAM omitted under CONFIG_OVE_ZERO_HEAP — see ove_thread.stack[] for
	 * the C++ class-layout reason. */
	uint8_t inline_storage[];
#endif
};

typedef struct ove_queue ove_queue_storage_t;

/* ── Timer ────────────────────────────────────────────────────────── */

struct ove_timer {
	StaticTimer_t static_timer;
	TimerHandle_t handle;
	void (*callback)(struct ove_timer *, void *);
	void *user_data;
};

typedef struct ove_timer ove_timer_storage_t;

/* ── Event group ──────────────────────────────────────────────────── */

struct ove_eventgroup {
	StaticEventGroup_t static_eg;
	EventGroupHandle_t handle;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_eventgroup ove_eventgroup_storage_t;

/* ── Workqueue ────────────────────────────────────────────────────── */

#define OVE_WQ_QUEUE_DEPTH 16

struct ove_work {
	void (*handler)(struct ove_work *);
	TimerHandle_t delay_timer;
	StaticTimer_t static_timer;
	struct ove_workqueue *target_wq;
	/* Completion synchronization (closes the cancel/free UAF window
	 * the POSIX TSan run flagged: cancel/free can race with the
	 * worker still inside the handler, freeing the struct out from
	 * under the worker).  in_progress is set/cleared by the worker
	 * around handler invocation (atomic store-release / load-acquire);
	 * completion_sem is given unconditionally after each handler so
	 * cancel/free can block on Take while in_progress is observed.
	 * Per-work semaphore costs ~80 B static — acceptable for the
	 * workqueue surface. */
	StaticSemaphore_t static_completion_sem;
	SemaphoreHandle_t completion_sem;
	int in_progress;
	/* Set when the item is queued/scheduled, cleared when the worker
	 * dequeues it; lets ove_work_cancel report OVE_ERR_INVAL for a
	 * not-pending item (atomic store-release / load-acquire). */
	int pending;
};

struct ove_workqueue {
	TaskHandle_t task;
	StaticTask_t static_task;
	StaticQueue_t static_queue;
	uint8_t queue_storage[OVE_WQ_QUEUE_DEPTH * sizeof(struct ove_work *)];
	QueueHandle_t queue;
	StaticSemaphore_t static_done_sem;
	SemaphoreHandle_t done_sem;
	volatile int running;
};

typedef struct ove_workqueue ove_workqueue_storage_t;
typedef struct ove_work ove_work_storage_t;

/* ── Stream ───────────────────────────────────────────────────────── */

struct ove_stream {
	/* FreeRTOS StreamBuffer's receive doesn't withhold sub-trigger data, so
	 * the stream is a plain ring honouring `trigger` directly: a critical
	 * section guards the ring (mutual exclusion vs. the *_from_isr ops) and
	 * two counting semaphores provide the blocking wait/wake. Sem counts are
	 * hints — every waiter re-checks the ring after waking. */
	unsigned char *buffer;
	size_t size;
	size_t trigger;
	size_t head;
	size_t tail;
	size_t count;
	SemaphoreHandle_t data_sem;  /* given when bytes arrive */
	SemaphoreHandle_t space_sem; /* given when space frees */
	StaticSemaphore_t data_sem_buf;
	StaticSemaphore_t space_sem_buf;
	ove_notify_cb notify_cb;
	void *notify_ud;
};

typedef struct ove_stream ove_stream_storage_t;

/* ── Watchdog ─────────────────────────────────────────────────────── */
/*
 * The FreeRTOS watchdog backend is board-specific.  Consumers that embed
 * `ove_watchdog_storage_t` must see the exact same size/layout the backend
 * writes, so the real struct is defined here — not just in the backend .c.
 * A defining .c unit can still set OVE_WATCHDOG_DEFINED first to override
 * with a custom layout.
 */
#ifndef OVE_WATCHDOG_DEFINED
#if defined(STM32F746xx) || defined(STM32F745xx) || defined(STM32F756xx) || defined(STM32F7)
#include "stm32f7xx_hal.h"
struct ove_watchdog {
	IWDG_HandleTypeDef hiwdg;
	uint32_t timeout_ms;
};
#define OVE_WATCHDOG_DEFINED
#else
struct ove_watchdog {
	uint32_t timeout_ms;
	int started;
};
#define OVE_WATCHDOG_DEFINED
#endif
#endif

typedef struct ove_watchdog ove_watchdog_storage_t;

/* ── Filesystem ───────────────────────────────────────────────────── */
#ifdef CONFIG_OVE_FS
struct ove_file {
	FIL fil;
	FSIZE_t position;
	int append;
};
struct ove_dir {
	DIR dir;
};
#else
struct ove_file {
	int fd;
};
struct ove_dir {
	void *dp;
};
#endif

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
	int fd;	      /* lwIP socket descriptor */
	int icmp_raw; /* AF_INET SOCK_RAW/IPPROTO_ICMP: MAC inserts the L4 csum */
};

struct ove_netif {
	int initialized;
	void *lwip_netif; /* struct netif * — opaque to avoid lwip header dep */
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
	UART_HandleTypeDef hal_handle;
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
	SPI_HandleTypeDef hal_handle;
#ifdef CONFIG_OVE_ASYNC
	ove_dma_complete_cb pending_cb;
	void *pending_ud;
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
	SAI_HandleTypeDef sai_tx;
	SAI_HandleTypeDef sai_rx;
	DMA_HandleTypeDef dma_tx;
	DMA_HandleTypeDef dma_rx;
};

typedef struct ove_i2s ove_i2s_storage_t;
#endif /* CONFIG_OVE_I2S */

#ifdef CONFIG_OVE_I2C
struct ove_i2c {
	unsigned int instance;
	uint32_t speed_hz;
	ove_mutex_storage_t bus_mtx_storage;
	ove_mutex_t bus_mtx;
	I2C_HandleTypeDef hal_handle;
#ifdef CONFIG_OVE_ASYNC
	ove_dma_complete_cb pending_cb;
	void *pending_ud;
	volatile int async_busy;
#endif
};

typedef struct ove_i2c ove_i2c_storage_t;
#endif /* CONFIG_OVE_I2C */

#ifdef __cplusplus
}
#endif

#endif /* OVE_STORAGE_FREERTOS_H */
