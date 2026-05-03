#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_smoltcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thin C handle over a smoltcp socket. Not BSD-compatible by design — the
 * semantics are smoltcp's (poll-driven, non-blocking-friendly). Helper
 * blocking calls are provided for convenience.
 *
 * Each call routes through the net_stack task; the handle itself is just
 * an opaque ID, safe to share across threads.
 */
typedef struct { uint32_t id; } net_sock_t;
#define NET_SOCK_INVALID ((net_sock_t){ .id = 0 })

static inline bool net_sock_valid(net_sock_t s) { return s.id != 0; }

/* TCP */
net_sock_t net_tcp_open(net_iface_id_t iface);
esp_err_t  net_tcp_connect(net_sock_t s, uint32_t ipv4_be, uint16_t port, uint32_t timeout_ms);
esp_err_t  net_tcp_listen(net_sock_t s, uint16_t port);
int        net_tcp_send(net_sock_t s, const void *buf, size_t len, uint32_t timeout_ms);
int        net_tcp_recv(net_sock_t s, void *buf, size_t len, uint32_t timeout_ms);
esp_err_t  net_tcp_close(net_sock_t s);
bool       net_tcp_is_active(net_sock_t s);
/* True only after a peer has connected (past LISTEN). */
bool       net_tcp_is_connected(net_sock_t s);
size_t     net_tcp_recv_queue(net_sock_t s);
size_t     net_tcp_send_capacity(net_sock_t s);

/* UDP */
net_sock_t net_udp_open(net_iface_id_t iface, uint16_t local_port);
int        net_udp_sendto(net_sock_t s, const void *buf, size_t len,
                          uint32_t dst_ipv4_be, uint16_t dst_port);
int        net_udp_recvfrom(net_sock_t s, void *buf, size_t len,
                            uint32_t *src_ipv4_be, uint16_t *src_port,
                            uint32_t timeout_ms);
esp_err_t  net_udp_close(net_sock_t s);

/* IP multicast (requires CONFIG_NETSTACK_IGMP) */
esp_err_t  net_mcast_join(net_iface_id_t iface, uint32_t group_ipv4_be);
esp_err_t  net_mcast_leave(net_iface_id_t iface, uint32_t group_ipv4_be);

#ifdef __cplusplus
}
#endif
