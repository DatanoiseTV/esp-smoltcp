#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_smoltcp.h"
#include "esp_smoltcp_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * lwip_compat FD table.
 *
 * BSD socket FDs returned by our wrapped socket()/accept() are integers in
 * the range [LWIP_SOCKET_OFFSET .. LWIP_SOCKET_OFFSET + MAX_FDS). Sticking
 * to lwIP's offset means VFS-aware code in IDF (which special-cases sockets
 * by FD range) doesn't get confused.
 */

typedef enum { FD_FREE = 0, FD_TCP, FD_UDP, FD_TCP_LISTEN } fd_kind_t;

typedef struct fd_entry {
    fd_kind_t       kind;
    bool            nonblocking;
    bool            connected;        /* TCP only */
    bool            loopback;         /* UDP bound to 127.0.0.0/8 */
    QueueHandle_t   lo_rx;            /* loopback recv queue (of lo_msg_t*) */
    uint32_t        rcv_timeout_ms;
    uint32_t        snd_timeout_ms;

    /* The active smoltcp socket (for TCP_LISTEN this is the currently
     * accepted connection; the listener pool lives in `pool`). */
    net_sock_t      sock;

    /* For TCP_LISTEN: a pool of pre-listening sockets on `port` so we can
     * accept multiple connections without races. NULL if not a listener. */
    net_sock_t     *pool;
    int             pool_len;
    int             pool_next;        /* index to drain next */
    uint16_t        port;

    /* Local bind state (TCP pre-listen, UDP). */
    uint32_t        local_ipv4_be;    /* 0 = INADDR_ANY */
    uint16_t        local_port;
    net_iface_id_t  iface;

    /* For select() wakeup: bit set when this FD changes state. */
    EventGroupHandle_t evt;
    /* Event bits */
    /*  bit 0 = readable, bit 1 = writable, bit 2 = error                */
} fd_entry_t;

esp_err_t   fd_table_init(void);

/* Allocate a free entry; returns the FD (>= LWIP_SOCKET_OFFSET) or -1. */
int         fd_alloc(fd_kind_t kind);

/* Look up an entry. Returns NULL if FD is out of range or unallocated. */
fd_entry_t *fd_get(int fd);

/* Free an entry (closes any owned smoltcp socket). */
void        fd_free(int fd);

/* True iff fd is a BSD socket FD owned by this shim. */
bool        fd_is_socket(int fd);

/* The base FD offset (matches CONFIG_LWIP_SOCKET_OFFSET so VFS plays nice). */
int         fd_offset(void);

/* Choose a default iface for unbound outbound sockets. */
net_iface_id_t fd_default_iface(void);

#ifdef __cplusplus
}
#endif
