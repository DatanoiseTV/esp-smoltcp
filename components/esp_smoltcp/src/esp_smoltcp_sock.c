#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "esp_smoltcp.h"
#include "esp_smoltcp_socket.h"
#include "esp_smoltcp_internal.h"
#include "smoltcp_glue.h"

/*
 * Thin wrapper that serializes app-thread socket calls onto the smoltcp
 * core (which is not internally thread-safe). All paths share the global
 * SMOLTCP_LOCK defined in net_stack_internal.h, which is also held by
 * the poll task during smoltcp_iface_poll. Without that, two threads can
 * race a state transition (e.g. SYN-RECV -> ESTABLISHED) and accept()
 * never sees it.
 */

#define LOCK()   SMOLTCP_LOCK()
#define UNLOCK() SMOLTCP_UNLOCK()

static smoltcp_iface_t handle_for(net_iface_id_t id)
{
    net_iface_t *i = net_iface_get(id);
    return i ? i->smoltcp_handle : NULL;
}

/* ---- TCP -------------------------------------------------------------- */
net_sock_t net_tcp_open(net_iface_id_t iface)
{
    net_sock_t r = NET_SOCK_INVALID;
    smoltcp_iface_t h = handle_for(iface);
    if (!h) return r;
    LOCK();
    uint32_t id = smoltcp_tcp_open(h);
    UNLOCK();
    /* Encode iface in the upper 8 bits so close/send know which iface. */
    if (id) r.id = ((uint32_t)iface << 24) | (id & 0x00FFFFFF);
    return r;
}

#define IFACE(s)  ((net_iface_id_t)(((s).id >> 24) & 0xFF))
#define LOCAL(s)  ((s).id & 0x00FFFFFF)

esp_err_t net_tcp_connect(net_sock_t s, uint32_t ipv4_be, uint16_t port, uint32_t timeout_ms)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return ESP_ERR_INVALID_ARG;
    LOCK();
    int rc = smoltcp_tcp_connect(h, LOCAL(s), ipv4_be, port, 0);
    UNLOCK();
    if (rc != 0) return ESP_FAIL;

    /* Poll-wait for the handshake. Replace with a proper notification
     * once the net_stack task supports per-socket waiters. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        LOCK();
        bool active = smoltcp_tcp_is_active(h, LOCAL(s));
        UNLOCK();
        if (active) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t net_tcp_listen(net_sock_t s, uint16_t port)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return ESP_ERR_INVALID_ARG;
    LOCK();
    int rc = smoltcp_tcp_listen(h, LOCAL(s), port);
    UNLOCK();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

int net_tcp_send(net_sock_t s, const void *buf, size_t len, uint32_t timeout_ms)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return -1;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t total = 0;
    while (total < len) {
        LOCK();
        int n = smoltcp_tcp_send(h, LOCAL(s), (const uint8_t *)buf + total, len - total);
        UNLOCK();
        if (n > 0) {
            total += n;
            net_stack_kick_poll();   /* push the bytes out NOW */
            continue;
        }
        if (xTaskGetTickCount() >= deadline) break;
        /* Buffer full — kick the poll task and wait for the next poll
         * cycle to free space (event-driven, ~µs latency vs the 1 ms
         * tick of vTaskDelay). Cap the wait so a stuck stack still
         * times out. */
        net_stack_kick_poll();
        net_stack_wait_progress(50);
    }
    return total ? (int)total : -1;
}

int net_tcp_recv(net_sock_t s, void *buf, size_t cap, uint32_t timeout_ms)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return -1;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() <= deadline) {
        LOCK();
        int n = smoltcp_tcp_recv(h, LOCAL(s), buf, cap);
        UNLOCK();
        if (n > 0) {
            net_stack_kick_poll();   /* may need to send a window update */
            return n;
        }
        net_stack_wait_progress(50);
    }
    return 0;
}

esp_err_t net_tcp_close(net_sock_t s)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return ESP_ERR_INVALID_ARG;
    LOCK();
    smoltcp_tcp_close(h, LOCAL(s));
    UNLOCK();
    return ESP_OK;
}

bool net_tcp_is_active(net_sock_t s)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return false;
    LOCK();
    bool a = smoltcp_tcp_is_active(h, LOCAL(s));
    UNLOCK();
    return a;
}

bool net_tcp_is_connected(net_sock_t s)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return false;
    LOCK();
    bool c = smoltcp_tcp_is_connected(h, LOCAL(s));
    UNLOCK();
    return c;
}

size_t net_tcp_recv_queue(net_sock_t s)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return 0;
    LOCK();
    size_t n = smoltcp_tcp_recv_queue(h, LOCAL(s));
    UNLOCK();
    return n;
}

size_t net_tcp_send_capacity(net_sock_t s)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return 0;
    LOCK();
    size_t n = smoltcp_tcp_send_capacity(h, LOCAL(s));
    UNLOCK();
    return n;
}

/* ---- UDP -------------------------------------------------------------- */
net_sock_t net_udp_open(net_iface_id_t iface, uint16_t local_port)
{
    net_sock_t r = NET_SOCK_INVALID;
    smoltcp_iface_t h = handle_for(iface);
    if (!h) return r;
    LOCK();
    uint32_t id = smoltcp_udp_open(h, local_port);
    UNLOCK();
    if (id) r.id = ((uint32_t)iface << 24) | (id & 0x00FFFFFF);
    return r;
}

int net_udp_sendto(net_sock_t s, const void *buf, size_t len,
                   uint32_t dst_be, uint16_t dst_port)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return -1;
    LOCK();
    int n = smoltcp_udp_sendto(h, LOCAL(s), buf, len, dst_be, dst_port);
    UNLOCK();
    if (n > 0) net_stack_kick_poll();
    return n;
}

int net_udp_recvfrom(net_sock_t s, void *buf, size_t cap,
                     uint32_t *src_be, uint16_t *src_port, uint32_t timeout_ms)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return -1;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() <= deadline) {
        LOCK();
        int n = smoltcp_udp_recvfrom(h, LOCAL(s), buf, cap, src_be, src_port);
        UNLOCK();
        if (n > 0) return n;
        net_stack_wait_progress(50);
    }
    return 0;
}

esp_err_t net_udp_close(net_sock_t s)
{
    smoltcp_iface_t h = handle_for(IFACE(s));
    if (!h) return ESP_ERR_INVALID_ARG;
    LOCK();
    smoltcp_udp_close(h, LOCAL(s));
    UNLOCK();
    return ESP_OK;
}

/* ---- multicast -------------------------------------------------------- */
esp_err_t net_mcast_join(net_iface_id_t iface, uint32_t group_be)
{
    smoltcp_iface_t h = handle_for(iface);
    if (!h) return ESP_ERR_INVALID_ARG;
    LOCK();
    int rc = smoltcp_iface_mcast_join(h, group_be);
    UNLOCK();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t net_mcast_leave(net_iface_id_t iface, uint32_t group_be)
{
    smoltcp_iface_t h = handle_for(iface);
    if (!h) return ESP_ERR_INVALID_ARG;
    LOCK();
    int rc = smoltcp_iface_mcast_leave(h, group_be);
    UNLOCK();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
