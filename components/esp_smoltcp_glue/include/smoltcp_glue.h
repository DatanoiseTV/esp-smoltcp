#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * smoltcp_glue — C FFI surface of the Rust smoltcp core.
 *
 * Threading model: every call below must be made from the net_stack task
 * (or under its mutex). The Rust core is NOT internally thread-safe —
 * we keep it that way deliberately so it can be #![no_std] without
 * pulling in critical-section / spinlock crates.
 *
 * Sockets are operated through this same interface; the net_stack task
 * proxies all socket operations from app threads.
 */

typedef struct smoltcp_iface_s *smoltcp_iface_t;
typedef uint32_t smoltcp_socket_t;
#define SMOLTCP_SOCKET_INVALID 0

/* ---- core lifecycle --------------------------------------------------- */
void              smoltcp_core_init(void);
smoltcp_iface_t   smoltcp_iface_new(uint32_t iface_id, const uint8_t mac[6]);
void              smoltcp_iface_destroy(smoltcp_iface_t h);

/* ---- per-poll cycle --------------------------------------------------- */
/* Hand a received frame to smoltcp. Frame is borrowed; smoltcp copies what it needs. */
void              smoltcp_iface_rx(smoltcp_iface_t h, const uint8_t *frame, size_t len);

/* Run smoltcp once. May invoke smoltcp_glue_tx_cb to push frames out. */
void              smoltcp_iface_poll(smoltcp_iface_t h);

/* Microseconds until next deadline (DHCP retransmit, TCP retx, etc.).
 * Returns -1 if no deadline (caller may sleep indefinitely). */
int64_t           smoltcp_iface_poll_delay_us(smoltcp_iface_t h);

void              smoltcp_iface_set_link_up(smoltcp_iface_t h, bool up);

/* ---- IP state --------------------------------------------------------- */
bool              smoltcp_iface_has_ip(smoltcp_iface_t h);
uint32_t          smoltcp_iface_get_ipv4(smoltcp_iface_t h);   /* network byte order */
uint32_t          smoltcp_iface_get_gw(smoltcp_iface_t h);
uint32_t          smoltcp_iface_get_netmask(smoltcp_iface_t h);
/* IPv6: copies 16 bytes of the first configured v6 address (typically
 * the link-local) into `out`. Returns 1 on success, 0 if none. */
int               smoltcp_iface_get_ipv6(smoltcp_iface_t h, uint8_t out[16]);

/* Number of currently-allocated app sockets (TCP + UDP). */
uint32_t          smoltcp_iface_socket_count(smoltcp_iface_t h);
void              smoltcp_iface_set_static(smoltcp_iface_t h,
                                           uint32_t ipv4_be,
                                           uint32_t prefix_len,
                                           uint32_t gw_be);
void              smoltcp_iface_enable_dhcp(smoltcp_iface_t h);

/* ---- multicast (IGMP) ------------------------------------------------- */
int               smoltcp_iface_mcast_join(smoltcp_iface_t h, uint32_t group_be);
int               smoltcp_iface_mcast_leave(smoltcp_iface_t h, uint32_t group_be);

/* ---- TCP -------------------------------------------------------------- */
smoltcp_socket_t  smoltcp_tcp_open(smoltcp_iface_t h);
int               smoltcp_tcp_connect(smoltcp_iface_t h, smoltcp_socket_t s,
                                      uint32_t dst_ipv4_be, uint16_t dst_port,
                                      uint16_t local_port);
int               smoltcp_tcp_listen(smoltcp_iface_t h, smoltcp_socket_t s, uint16_t port);
int               smoltcp_tcp_send(smoltcp_iface_t h, smoltcp_socket_t s,
                                   const uint8_t *buf, size_t len);
int               smoltcp_tcp_recv(smoltcp_iface_t h, smoltcp_socket_t s,
                                   uint8_t *buf, size_t cap);
bool              smoltcp_tcp_is_active(smoltcp_iface_t h, smoltcp_socket_t s);
/* True only when past LISTEN — i.e. a peer has actually connected. */
bool              smoltcp_tcp_is_connected(smoltcp_iface_t h, smoltcp_socket_t s);
/* Bytes currently buffered for recv() / room available for send(). */
size_t            smoltcp_tcp_recv_queue(smoltcp_iface_t h, smoltcp_socket_t s);
size_t            smoltcp_tcp_send_capacity(smoltcp_iface_t h, smoltcp_socket_t s);
void              smoltcp_tcp_close(smoltcp_iface_t h, smoltcp_socket_t s);

/* ---- UDP -------------------------------------------------------------- */
smoltcp_socket_t  smoltcp_udp_open(smoltcp_iface_t h, uint16_t local_port);
int               smoltcp_udp_sendto(smoltcp_iface_t h, smoltcp_socket_t s,
                                     const uint8_t *buf, size_t len,
                                     uint32_t dst_ipv4_be, uint16_t dst_port);
int               smoltcp_udp_recvfrom(smoltcp_iface_t h, smoltcp_socket_t s,
                                       uint8_t *buf, size_t cap,
                                       uint32_t *src_ipv4_be, uint16_t *src_port);
void              smoltcp_udp_close(smoltcp_iface_t h, smoltcp_socket_t s);

/* ---- helpers ---------------------------------------------------------- */
/* "192.168.1.1" -> network-byte-order u32. Returns 0 on success, -1 on parse error. */
int               smoltcp_parse_ipv4(const char *s, uint32_t *out_be);

/* DNS A-record resolver. `name` may be a hostname or dotted-quad.
 * Returns 0 on success, -1 on failure. */
int               smoltcp_resolve(smoltcp_iface_t h, const char *name,
                                  uint32_t server_be, uint32_t timeout_ms,
                                  uint32_t *out_be);

/* ---- callbacks the Rust core calls into C ---------------------------- */
/*
 * These must be implemented by the C side. The Rust crate links against
 * them as `extern "C"` symbols.
 */
extern int        smoltcp_glue_tx_cb(uint32_t iface_id, const uint8_t *frame, size_t len);
extern int64_t    smoltcp_glue_now_us(void);
extern uint32_t   smoltcp_glue_rand32(void);

#ifdef __cplusplus
}
#endif
