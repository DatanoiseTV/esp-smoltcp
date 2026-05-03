#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_smoltcp.h"
#include "smoltcp_glue.h"

/*
 * Single mutex serializing every smoltcp call. The Rust core is not
 * thread-safe by design, so the poll task and the socket-API wrappers
 * (net_sock.c) and the link/event callbacks all take this before
 * touching smoltcp_* functions.
 */
extern SemaphoreHandle_t g_smoltcp_lock;
#define SMOLTCP_LOCK()   xSemaphoreTake(g_smoltcp_lock, portMAX_DELAY)
#define SMOLTCP_UNLOCK() xSemaphoreGive(g_smoltcp_lock)

/* Wake the poll task immediately — call after any socket-state change
 * that should trigger TX (data queued for send) or that we want acked
 * promptly. Avoids waiting for the next poll deadline. */
void net_stack_kick_poll(void);

/* Block until the poll task has made forward progress (one full
 * smoltcp_iface_poll cycle), or until `timeout_ms` elapses. Used by
 * net_tcp_send/recv to wake on real events instead of polling at 1 kHz
 * via vTaskDelay(1). */
void net_stack_wait_progress(uint32_t timeout_ms);

/*
 * Internal: glue between L2 drivers (eth_link, wifi_link), the smoltcp
 * Rust core (smoltcp_glue), and the net_stack task.
 *
 * Each interface owns:
 *   - A frame ringbuffer (RX) populated by the L2 driver in IRQ/driver task.
 *   - A TX function pointer the smoltcp core invokes to enqueue frames.
 *   - A handle (smoltcp_iface_t) into the Rust core.
 */

typedef struct net_iface_s net_iface_t;

typedef esp_err_t (*l2_tx_fn)(net_iface_t *iface, const uint8_t *frame, size_t len);
typedef void      (*l2_link_state_fn)(net_iface_t *iface, bool up);

struct net_iface_s {
    net_iface_id_t      id;
    bool                enabled;
    bool                link_up;
    bool                ip_up;
    uint8_t             mac[6];

    QueueHandle_t       rx_queue;       /* of net_frame_t* */
    l2_tx_fn            tx;
    void               *l2_ctx;         /* driver-specific (handle, etc.) */

    smoltcp_iface_t     smoltcp_handle; /* opaque, owned by Rust core */

    /* Counters — bumped from the L2 driver hot path. */
    uint32_t            rx_frames;
    uint64_t            rx_bytes;
    uint32_t            rx_drops;
    uint32_t            tx_frames;
    uint64_t            tx_bytes;
    uint32_t            tx_fails;
    uint32_t            link_ups;
    uint32_t            link_downs;
};

/* Max ETH frame including 14-byte L2 header + max payload. Frames are
 * fixed-size so the slab pool can hand them out without any heap call.
 * Slightly oversized to cover VLAN-tagged frames cleanly. */
#define NET_FRAME_BUF 1536

typedef struct {
    size_t   len;
    int64_t  hw_ts_ns;     /* -1 if not timestamped */
    uint8_t  data[NET_FRAME_BUF];
} net_frame_t;

/* Allocate from the net_stack frame pool. NULL on exhaustion. */
net_frame_t *net_frame_alloc(size_t len);
void         net_frame_free(net_frame_t *f);

/* Called by L2 drivers when a frame arrives. Takes ownership of `f`. */
void         net_stack_post_rx(net_iface_id_t iface, net_frame_t *f);

/* Notify the net_stack task that link state changed. */
void         net_stack_link_event(net_iface_id_t iface, bool up);

/* Look up the iface struct (NULL if not enabled). */
net_iface_t *net_iface_get(net_iface_id_t id);

/* Driver init entry points (no-op if mode disabled). */
esp_err_t    eth_link_init(net_iface_t *iface);
esp_err_t    wifi_link_init(net_iface_t *iface);

/* Board-specific init hook (PHY reset sequencing, co-proc reset, etc.). */
esp_err_t    board_init_pre_net(void);
esp_err_t    board_init_post_net(void);

/* NTP / PTP entry points (compile out when not configured). */
#if CONFIG_NETSTACK_NTP_CLIENT
esp_err_t    ntp_start(net_iface_id_t iface);
#endif

#if CONFIG_NETSTACK_PTP
esp_err_t    ptp_start(net_iface_id_t iface);
#endif
