#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_smoltcp.h"
#include "esp_smoltcp_socket.h"
#include "esp_smoltcp_internal.h"
#include "smoltcp_glue.h"

#if CONFIG_NETSTACK_NTP_CLIENT

static const char *TAG = "ntp";

/* SNTPv4 packet — RFC 4330. */
typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint64_t ref_ts;
    uint64_t orig_ts;
    uint64_t recv_ts;
    uint64_t tx_ts;
} sntp_pkt_t;

#define NTP_UNIX_OFFSET 2208988800ULL  /* seconds 1900 -> 1970 */

static void ntp_task(void *arg)
{
    net_iface_id_t iface = (net_iface_id_t)(intptr_t)arg;

    /* TODO: replace with proper DNS resolution once DNS client is wired. */
    /* For now this hits a hardcoded IPv4 — pool.ntp.org typically resolves
     * to an A record; the user should set CONFIG_NETSTACK_NTP_SERVER to a
     * dotted-quad until DNS is in. */
    uint32_t server_be = 0;
    if (smoltcp_parse_ipv4(CONFIG_NETSTACK_NTP_SERVER, &server_be) != 0) {
        ESP_LOGE(TAG, "NTP server '%s' must be a dotted-quad until DNS lands",
                 CONFIG_NETSTACK_NTP_SERVER);
        vTaskDelete(NULL);
        return;
    }

    net_sock_t s = net_udp_open(iface, 0);
    if (!net_sock_valid(s)) {
        ESP_LOGE(TAG, "udp open failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        sntp_pkt_t req = {0};
        req.li_vn_mode = (0 << 6) | (4 << 3) | 3;  /* LI=0, VN=4, mode=client */

        net_udp_sendto(s, &req, sizeof(req), server_be, 123);

        sntp_pkt_t rsp;
        uint32_t src_ip; uint16_t src_port;
        int n = net_udp_recvfrom(s, &rsp, sizeof(rsp), &src_ip, &src_port, 5000);
        if (n >= (int)sizeof(rsp)) {
            uint32_t secs = __builtin_bswap32((uint32_t)(rsp.tx_ts & 0xFFFFFFFF));
            uint64_t unix_secs = secs - NTP_UNIX_OFFSET;
            struct timeval tv = { .tv_sec = (time_t)unix_secs, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "synced: unix=%llu", (unsigned long long)unix_secs);
            /* TODO: emit NET_EVT_NTP_SYNCED via net_stack event cb. */
        } else {
            ESP_LOGW(TAG, "NTP timeout/short reply (%d)", n);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_NETSTACK_NTP_POLL_SECONDS * 1000));
    }
}

esp_err_t ntp_start(net_iface_id_t iface)
{
    return xTaskCreate(ntp_task, "ntp", 4096, (void *)(intptr_t)iface,
                       4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#endif /* CONFIG_NETSTACK_NTP_CLIENT */
