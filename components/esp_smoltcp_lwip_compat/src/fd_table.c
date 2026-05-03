#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"   /* LWIP_SOCKET_OFFSET, etc. */

#include "fd_table.h"
#include "loopback.h"

static const char *TAG = "fd_table";

#define MAX_FDS CONFIG_LWIP_COMPAT_MAX_FDS

static fd_entry_t       s_table[MAX_FDS];
static SemaphoreHandle_t s_lock;

esp_err_t fd_table_init(void)
{
    if (s_lock) return ESP_OK;        /* already initialized */
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    for (int i = 0; i < MAX_FDS; i++) {
        s_table[i].evt = xEventGroupCreate();
    }
    ESP_LOGI(TAG, "fd table ready (max=%d, offset=%d)", MAX_FDS, fd_offset());
    return ESP_OK;
}

/*
 * Auto-init the fd table at C-startup so any of our wrap functions can
 * be called without a separate explicit init from app code. Constructor
 * priority 101 runs before main but after FreeRTOS heap is up.
 */
__attribute__((constructor(101)))
static void fd_table_auto_init(void)
{
    fd_table_init();
}

int fd_offset(void)
{
#ifdef LWIP_SOCKET_OFFSET
    return LWIP_SOCKET_OFFSET;
#else
    return 0;
#endif
}

bool fd_is_socket(int fd)
{
    int i = fd - fd_offset();
    return i >= 0 && i < MAX_FDS && s_table[i].kind != FD_FREE;
}

int fd_alloc(fd_kind_t kind)
{
    if (!s_lock) fd_table_init();   /* belt-and-suspenders: lazy init */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int found = -1;
    for (int i = 0; i < MAX_FDS; i++) {
        if (s_table[i].kind == FD_FREE) {
            memset(&s_table[i], 0, offsetof(fd_entry_t, evt));
            s_table[i].kind = kind;
            s_table[i].sock = NET_SOCK_INVALID;
            s_table[i].iface = fd_default_iface();
            xEventGroupClearBits(s_table[i].evt, 0xFF);
            found = i;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found < 0 ? -1 : (fd_offset() + found);
}

fd_entry_t *fd_get(int fd)
{
    int i = fd - fd_offset();
    if (i < 0 || i >= MAX_FDS) return NULL;
    if (s_table[i].kind == FD_FREE) return NULL;
    return &s_table[i];
}

void fd_free(int fd)
{
    fd_entry_t *e = fd_get(fd);
    if (!e) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (e->loopback) {
        lo_unbind(e->local_port, e);
        if (e->lo_rx) { vQueueDelete(e->lo_rx); e->lo_rx = NULL; }
        e->loopback = false;
    }
    if (net_sock_valid(e->sock)) {
        if (e->kind == FD_UDP) net_udp_close(e->sock);
        else                    net_tcp_close(e->sock);
    }
    if (e->pool) {
        for (int i = 0; i < e->pool_len; i++) {
            if (net_sock_valid(e->pool[i])) net_tcp_close(e->pool[i]);
        }
        free(e->pool);
    }
    e->kind = FD_FREE;
    xSemaphoreGive(s_lock);
}

net_iface_id_t fd_default_iface(void)
{
#if CONFIG_LWIP_COMPAT_DEFAULT_IFACE_ETH
    if (net_stack_get_ipv4(NET_IFACE_ETH)) return NET_IFACE_ETH;
    if (net_stack_get_ipv4(NET_IFACE_WIFI)) return NET_IFACE_WIFI;
    return NET_IFACE_ETH;
#else
    if (net_stack_get_ipv4(NET_IFACE_WIFI)) return NET_IFACE_WIFI;
    if (net_stack_get_ipv4(NET_IFACE_ETH)) return NET_IFACE_ETH;
    return NET_IFACE_WIFI;
#endif
}
