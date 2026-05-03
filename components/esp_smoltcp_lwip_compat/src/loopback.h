#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "fd_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * In-process loopback for 127.0.0.0/8 UDP traffic.
 *
 * Why this exists: esp_http_server (and a few other IDF components)
 * create a UDP control-socket pair on 127.0.0.1 to interrupt their
 * own poll loops. With smoltcp as the data plane there's no real
 * loopback netif, so we keep these datagrams in RAM and never touch
 * the wire.
 *
 * Lookup is by local port only — fine for the control-socket pattern,
 * not a general-purpose loopback netif.
 */

/* Queued datagram. Allocated on send, freed on recv. */
typedef struct {
    uint32_t src_be;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[];        /* flexible array; len bytes */
} lo_msg_t;

/* Returns true if `ip_be` is in 127.0.0.0/8 (network-byte-order uint32). */
static inline bool is_loopback_ip(uint32_t ip_be)
{
    return (ip_be & 0xFF) == 127;
}

/* Register a UDP FD as a loopback receiver on `port`. The FD's lo_rx queue
 * must already be created. Replaces any existing registration on that port. */
esp_err_t lo_bind(uint16_t port, fd_entry_t *e);

/* Drop the registration for `port` if it points at `e`. */
void      lo_unbind(uint16_t port, fd_entry_t *e);

/* Push a datagram onto the loopback FD bound to `dst_port`. Returns the
 * number of bytes queued, or -1 if no FD is bound to that port / queue full. */
int       lo_deliver(uint16_t dst_port, uint32_t src_be, uint16_t src_port,
                     const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
