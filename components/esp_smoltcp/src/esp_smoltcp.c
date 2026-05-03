#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_attr.h"           /* IRAM_ATTR */
#include "nvs_flash.h"

#include "esp_smoltcp.h"
#include "esp_smoltcp_internal.h"
#include "smoltcp_glue.h"

static const char *TAG = "net_stack";

#define IP_UP_BIT_(i) (1u << (i))

static net_iface_t       s_ifaces[NET_IFACE_MAX];
static EventGroupHandle_t s_evt;
static TaskHandle_t       s_task;
static net_event_cb_t     s_user_cb;
static void              *s_user_cb_arg;

SemaphoreHandle_t        g_smoltcp_lock;
static EventGroupHandle_t s_progress_evt;
#define PROGRESS_BIT 0x01

/* ---- frame slab pool ----------------------------------------------------
 * Fixed pool of N frames preallocated at startup, recycled via a free-list
 * queue. Eliminates heap_caps_malloc on the RX hot path (~5–10 K calls/s
 * under load) and gives bounded memory + bounded latency.
 */
#define POOL_SZ CONFIG_NETSTACK_FRAME_POOL_SIZE

static net_frame_t   s_frame_pool[POOL_SZ];
static QueueHandle_t s_frame_free_q;
static uint32_t      s_frame_alloc_fail;   /* counter for stats */

static void frame_pool_init(void)
{
    s_frame_free_q = xQueueCreate(POOL_SZ, sizeof(net_frame_t *));
    for (int i = 0; i < POOL_SZ; i++) {
        net_frame_t *p = &s_frame_pool[i];
        xQueueSend(s_frame_free_q, &p, 0);
    }
    ESP_LOGI(TAG, "frame pool: %d slots × %d B = %d KiB",
             POOL_SZ, (int)sizeof(net_frame_t),
             (POOL_SZ * (int)sizeof(net_frame_t)) / 1024);
}

IRAM_ATTR net_frame_t *net_frame_alloc(size_t len)
{
    if (!s_frame_free_q || len > NET_FRAME_BUF) {
        s_frame_alloc_fail++;
        return NULL;
    }
    net_frame_t *f = NULL;
    if (xQueueReceive(s_frame_free_q, &f, 0) != pdTRUE) {
        s_frame_alloc_fail++;
        return NULL;
    }
    f->len = len;
    f->hw_ts_ns = -1;
    return f;
}

IRAM_ATTR void net_frame_free(net_frame_t *f)
{
    if (!f || !s_frame_free_q) return;
    xQueueSend(s_frame_free_q, &f, 0);
}

uint32_t net_stack_frame_drops(void) { return s_frame_alloc_fail; }

/* ---- event delivery ---------------------------------------------------- */
static void emit_event(net_iface_id_t iface, net_event_t evt)
{
    if (s_user_cb) s_user_cb(iface, evt, s_user_cb_arg);
}

void net_stack_link_event(net_iface_id_t iface, bool up)
{
    if (iface >= NET_IFACE_MAX) return;
    s_ifaces[iface].link_up = up;
    if (up) s_ifaces[iface].link_ups++;
    else    s_ifaces[iface].link_downs++;
    if (g_smoltcp_lock) {
        SMOLTCP_LOCK();
        smoltcp_iface_set_link_up(s_ifaces[iface].smoltcp_handle, up);
        SMOLTCP_UNLOCK();
    }
    emit_event(iface, up ? NET_EVT_IFACE_UP : NET_EVT_IFACE_DOWN);
    if (s_task) xTaskNotifyGive(s_task);
}

IRAM_ATTR void net_stack_post_rx(net_iface_id_t iface, net_frame_t *f)
{
    if (iface >= NET_IFACE_MAX || !s_ifaces[iface].enabled) {
        net_frame_free(f);
        return;
    }
    s_ifaces[iface].rx_frames++;
    s_ifaces[iface].rx_bytes += f->len;
    if (xQueueSend(s_ifaces[iface].rx_queue, &f, 0) != pdTRUE) {
        s_ifaces[iface].rx_drops++;
        net_frame_free(f);
        return;
    }
    if (s_task) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

/* ---- L2 tap ------------------------------------------------------------ */
typedef struct {
    uint16_t        ethertype;     /* host order; 0 = unused slot */
    net_l2_tap_cb_t cb;
    void           *user;
} l2_tap_t;

#define L2_TAP_SLOTS 4
static l2_tap_t s_taps[L2_TAP_SLOTS];

esp_err_t net_stack_l2_tap(uint16_t ethertype, net_l2_tap_cb_t cb, void *user)
{
    /* If cb == NULL clear an existing slot for this ethertype. */
    for (int i = 0; i < L2_TAP_SLOTS; i++) {
        if (s_taps[i].ethertype == ethertype) {
            s_taps[i].cb = cb;
            s_taps[i].user = user;
            if (cb == NULL) s_taps[i].ethertype = 0;
            return ESP_OK;
        }
    }
    if (cb == NULL) return ESP_OK;
    for (int i = 0; i < L2_TAP_SLOTS; i++) {
        if (s_taps[i].ethertype == 0) {
            s_taps[i] = (l2_tap_t){ ethertype, cb, user };
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static bool dispatch_tap(net_iface_id_t iface, net_frame_t *f)
{
    if (f->len < 14) return false;
    uint16_t et = ((uint16_t)f->data[12] << 8) | f->data[13];
    for (int i = 0; i < L2_TAP_SLOTS; i++) {
        if (s_taps[i].ethertype == et && s_taps[i].cb) {
            s_taps[i].cb(iface, f->data, f->len, f->hw_ts_ns, s_taps[i].user);
            return true;
        }
    }
    return false;
}

/* ---- TX path called by smoltcp core ----------------------------------- */
/* Called from Rust via FFI. Returns 0 on success, -1 if the L2 driver
 * could not enqueue the frame (Rust treats this as TX exhaustion). */
IRAM_ATTR int smoltcp_glue_tx_cb(uint32_t iface_id, const uint8_t *frame, size_t len)
{
    if (iface_id >= NET_IFACE_MAX) return -1;
    net_iface_t *i = &s_ifaces[iface_id];
    if (!i->enabled || !i->tx) return -1;
    esp_err_t r = i->tx(i, frame, len);
    if (r == ESP_OK) {
        i->tx_frames++;
        i->tx_bytes += len;
        return 0;
    }
    i->tx_fails++;
    return -1;
}

/* Time / RNG callbacks for the Rust core. now_us is called every poll
 * cycle; keep it in IRAM. */
IRAM_ATTR int64_t smoltcp_glue_now_us(void) { return esp_timer_get_time(); }
uint32_t smoltcp_glue_rand32(void) { return esp_random(); }

/* ---- main task -------------------------------------------------------- */
net_iface_t *net_iface_get(net_iface_id_t id)
{
    if (id >= NET_IFACE_MAX || !s_ifaces[id].enabled) return NULL;
    return &s_ifaces[id];
}

static void poll_iface(net_iface_t *i)
{
    /* Drain RX queue into smoltcp; tap consumes any frame matching a
     * registered EtherType before smoltcp ever sees it. */
    net_frame_t *f;
    while (xQueueReceive(i->rx_queue, &f, 0) == pdTRUE) {
        if (!dispatch_tap(i->id, f)) {
            SMOLTCP_LOCK();
            smoltcp_iface_rx(i->smoltcp_handle, f->data, f->len);
            SMOLTCP_UNLOCK();
        }
        net_frame_free(f);
    }

    /* Run smoltcp: it may invoke smoltcp_glue_tx_cb to push frames out. */
    SMOLTCP_LOCK();
    smoltcp_iface_poll(i->smoltcp_handle);
    bool ip_up_now = smoltcp_iface_has_ip(i->smoltcp_handle);
    SMOLTCP_UNLOCK();

    /* Signal any send/recv waiters that smoltcp's state may have
     * advanced — TX buffer drained, new RX bytes, sockets transitioned.
     * Cheaper than ms-scale vTaskDelay polling on the caller side. */
    xEventGroupSetBits(s_progress_evt, PROGRESS_BIT);

    /* Surface IP-state changes to the app. */
    bool was_up = i->ip_up;
    i->ip_up = ip_up_now;
    if (i->ip_up != was_up) {
        emit_event(i->id, i->ip_up ? NET_EVT_GOT_IP : NET_EVT_LOST_IP);
        if (i->ip_up) {
            xEventGroupSetBits(s_evt, IP_UP_BIT_(i->id));
        } else {
            xEventGroupClearBits(s_evt, IP_UP_BIT_(i->id));
        }
    }
}

static void net_stack_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "net stack task running");

    for (;;) {
        for (int i = 0; i < NET_IFACE_MAX; i++) {
            if (s_ifaces[i].enabled) poll_iface(&s_ifaces[i]);
        }

        /* Block until either an RX/link event wakes us, or the soonest
         * smoltcp deadline elapses (DHCP/TCP retx etc.). */
        int64_t next_us = INT64_MAX;
        SMOLTCP_LOCK();
        for (int i = 0; i < NET_IFACE_MAX; i++) {
            if (!s_ifaces[i].enabled) continue;
            int64_t d = smoltcp_iface_poll_delay_us(s_ifaces[i].smoltcp_handle);
            if (d >= 0 && d < next_us) next_us = d;
        }
        SMOLTCP_UNLOCK();
        TickType_t wait = (next_us == INT64_MAX)
                          ? portMAX_DELAY
                          : pdMS_TO_TICKS((next_us + 999) / 1000);
        ulTaskNotifyTake(pdTRUE, wait ? wait : 1);
    }
}

/* ---- public API -------------------------------------------------------- */
esp_err_t net_stack_register_event_cb(net_event_cb_t cb, void *user)
{
    s_user_cb = cb;
    s_user_cb_arg = user;
    return ESP_OK;
}

void net_stack_kick_poll(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void net_stack_wait_progress(uint32_t timeout_ms)
{
    if (!s_progress_evt) {
        vTaskDelay(1);
        return;
    }
    /* Clear-on-exit so each wait represents one full poll cycle since
     * the wait started. Wait-for-any (xWaitForAllBits=pdFALSE). */
    xEventGroupWaitBits(s_progress_evt, PROGRESS_BIT,
                        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
}

uint32_t net_stack_get_ipv4(net_iface_id_t iface)
{
    if (iface >= NET_IFACE_MAX || !s_ifaces[iface].enabled) return 0;
    SMOLTCP_LOCK();
    uint32_t v = smoltcp_iface_get_ipv4(s_ifaces[iface].smoltcp_handle);
    SMOLTCP_UNLOCK();
    return v;
}

uint32_t net_stack_get_gateway(net_iface_id_t iface)
{
    if (iface >= NET_IFACE_MAX || !s_ifaces[iface].enabled) return 0;
    SMOLTCP_LOCK();
    uint32_t v = smoltcp_iface_get_gw(s_ifaces[iface].smoltcp_handle);
    SMOLTCP_UNLOCK();
    return v;
}

uint32_t net_stack_get_netmask(net_iface_id_t iface)
{
    if (iface >= NET_IFACE_MAX || !s_ifaces[iface].enabled) return 0;
    SMOLTCP_LOCK();
    uint32_t v = smoltcp_iface_get_netmask(s_ifaces[iface].smoltcp_handle);
    SMOLTCP_UNLOCK();
    return v;
}

esp_err_t esp_smoltcp_get_ipv6_link_local(esp_smoltcp_iface_t iface, uint8_t out[16])
{
    if (iface >= NET_IFACE_MAX || !s_ifaces[iface].enabled || !out) {
        return ESP_ERR_NOT_FOUND;
    }
    SMOLTCP_LOCK();
    int ok = smoltcp_iface_get_ipv6(s_ifaces[iface].smoltcp_handle, out);
    SMOLTCP_UNLOCK();
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t net_stack_l2_send(net_iface_id_t iface, const uint8_t *frame, size_t len)
{
    if (iface >= NET_IFACE_MAX) return ESP_ERR_INVALID_ARG;
    net_iface_t *i = &s_ifaces[iface];
    if (!i->enabled || !i->tx) return ESP_ERR_INVALID_STATE;
    return i->tx(i, frame, len);
}

esp_err_t net_stack_get_stats(net_iface_id_t iface, net_iface_stats_t *out)
{
    if (iface >= NET_IFACE_MAX || !out) return ESP_ERR_INVALID_ARG;
    net_iface_t *i = &s_ifaces[iface];
    out->rx_frames  = i->rx_frames;
    out->rx_bytes   = i->rx_bytes;
    out->rx_drops   = i->rx_drops;
    out->tx_frames  = i->tx_frames;
    out->tx_bytes   = i->tx_bytes;
    out->tx_fails   = i->tx_fails;
    out->link_ups   = i->link_ups;
    out->link_downs = i->link_downs;
    if (i->enabled && i->smoltcp_handle) {
        SMOLTCP_LOCK();
        out->active_sockets = smoltcp_iface_socket_count(i->smoltcp_handle);
        SMOLTCP_UNLOCK();
    } else {
        out->active_sockets = 0;
    }
    return ESP_OK;
}

void *net_stack_smoltcp_handle(net_iface_id_t iface)
{
    if (iface >= NET_IFACE_MAX || !s_ifaces[iface].enabled) return NULL;
    return s_ifaces[iface].smoltcp_handle;
}

esp_err_t esp_smoltcp_wait_for_ip(esp_smoltcp_iface_t iface, uint32_t timeout_ms)
{
    if (iface >= NET_IFACE_MAX) return ESP_ERR_INVALID_ARG;
    EventBits_t bit = IP_UP_BIT_(iface);
    EventBits_t got = xEventGroupWaitBits(s_evt, bit, pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(timeout_ms));
    return (got & bit) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t net_stack_init(void)
{
    static bool inited;
    if (inited) return ESP_OK;
    inited = true;

    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "nvs init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    s_evt = xEventGroupCreate();
    s_progress_evt = xEventGroupCreate();
    if (!s_evt || !s_progress_evt) return ESP_ERR_NO_MEM;
    g_smoltcp_lock = xSemaphoreCreateMutex();
    if (!g_smoltcp_lock) return ESP_ERR_NO_MEM;
    frame_pool_init();

    /* Rust core init — sets up the global allocator and TX scratch pool. */
    smoltcp_core_init();

    for (int i = 0; i < NET_IFACE_MAX; i++) {
        s_ifaces[i].id = (net_iface_id_t)i;
        s_ifaces[i].rx_queue = xQueueCreate(CONFIG_NETSTACK_RX_QUEUE_DEPTH,
                                            sizeof(net_frame_t *));
    }

    /*
     * Spawn the poll task BEFORE the app attaches any L2 driver. The
     * eth driver / esp_hosted may post link or RX events the moment we
     * register their callbacks — s_task must already exist or FreeRTOS
     * asserts in xTaskNotifyGive.
     */
    BaseType_t ok = xTaskCreatePinnedToCore(
        net_stack_task, "esp_smoltcp", CONFIG_NETSTACK_TASK_STACK,
        NULL, CONFIG_NETSTACK_TASK_PRIORITY, &s_task,
        CONFIG_NETSTACK_TASK_CORE < 0 ? tskNO_AFFINITY : CONFIG_NETSTACK_TASK_CORE);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "esp_smoltcp ready (no L2 attached yet — call esp_smoltcp_attach_eth)");
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/* Ethernet attach: app brings its own esp_eth_handle_t (driver already
 * installed, possibly already started). We register our RX hook + event
 * handler, allocate the smoltcp iface, and (re)start the eth driver. */

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_check.h"
#include "esp_attr.h"

static esp_eth_handle_t s_eth_handle;

static IRAM_ATTR esp_err_t eth_input_cb(esp_eth_handle_t h, uint8_t *buf, uint32_t len, void *priv)
{
    (void)h; (void)priv;
    net_frame_t *f = net_frame_alloc(len);
    if (!f) { free(buf); return ESP_ERR_NO_MEM; }
    memcpy(f->data, buf, len);
    free(buf);
    net_stack_post_rx(NET_IFACE_ETH, f);
    return ESP_OK;
}

static IRAM_ATTR esp_err_t eth_tx(net_iface_t *iface, const uint8_t *frame, size_t len)
{
    (void)iface;
    return esp_eth_transmit(s_eth_handle, (void *)frame, len);
}

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ETH link up");
        net_stack_link_event(NET_IFACE_ETH, true);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "ETH link down");
        net_stack_link_event(NET_IFACE_ETH, false);
        break;
    default: break;
    }
}

esp_err_t esp_smoltcp_attach_eth(esp_eth_handle_t eth_handle)
{
    if (!eth_handle) return ESP_ERR_INVALID_ARG;
    if (s_ifaces[NET_IFACE_ETH].enabled) return ESP_ERR_INVALID_STATE;

    s_eth_handle = eth_handle;

    net_iface_t *iface = &s_ifaces[NET_IFACE_ETH];
    iface->enabled = true;
    iface->tx = eth_tx;

    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, iface->mac),
                        TAG, "get MAC");
    ESP_RETURN_ON_ERROR(esp_eth_update_input_path(s_eth_handle, eth_input_cb, NULL),
                        TAG, "set RX cb");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   eth_event, NULL),
                        TAG, "ETH event handler");

    iface->smoltcp_handle = smoltcp_iface_new((uint32_t)NET_IFACE_ETH, iface->mac);

    /* Start (or re-start) the eth driver so RX flows. Safe even if the
     * app already started it — esp_eth_start is idempotent on running. */
    esp_eth_start(s_eth_handle);

    /* Start DHCPv4 by default (Kconfig-controlled). */
#if CONFIG_NETSTACK_DHCPV4_CLIENT
    SMOLTCP_LOCK();
    smoltcp_iface_enable_dhcp(iface->smoltcp_handle);
    SMOLTCP_UNLOCK();
    ESP_LOGI(TAG, "ETH attached, MAC %02x:%02x:%02x:%02x:%02x:%02x, DHCPv4 client started",
             iface->mac[0], iface->mac[1], iface->mac[2],
             iface->mac[3], iface->mac[4], iface->mac[5]);
#else
    uint32_t ip_be = 0, gw_be = 0;
    smoltcp_parse_ipv4(CONFIG_NETSTACK_STATIC_IP, &ip_be);
    smoltcp_parse_ipv4(CONFIG_NETSTACK_STATIC_GW, &gw_be);
    SMOLTCP_LOCK();
    smoltcp_iface_set_static(iface->smoltcp_handle, ip_be, 24, gw_be);
    SMOLTCP_UNLOCK();
    ESP_LOGI(TAG, "ETH attached, static IP %s", CONFIG_NETSTACK_STATIC_IP);
#endif

#if CONFIG_NETSTACK_PTP
    extern esp_err_t ptp_start(net_iface_id_t iface);
    ptp_start(NET_IFACE_ETH);
#endif

    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}
