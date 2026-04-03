/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Simulated network stack — fully in-process loopback.
 *
 * All TCP/UDP connections go through in-memory ring-buffer "pipes".
 * No real network I/O.  Apps talk to each other and to virtual
 * servers (httpd, MQTT broker) within the same process.
 *
 * Replaces posix_net.c when CONFIG_OVE_SIM=y.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_NET

#include "ove/net.h"
#include "ove/types.h"
#include "ove_backend_common.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Pipe: bidirectional byte stream between two sockets ───────────── */

#define PIPE_BUF_SIZE 8192

struct sim_pipe {
	uint8_t         a_to_b[PIPE_BUF_SIZE];
	uint32_t        a_to_b_wp;
	uint32_t        a_to_b_rp;

	uint8_t         b_to_a[PIPE_BUF_SIZE];
	uint32_t        b_to_a_wp;
	uint32_t        b_to_a_rp;

	pthread_mutex_t lock;
	pthread_cond_t  cond; /* signaled on any write or close */
	int             closed_a; /* side A closed */
	int             closed_b; /* side B closed */
	int             refcount;
};

static struct sim_pipe *pipe_create(void)
{
	struct sim_pipe *p = calloc(1, sizeof(*p));
	if (!p) return NULL;
	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->cond, NULL);
	p->refcount = 2;
	return p;
}

static void pipe_unref(struct sim_pipe *p)
{
	if (!p) return;
	pthread_mutex_lock(&p->lock);
	p->refcount--;
	int dead = (p->refcount <= 0);
	pthread_mutex_unlock(&p->lock);
	if (dead) {
		pthread_cond_destroy(&p->cond);
		pthread_mutex_destroy(&p->lock);
		free(p);
	}
}

/* Write to ring (caller holds lock). Returns bytes written. */
static size_t ring_write(uint8_t *buf, uint32_t *wp, uint32_t rp,
			 const void *data, size_t len)
{
	uint32_t avail = PIPE_BUF_SIZE - (*wp - rp);
	if (len > avail) len = avail;
	const uint8_t *src = (const uint8_t *)data;
	for (size_t i = 0; i < len; i++) {
		buf[(*wp) & (PIPE_BUF_SIZE - 1)] = src[i];
		(*wp)++;
	}
	return len;
}

/* Read from ring (caller holds lock). Returns bytes read. */
static size_t ring_read(uint8_t *buf, uint32_t wp, uint32_t *rp,
			void *out, size_t len)
{
	uint32_t avail = wp - *rp;
	if (len > avail) len = avail;
	uint8_t *dst = (uint8_t *)out;
	for (size_t i = 0; i < len; i++) {
		dst[i] = buf[(*rp) & (PIPE_BUF_SIZE - 1)];
		(*rp)++;
	}
	return len;
}

/* ── Listener: tracks bound+listening ports ────────────────────────── */

#define MAX_LISTENERS 16
#define ACCEPT_QUEUE  8

struct sim_listener {
	uint16_t          port;
	ove_sock_type_t   type;
	int               active;
	struct sim_pipe  *pending[ACCEPT_QUEUE]; /* pipes waiting for accept */
	int               pend_count;
	pthread_mutex_t   lock;
	pthread_cond_t    cond;
};

static struct sim_listener listeners[MAX_LISTENERS];
static pthread_mutex_t listener_lock = PTHREAD_MUTEX_INITIALIZER;

static struct sim_listener *listener_find(uint16_t port)
{
	for (int i = 0; i < MAX_LISTENERS; i++)
		if (listeners[i].active && listeners[i].port == port)
			return &listeners[i];
	return NULL;
}

static struct sim_listener *listener_alloc(uint16_t port, ove_sock_type_t type)
{
	for (int i = 0; i < MAX_LISTENERS; i++) {
		if (!listeners[i].active) {
			struct sim_listener *l = &listeners[i];
			memset(l, 0, sizeof(*l));
			l->port = port;
			l->type = type;
			l->active = 1;
			pthread_mutex_init(&l->lock, NULL);
			pthread_cond_init(&l->cond, NULL);
			return l;
		}
	}
	return NULL;
}

/* ── Socket state ──────────────────────────────────────────────────── */

enum sim_sock_state {
	SS_CREATED,
	SS_BOUND,
	SS_LISTENING,
	SS_CONNECTED,
	SS_CLOSED,
};

/* Extended socket — appended after the opaque ove_socket storage. */
struct sim_sock_ext {
	ove_af_t         af;
	ove_sock_type_t  type;
	enum sim_sock_state state;
	uint16_t         local_port;
	uint16_t         remote_port;
	struct sim_pipe *pipe;
	int              is_side_a;  /* which side of the pipe we are */
	struct sim_listener *listener; /* if listening */
};

static struct sim_sock_ext *ext_of(ove_socket_t sock)
{
	/* Store the ext in the fd field of the storage — it's unused. */
	return (struct sim_sock_ext *)(void *)&sock->fd;
}

/* We need more space than ove_socket_storage_t provides (it's just
 * "int fd").  Use a separate allocation alongside the socket. */

#define MAX_SIM_SOCKETS 32
static struct sim_sock_ext sim_sockets[MAX_SIM_SOCKETS];
static pthread_mutex_t sock_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static struct sim_sock_ext *sock_alloc(void)
{
	pthread_mutex_lock(&sock_alloc_lock);
	for (int i = 0; i < MAX_SIM_SOCKETS; i++) {
		if (sim_sockets[i].state == SS_CLOSED) {
			memset(&sim_sockets[i], 0, sizeof(sim_sockets[i]));
			sim_sockets[i].state = SS_CREATED;
			pthread_mutex_unlock(&sock_alloc_lock);
			return &sim_sockets[i];
		}
	}
	pthread_mutex_unlock(&sock_alloc_lock);
	return NULL;
}

static void sock_set(ove_socket_t sock, struct sim_sock_ext *e)
{
	/* Store pointer in the fd field. */
	memcpy(&sock->fd, &e, sizeof(e));
}

static struct sim_sock_ext *sock_get(ove_socket_t sock)
{
	struct sim_sock_ext *e;
	memcpy(&e, &sock->fd, sizeof(e));
	return e;
}

/* ── Auto-assign ephemeral port ────────────────────────────────────── */

static uint16_t next_ephemeral = 49152;

static uint16_t alloc_ephemeral(void)
{
	return next_ephemeral++;
}

/* ── Timed wait helper ─────────────────────────────────────────────── */

static int cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m,
			     uint32_t timeout_ms)
{
	if (timeout_ms == OVE_WAIT_FOREVER) {
		pthread_cond_wait(c, m);
		return 0;
	}
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_ms / 1000;
	ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	return pthread_cond_timedwait(c, m, &ts);
}

/* ═══════════════════════════════════════════════════════════════════
   Network Interface (always succeeds — simulated)
   ═══════════════════════════════════════════════════════════════════ */

static ove_sockaddr_t sim_ip;
static int netif_up_flag;

int ove_netif_init(ove_netif_t *netif, ove_netif_storage_t *storage)
{
	if (!netif || !storage) return OVE_ERR_INVALID_PARAM;
	struct ove_netif *n = (struct ove_netif *)storage;
	n->initialized = 1;
	*netif = n;
	return OVE_OK;
}

void ove_netif_deinit(ove_netif_t netif)
{
	if (netif) netif->initialized = 0;
	netif_up_flag = 0;
}

int ove_netif_up(ove_netif_t netif, const ove_netif_config_t *cfg)
{
	(void)netif;
	/* Sim: always assign 127.0.0.1 (loopback). */
	memset(&sim_ip, 0, sizeof(sim_ip));
	sim_ip.family = OVE_AF_INET;
	sim_ip.addr[0] = 127;
	sim_ip.addr[3] = 1;
	if (cfg && !cfg->use_dhcp)
		sim_ip = cfg->static_ip;
	netif_up_flag = 1;
	return OVE_OK;
}

void ove_netif_down(ove_netif_t netif)
{
	(void)netif;
	netif_up_flag = 0;
}

int ove_netif_get_addr(ove_netif_t netif, ove_sockaddr_t *ip,
		       ove_sockaddr_t *gateway, ove_sockaddr_t *netmask)
{
	(void)netif;
	if (ip) *ip = sim_ip;
	if (gateway) {
		memset(gateway, 0, sizeof(*gateway));
		gateway->family = OVE_AF_INET;
		gateway->addr[0] = 127; gateway->addr[3] = 1;
	}
	if (netmask) {
		memset(netmask, 0, sizeof(*netmask));
		netmask->family = OVE_AF_INET;
		netmask->addr[0] = 255;
	}
	return OVE_OK;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_netif_create(ove_netif_t *netif)
{
	if (!netif) return OVE_ERR_INVALID_PARAM;
	struct ove_netif *n = OVE_BACKEND_MALLOC(sizeof(*n));
	if (!n) return OVE_ERR_NO_MEMORY;
	n->initialized = 1;
	*netif = n;
	return OVE_OK;
}

void ove_netif_destroy(ove_netif_t netif)
{
	if (netif) OVE_BACKEND_FREE(netif);
}
#endif

/* ═══════════════════════════════════════════════════════════════════
   Sockets
   ═══════════════════════════════════════════════════════════════════ */

int ove_socket_open(ove_socket_t *sock, ove_socket_storage_t *storage,
		    ove_af_t af, ove_sock_type_t type)
{
	if (!sock || !storage) return OVE_ERR_INVALID_PARAM;
	struct ove_socket *s = (struct ove_socket *)storage;
	struct sim_sock_ext *e = sock_alloc();
	if (!e) return OVE_ERR_NO_MEMORY;
	e->af = af;
	e->type = type;
	sock_set(s, e);
	*sock = s;
	return OVE_OK;
}

void ove_socket_close(ove_socket_t sock)
{
	if (!sock) return;
	struct sim_sock_ext *e = sock_get(sock);
	if (!e) return;

	if (e->pipe) {
		pthread_mutex_lock(&e->pipe->lock);
		if (e->is_side_a)
			e->pipe->closed_a = 1;
		else
			e->pipe->closed_b = 1;
		pthread_cond_broadcast(&e->pipe->cond);
		pthread_mutex_unlock(&e->pipe->lock);
		pipe_unref(e->pipe);
		e->pipe = NULL;
	}
	if (e->listener) {
		pthread_mutex_lock(&listener_lock);
		e->listener->active = 0;
		pthread_cond_broadcast(&e->listener->cond);
		pthread_mutex_unlock(&listener_lock);
		e->listener = NULL;
	}
	e->state = SS_CLOSED;
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_socket_create(ove_socket_t *sock, ove_af_t af, ove_sock_type_t type)
{
	if (!sock) return OVE_ERR_INVALID_PARAM;
	struct ove_socket *s = OVE_BACKEND_MALLOC(sizeof(*s));
	if (!s) return OVE_ERR_NO_MEMORY;
	struct sim_sock_ext *e = sock_alloc();
	if (!e) { OVE_BACKEND_FREE(s); return OVE_ERR_NO_MEMORY; }
	e->af = af;
	e->type = type;
	sock_set(s, e);
	*sock = s;
	return OVE_OK;
}

void ove_socket_destroy(ove_socket_t sock)
{
	ove_socket_close(sock);
	if (sock) OVE_BACKEND_FREE(sock);
}
#endif

int ove_socket_bind(ove_socket_t sock, const ove_sockaddr_t *addr)
{
	if (!sock || !addr) return OVE_ERR_INVALID_PARAM;
	struct sim_sock_ext *e = sock_get(sock);
	e->local_port = addr->port;
	e->state = SS_BOUND;
	return OVE_OK;
}

int ove_socket_listen(ove_socket_t sock, int backlog)
{
	(void)backlog;
	if (!sock) return OVE_ERR_INVALID_PARAM;
	struct sim_sock_ext *e = sock_get(sock);
	if (e->state != SS_BOUND) return OVE_ERR_INVALID_PARAM;

	pthread_mutex_lock(&listener_lock);
	if (listener_find(e->local_port)) {
		pthread_mutex_unlock(&listener_lock);
		return OVE_ERR_NET_ADDR_IN_USE;
	}
	struct sim_listener *l = listener_alloc(e->local_port, e->type);
	if (!l) {
		pthread_mutex_unlock(&listener_lock);
		return OVE_ERR_NO_MEMORY;
	}
	e->listener = l;
	e->state = SS_LISTENING;
	pthread_mutex_unlock(&listener_lock);
	return OVE_OK;
}

int ove_socket_accept(ove_socket_t sock, ove_socket_t *client,
		      ove_socket_storage_t *client_storage,
		      uint32_t timeout_ms)
{
	if (!sock || !client || !client_storage) return OVE_ERR_INVALID_PARAM;
	struct sim_sock_ext *e = sock_get(sock);
	if (!e->listener) return OVE_ERR_INVALID_PARAM;

	struct sim_listener *l = e->listener;
	pthread_mutex_lock(&l->lock);

	while (l->pend_count == 0 && l->active) {
		if (cond_timedwait_ms(&l->cond, &l->lock, timeout_ms) != 0) {
			pthread_mutex_unlock(&l->lock);
			return OVE_ERR_TIMEOUT;
		}
	}
	if (!l->active || l->pend_count == 0) {
		pthread_mutex_unlock(&l->lock);
		return OVE_ERR_NET_CLOSED;
	}

	/* Dequeue the first pending pipe. */
	struct sim_pipe *p = l->pending[0];
	for (int i = 0; i < l->pend_count - 1; i++)
		l->pending[i] = l->pending[i + 1];
	l->pend_count--;
	pthread_mutex_unlock(&l->lock);

	/* Create the accepted socket (side B of the pipe). */
	struct ove_socket *cs = (struct ove_socket *)client_storage;
	struct sim_sock_ext *ce = sock_alloc();
	if (!ce) { pipe_unref(p); return OVE_ERR_NO_MEMORY; }
	ce->af = e->af;
	ce->type = e->type;
	ce->state = SS_CONNECTED;
	ce->pipe = p;
	ce->is_side_a = 0; /* server is side B */
	sock_set(cs, ce);
	*client = cs;
	return OVE_OK;
}

int ove_socket_connect(ove_socket_t sock, const ove_sockaddr_t *addr,
		       uint32_t timeout_ms)
{
	(void)timeout_ms;
	if (!sock || !addr) return OVE_ERR_INVALID_PARAM;
	struct sim_sock_ext *e = sock_get(sock);

	uint16_t port = addr->port;

	/* Find a listener on this port. */
	pthread_mutex_lock(&listener_lock);
	struct sim_listener *l = listener_find(port);
	if (!l) {
		pthread_mutex_unlock(&listener_lock);
		return OVE_ERR_NET_REFUSED;
	}
	pthread_mutex_unlock(&listener_lock);

	/* Create a pipe and enqueue on the listener. */
	struct sim_pipe *p = pipe_create();
	if (!p) return OVE_ERR_NO_MEMORY;

	pthread_mutex_lock(&l->lock);
	if (l->pend_count >= ACCEPT_QUEUE) {
		pthread_mutex_unlock(&l->lock);
		pipe_unref(p);
		pipe_unref(p);
		return OVE_ERR_NET_REFUSED;
	}
	l->pending[l->pend_count++] = p;
	pthread_cond_signal(&l->cond);
	pthread_mutex_unlock(&l->lock);

	/* Client is side A. */
	e->pipe = p;
	e->is_side_a = 1;
	e->state = SS_CONNECTED;
	e->remote_port = port;
	if (e->local_port == 0)
		e->local_port = alloc_ephemeral();

	return OVE_OK;
}

int ove_socket_send(ove_socket_t sock, const void *data, size_t len,
		    size_t *sent)
{
	if (!sock || !data) return OVE_ERR_INVALID_PARAM;
	struct sim_sock_ext *e = sock_get(sock);
	if (!e->pipe) return OVE_ERR_NET_CLOSED;

	struct sim_pipe *p = e->pipe;
	pthread_mutex_lock(&p->lock);

	int peer_closed = e->is_side_a ? p->closed_b : p->closed_a;
	if (peer_closed) {
		pthread_mutex_unlock(&p->lock);
		return OVE_ERR_NET_RESET;
	}

	size_t n;
	if (e->is_side_a)
		n = ring_write(p->a_to_b, &p->a_to_b_wp, p->a_to_b_rp,
			       data, len);
	else
		n = ring_write(p->b_to_a, &p->b_to_a_wp, p->b_to_a_rp,
			       data, len);

	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->lock);

	if (sent) *sent = n;
	return OVE_OK;
}

int ove_socket_recv(ove_socket_t sock, void *buf, size_t len,
		    size_t *received, uint32_t timeout_ms)
{
	if (!sock || !buf) return OVE_ERR_INVALID_PARAM;
	struct sim_sock_ext *e = sock_get(sock);
	if (!e->pipe) return OVE_ERR_NET_CLOSED;

	struct sim_pipe *p = e->pipe;
	pthread_mutex_lock(&p->lock);

	/* Wait for data or peer close. */
	for (;;) {
		uint32_t avail;
		int peer_closed;
		if (e->is_side_a) {
			avail = p->b_to_a_wp - p->b_to_a_rp;
			peer_closed = p->closed_b;
		} else {
			avail = p->a_to_b_wp - p->a_to_b_rp;
			peer_closed = p->closed_a;
		}

		if (avail > 0) break;
		if (peer_closed) {
			pthread_mutex_unlock(&p->lock);
			if (received) *received = 0;
			return OVE_ERR_NET_CLOSED;
		}

		if (cond_timedwait_ms(&p->cond, &p->lock, timeout_ms) != 0) {
			pthread_mutex_unlock(&p->lock);
			return OVE_ERR_TIMEOUT;
		}
	}

	size_t n;
	if (e->is_side_a)
		n = ring_read(p->b_to_a, p->b_to_a_wp, &p->b_to_a_rp,
			      buf, len);
	else
		n = ring_read(p->a_to_b, p->a_to_b_wp, &p->a_to_b_rp,
			      buf, len);

	pthread_mutex_unlock(&p->lock);
	if (received) *received = n;
	return OVE_OK;
}

/* ── UDP (sendto/recvfrom) — simplified: loopback only ─────────────── */

int ove_socket_sendto(ove_socket_t sock, const void *data, size_t len,
		      size_t *sent, const ove_sockaddr_t *dest)
{
	/* For UDP loopback, auto-connect to the dest port and use send. */
	struct sim_sock_ext *e = sock_get(sock);
	if (e->state != SS_CONNECTED && dest) {
		int ret = ove_socket_connect(sock, dest, 1000);
		if (ret != OVE_OK) return ret;
	}
	return ove_socket_send(sock, data, len, sent);
}

int ove_socket_recvfrom(ove_socket_t sock, void *buf, size_t len,
			size_t *received, ove_sockaddr_t *src,
			uint32_t timeout_ms)
{
	if (src) {
		memset(src, 0, sizeof(*src));
		src->family = OVE_AF_INET;
		src->addr[0] = 127; src->addr[3] = 1;
	}
	return ove_socket_recv(sock, buf, len, received, timeout_ms);
}

/* ── DNS (sim table) ───────────────────────────────────────────────── */

int ove_dns_resolve(const char *hostname, ove_sockaddr_t *addr,
		    uint32_t timeout_ms)
{
	(void)timeout_ms;
	if (!hostname || !addr) return OVE_ERR_INVALID_PARAM;

	/* Sim: all hostnames resolve to 127.0.0.1 (loopback). */
	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	addr->addr[0] = 127;
	addr->addr[3] = 1;
	return OVE_OK;
}

/* ── sockaddr helper ───────────────────────────────────────────────── */

void ove_sockaddr_ipv4(ove_sockaddr_t *addr, uint8_t a, uint8_t b,
		       uint8_t c, uint8_t d, uint16_t port)
{
	if (!addr) return;
	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	addr->port = port;
	addr->addr[0] = a;
	addr->addr[1] = b;
	addr->addr[2] = c;
	addr->addr[3] = d;
}

#endif /* CONFIG_OVE_NET */
