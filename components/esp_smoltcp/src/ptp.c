#include <string.h>
#include "esp_log.h"
#include "esp_eth.h"
#include "freertos/FreeRTOS.h"

#include "esp_smoltcp.h"
#include "esp_smoltcp_internal.h"

#if CONFIG_NETSTACK_PTP

static const char *TAG = "ptp";

#define ETHERTYPE_PTP 0x88F7

/*
 * Stub PTP entry point. The L2 tap delivers all PTP messages to us with
 * hardware RX timestamps from the EMAC. A real implementation needs:
 *
 *   - PTP message parser (Sync, Follow_Up, Delay_Req, Delay_Resp, ...)
 *   - State machine (ordinary clock, slave-only is the common case)
 *   - Servo (PI controller) driving esp_eth_ioctl(ETH_CMD_S_PTP_TIME, ...)
 *     to discipline the EMAC's PHC.
 *
 * Recommend using `linuxptp`'s message layout as a reference, or wrapping
 * the C reference impl from IEEE 1588.
 */
static void ptp_rx(net_iface_id_t iface, const uint8_t *frame, size_t len,
                   int64_t hw_ts_ns, void *user)
{
    (void)iface; (void)user;
    if (len < 14 + 34) return;  /* min PTP header */
    uint8_t msg_type = frame[14] & 0x0F;
    ESP_LOGD(TAG, "PTP msg type=%u len=%zu hw_ts=%lld", msg_type, len, (long long)hw_ts_ns);
    /* TODO: hand off to PTP state machine. */
}

esp_err_t ptp_start(net_iface_id_t iface)
{
    /* TODO: enable EMAC HW timestamping via esp_eth_ioctl(ETH_CMD_S_PTP_*). */
    return net_stack_l2_tap(ETHERTYPE_PTP, ptp_rx, NULL);
}

#endif /* CONFIG_NETSTACK_PTP */
