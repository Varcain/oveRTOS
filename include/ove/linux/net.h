/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LINUX_NET_H
#define OVE_LINUX_NET_H

/**
 * @file net.h
 * @defgroup ove_linux_net Linux personality socket layer
 * @ingroup ove_linux
 * @brief BSD sockets for the Linux personality, bridged to the ove_net HAL.
 *
 * The socket-family syscalls (socket/connect/send/recv/...) of a loaded FDPIC
 * program are routed to a small in-kernel socket model that bridges to the
 * engine-neutral @c ove_socket_* HAL (lwIP / NuttX net / Zephyr net). It mirrors
 * the /dev device layer (@ref ove_linux_dev): a refcounted per-open pool, and a
 * park/retry deferral for blocking I/O.
 *
 * Blocking model: like the syscall layer, the entry points run in the
 * SVC/exception context and must NOT block inline. Every backing @c ove_socket
 * is put in non-blocking mode at open, and every op is called so it returns at
 * once; a would-block (@c OVE_ERR_TIMEOUT) parks the caller (@c sock_wait) and
 * the run-loop coordinator retries on its own thread — the same park/retry the
 * pipe and device layers use.
 *
 * @note Requires @c CONFIG_OVE_LINUX_NET.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include "ove/linux/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/** proc->sock_wait op codes: which parked socket op the coordinator retries.
 *  Shared with the run loop (backends/common/ove_lnx_run.c). */
#define OVE_LNX_SOCKW_CONNECT 1u
#define OVE_LNX_SOCKW_SEND 2u
#define OVE_LNX_SOCKW_RECV 3u
#define OVE_LNX_SOCKW_ACCEPT 4u /**< P4: a blocked accept(2). */
#define OVE_LNX_SOCKW_POLL 5u	/**< A blocking poll(2)/select over a set that includes a socket. */

/* Guest socket ABI constants — the Linux/ARM values FDPIC programs pass. */
#define OVE_LNX_AF_INET 2
#define OVE_LNX_AF_INET6 10
#define OVE_LNX_SOCK_STREAM 1
#define OVE_LNX_SOCK_DGRAM 2
#define OVE_LNX_SOCK_RAW 3
#define OVE_LNX_SOCK_NONBLOCK 0x800   /**< ORed into the type arg. */
#define OVE_LNX_SOCK_CLOEXEC 0x80000  /**< ORed into the type arg (ignored: no exec close). */
#define OVE_LNX_SOCK_TYPE_MASK 0xff   /**< Base type after masking the flag bits. */
#define OVE_LNX_IPPROTO_ICMP 1
#define OVE_LNX_IPPROTO_TCP 6
#define OVE_LNX_IPPROTO_UDP 17
#define OVE_LNX_SOL_SOCKET 1
#define OVE_LNX_SO_ERROR 4
#define OVE_LNX_MSG_DONTWAIT 0x40 /**< Per-call non-blocking hint for send/recv. */

/** @c struct sockaddr_in (Linux/ARM, 16 bytes). @c sin_port / @c sin_addr are
 *  in network byte order. */
typedef struct ove_lnx_sockaddr_in {
	uint16_t sin_family;
	uint16_t sin_port;
	uint32_t sin_addr;
	uint8_t sin_zero[8];
} ove_lnx_sockaddr_in;

/* ---- interface config ioctls (ifconfig/route, P2) ------------------------- */
#define OVE_LNX_SIOCADDRT 0x890b     /* add a routing table entry */
#define OVE_LNX_SIOCDELRT 0x890c     /* delete a routing table entry */
#define OVE_LNX_SIOCGIFCONF 0x8912   /* list interfaces */
#define OVE_LNX_SIOCGIFFLAGS 0x8913  /* get flags */
#define OVE_LNX_SIOCSIFFLAGS 0x8914  /* set flags (up/down) */
#define OVE_LNX_SIOCGIFADDR 0x8915   /* get IPv4 address */
#define OVE_LNX_SIOCSIFADDR 0x8916   /* set IPv4 address */
#define OVE_LNX_SIOCGIFBRDADDR 0x8919 /* get broadcast address */
#define OVE_LNX_SIOCGIFNETMASK 0x891b /* get netmask */
#define OVE_LNX_SIOCSIFNETMASK 0x891c /* set netmask */
#define OVE_LNX_SIOCGIFMTU 0x8921     /* get MTU */
#define OVE_LNX_SIOCGIFHWADDR 0x8927  /* get MAC */
#define OVE_LNX_SIOCGIFINDEX 0x8933   /* get interface index */

/* Linux interface flags (SIOC[GS]IFFLAGS ifr_flags). */
#define OVE_LNX_IFF_UP 0x1
#define OVE_LNX_IFF_BROADCAST 0x2
#define OVE_LNX_IFF_LOOPBACK 0x8
#define OVE_LNX_IFF_POINTOPOINT 0x10
#define OVE_LNX_IFF_RUNNING 0x40
#define OVE_LNX_IFF_MULTICAST 0x1000

#define OVE_LNX_ARPHRD_ETHER 1 /* ifr_hwaddr.sa_family for an ethernet MAC */
#define OVE_LNX_IFNAMSIZ 16

/** @c struct ifreq (Linux/ARM32): a 16-byte name + a 16-byte union (== 32 bytes). */
typedef struct ove_lnx_ifreq {
	char ifr_name[OVE_LNX_IFNAMSIZ];
	union {
		ove_lnx_sockaddr_in ifru_addr; /* SIOC[GS]IF{ADDR,NETMASK,BRDADDR} */
		int16_t ifru_flags;	       /* SIOC[GS]IFFLAGS */
		int32_t ifru_ivalue;	       /* SIOCGIFINDEX / SIOCGIFMTU */
		uint8_t ifru_raw[16];	       /* SIOCGIFHWADDR: sockaddr{sa_family, sa_data[14]} */
	} ifr_ifru;
} ove_lnx_ifreq;

/** @c struct ifconf (Linux/ARM32) for SIOCGIFCONF: len + a user pointer to ifreq[]. */
typedef struct ove_lnx_ifconf {
	int32_t ifc_len;  /* buffer size in bytes (in), used bytes (out) */
	uint32_t ifc_buf; /* user pointer to a struct ifreq array */
} ove_lnx_ifconf;

#define OVE_LNX_RTF_UP 0x0001
#define OVE_LNX_RTF_GATEWAY 0x0002

/** @c struct rtentry prefix (Linux/ARM32) for SIOC[AD]DRT — only the leading fields
 *  needed to read a default route's gateway; trailing rt_dev/rt_mtu/... are ignored. */
typedef struct ove_lnx_rtentry {
	uint32_t rt_pad1;
	ove_lnx_sockaddr_in rt_dst;
	ove_lnx_sockaddr_in rt_gateway;
	ove_lnx_sockaddr_in rt_genmask;
	uint16_t rt_flags;
} ove_lnx_rtentry;

/* ---- syscall-layer <-> socket-core interface (called from ove_linux_syscall.c) ---- */
/* Compiled only when CONFIG_OVE_LINUX_NET is set (the FD_SOCKET branches are #if'd),
 * so no weak fallbacks are needed — the core is always linked when the feature is on
 * (firmware) or under test (host cmocka). */

/** socket(2): allocate a socket open slot + open the backing ove_socket.
 *  @return the open-pool index (the fd's file_idx) or a negative Linux errno. */
long ove_lnx_sock_new(int domain, int type, int protocol);
/** Drop a reference on open @p oi (close/exit); @c ove_socket_close at the last. */
void ove_lnx_sock_close(int oi);
/** Add a reference on open @p oi (dup/fork inheritance). */
void ove_lnx_sock_get(int oi);
/** fcntl F_SETFL / F_GETFL: the open's status flags (O_NONBLOCK gates parking). */
void ove_lnx_sock_setfl(int oi, int flags);
int ove_lnx_sock_getfl(int oi);

/** connect(2). @p uaddr / @p addrlen are the guest's sockaddr. Returns 0, a
 *  negative Linux errno, or parks (returns 0 with @c p->sock_wait set). */
long ove_lnx_sock_connect(ove_lnx_proc_t *p, int oi, const void *uaddr, unsigned addrlen);
/** send(2)/sendto(2). @p udest NULL => send; non-NULL => sendto. Returns bytes
 *  sent, a negative Linux errno, or parks. */
long ove_lnx_sock_send(ove_lnx_proc_t *p, int oi, const void *ubuf, size_t len, int flags,
		       const void *udest, unsigned destlen);
/** recv(2)/recvfrom(2). @p usrc NULL => recv; non-NULL => recvfrom (fills
 *  @p usrc / @p usrclen). Returns bytes received, 0 (peer closed), errno, or parks. */
long ove_lnx_sock_recv(ove_lnx_proc_t *p, int oi, void *ubuf, size_t len, int flags, void *usrc,
		       void *usrclen);
long ove_lnx_sock_shutdown(int oi, int how);
long ove_lnx_sock_getsockname(ove_lnx_proc_t *p, int oi, void *uaddr, void *uaddrlen);
long ove_lnx_sock_getpeername(ove_lnx_proc_t *p, int oi, void *uaddr, void *uaddrlen);
long ove_lnx_sock_getsockopt(ove_lnx_proc_t *p, int oi, int level, int optname, void *uval,
			     void *ulen);
long ove_lnx_sock_setsockopt(ove_lnx_proc_t *p, int oi, int level, int optname, const void *uval,
			     unsigned len);
/** poll(2)/select: current readiness (OVE_LNX_POLLIN|OVE_LNX_POLLOUT); never blocks. */
unsigned ove_lnx_sock_poll(int oi);
/** Fill @c S_IFSOCK mode (+ size 0) for fstat/statx of a socket fd. */
void ove_lnx_sock_fstat(int oi, uint32_t *mode, uint64_t *size);

/* SIOC* interface-config ioctls (ifconfig/route) issued on a socket fd, bridged to the
 * ove_netif HAL. The op targets the interface, not one socket, so no open index. */
long ove_lnx_sock_ioctl(ove_lnx_proc_t *p, unsigned long req, unsigned long arg);

/* Register the interface handle the SIOC* ioctls operate on (opaque ove_netif_t; the
 * board/app calls this once at boot, after bringing the interface up). */
void ove_lnx_sock_set_netif(void *netif_handle);

/* Snapshot the registered interface for the /proc/net/{dev,route} generators. Any out
 * param may be NULL. Returns 0, or -1 if no interface is registered. */
int ove_lnx_sock_ifsnapshot(uint8_t ip[4], uint8_t gw[4], uint8_t nm[4], uint8_t mac[6],
			    unsigned *flags);

/** Retry a parked socket op for the coordinator; result or -EAGAIN (still blocked). */
long ove_lnx_sock_retry(ove_lnx_proc_t *p);

/* Re-scan a parked poll(2)/select's fd set for readiness (called from ove_lnx_sock_retry
 * for OVE_LNX_SOCKW_POLL). Implemented in the syscall TU, which owns the fd table + the
 * per-kind readiness probes. Returns the ready count (>0), 0 at the deadline, or -EAGAIN. */
long ove_lnx_poll_retry(ove_lnx_proc_t *p);
/** fork: the child inherited the parent's FD_SOCKET fds — add a reference to each. */
void ove_lnx_sock_fork_inherit(ove_lnx_proc_t *child);
/** exit: release every FD_SOCKET open the process still holds. */
void ove_lnx_sock_proc_exit(ove_lnx_proc_t *p);

/** access_ok for the socket handlers to validate a guest pointer (confused-deputy
 *  guard — handlers run PRIVILEGED). Defined in ove_linux_syscall.c. */
int user_ok(const ove_lnx_proc_t *p, const void *ptr, size_t len, int write);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LINUX_NET_H */
