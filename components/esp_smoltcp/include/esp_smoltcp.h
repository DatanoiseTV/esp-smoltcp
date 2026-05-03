#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * esp_smoltcp — Rust smoltcp networking stack as an ESP-IDF component.
 *
 * Lifecycle:
 *
 *   1. Initialize the framework — creates the poll task, slab pool, lock.
 *
 *        esp_smoltcp_init();
 *
 *   2. Bring up your L2 driver yourself (esp_eth_driver_install + start,
 *      or ESP-Hosted), then attach it:
 *
 *        esp_smoltcp_attach_eth(eth_handle);
 *
 *   3. Optionally wait for an IP:
 *
 *        esp_smoltcp_wait_for_ip(ESP_SMOLTCP_IFACE_ETH, 10000);
 *
 * Once attached + with link up, BSD sockets just work — esp_http_server,
 * esp-tls, esp-mqtt, etc. all see smoltcp underneath via the linker
 * --wrap shim in `esp_smoltcp_lwip_compat`.
 *
 * For raw smoltcp-native sockets (lower overhead than BSD), see
 * <esp_smoltcp_socket.h>.
 */

typedef enum {
    ESP_SMOLTCP_IFACE_ETH  = 0,
    ESP_SMOLTCP_IFACE_WIFI = 1,
    ESP_SMOLTCP_IFACE_MAX  = 2,
} esp_smoltcp_iface_t;

typedef enum {
    ESP_SMOLTCP_EVT_IFACE_UP,
    ESP_SMOLTCP_EVT_IFACE_DOWN,
    ESP_SMOLTCP_EVT_GOT_IP,
    ESP_SMOLTCP_EVT_LOST_IP,
    ESP_SMOLTCP_EVT_NTP_SYNCED,
} esp_smoltcp_event_t;

typedef void (*esp_smoltcp_event_cb_t)(esp_smoltcp_iface_t iface,
                                       esp_smoltcp_event_t evt,
                                       void *user);

/* ---- lifecycle -------------------------------------------------------- */

/* Initialize the poll task, slab pool, mutex, and event group. Idempotent. */
esp_err_t esp_smoltcp_init(void);

/* Attach an already-installed esp_eth driver to interface
 * ESP_SMOLTCP_IFACE_ETH. The eth driver must NOT have an input callback
 * registered — esp_smoltcp installs its own. After this returns the
 * eth driver is started; you can wait for IP next.
 *
 * Typical use:
 *   esp_eth_handle_t h;
 *   esp_eth_driver_install(&cfg, &h);  // app picks PHY, pins, etc.
 *   esp_smoltcp_init();
 *   esp_smoltcp_attach_eth(h);
 */
esp_err_t esp_smoltcp_attach_eth(esp_eth_handle_t eth_handle);

/* Optional: register a callback for high-level events. Single global
 * callback; replace by calling again with a different cb. */
esp_err_t esp_smoltcp_register_event_cb(esp_smoltcp_event_cb_t cb, void *user);

/* Wait until the given interface has an IPv4 address, or timeout. */
esp_err_t esp_smoltcp_wait_for_ip(esp_smoltcp_iface_t iface, uint32_t timeout_ms);

/* ---- IP query --------------------------------------------------------- */

/* All addresses are uint32_t with octets stored in network-byte-order
 * memory layout (matches struct in_addr::s_addr). Returns 0 if unset. */
uint32_t esp_smoltcp_get_ipv4(esp_smoltcp_iface_t iface);
uint32_t esp_smoltcp_get_gateway(esp_smoltcp_iface_t iface);
uint32_t esp_smoltcp_get_netmask(esp_smoltcp_iface_t iface);

/* Copy the IPv6 link-local address (auto-generated from the MAC at
 * attach time) into `out`. `out` must point to at least 16 bytes.
 * Returns ESP_OK if a v6 address is configured, ESP_ERR_NOT_FOUND if
 * the iface isn't up.
 *
 * Note: only the link-local is auto-configured. SLAAC for global v6
 * addresses and DHCPv6 are not implemented yet — track the v0.2
 * milestone for that work. */
esp_err_t esp_smoltcp_get_ipv6_link_local(esp_smoltcp_iface_t iface, uint8_t out[16]);

/* ---- runtime stats ---------------------------------------------------- */

typedef struct {
    uint32_t rx_frames;
    uint64_t rx_bytes;
    uint32_t rx_drops;
    uint32_t tx_frames;
    uint64_t tx_bytes;
    uint32_t tx_fails;
    uint32_t link_ups;
    uint32_t link_downs;
} esp_smoltcp_stats_t;

esp_err_t esp_smoltcp_get_stats(esp_smoltcp_iface_t iface, esp_smoltcp_stats_t *out);
uint32_t  esp_smoltcp_frame_pool_drops(void);

/* ---- L2 tap (PTP / LLDP / custom EtherTypes) -------------------------- */

typedef void (*esp_smoltcp_l2_tap_cb_t)(esp_smoltcp_iface_t iface,
                                        const uint8_t *frame, size_t len,
                                        int64_t hw_timestamp_ns,
                                        void *user);

/* Register a callback for a specific EtherType so frames bypass smoltcp.
 * `ethertype` is host byte order. Pass cb=NULL to clear. The callback
 * runs on the smoltcp poll task — keep it short. */
esp_err_t esp_smoltcp_l2_tap(uint16_t ethertype,
                             esp_smoltcp_l2_tap_cb_t cb, void *user);

/* Inject an L2 frame (must include the 14-byte Ethernet header). */
esp_err_t esp_smoltcp_l2_send(esp_smoltcp_iface_t iface,
                              const uint8_t *frame, size_t len);

/* ---- backwards-compat aliases ----------------------------------------- */
/* The original internal name was `net_stack`. Keep aliases for now so
 * existing app code builds against the renamed component unchanged.
 * New code should prefer the esp_smoltcp_* names. */
typedef esp_smoltcp_iface_t      net_iface_id_t;
typedef esp_smoltcp_event_t      net_event_t;
typedef esp_smoltcp_event_cb_t   net_event_cb_t;
typedef esp_smoltcp_l2_tap_cb_t  net_l2_tap_cb_t;
typedef esp_smoltcp_stats_t      net_iface_stats_t;
#define NET_IFACE_ETH       ESP_SMOLTCP_IFACE_ETH
#define NET_IFACE_WIFI      ESP_SMOLTCP_IFACE_WIFI
#define NET_IFACE_MAX       ESP_SMOLTCP_IFACE_MAX
#define NET_EVT_IFACE_UP    ESP_SMOLTCP_EVT_IFACE_UP
#define NET_EVT_IFACE_DOWN  ESP_SMOLTCP_EVT_IFACE_DOWN
#define NET_EVT_GOT_IP      ESP_SMOLTCP_EVT_GOT_IP
#define NET_EVT_LOST_IP     ESP_SMOLTCP_EVT_LOST_IP
#define NET_EVT_NTP_SYNCED  ESP_SMOLTCP_EVT_NTP_SYNCED
#define net_stack_init               esp_smoltcp_init
#define net_stack_register_event_cb  esp_smoltcp_register_event_cb
#define net_stack_wait_ready(ms)     esp_smoltcp_wait_for_ip(NET_IFACE_ETH, ms)
#define net_stack_get_ipv4           esp_smoltcp_get_ipv4
#define net_stack_get_gateway        esp_smoltcp_get_gateway
#define net_stack_get_netmask        esp_smoltcp_get_netmask
#define net_stack_get_stats          esp_smoltcp_get_stats
#define net_stack_frame_drops        esp_smoltcp_frame_pool_drops
#define net_stack_l2_tap             esp_smoltcp_l2_tap
#define net_stack_l2_send            esp_smoltcp_l2_send

/* Internal — used by lwip_compat's DNS resolver. */
void *net_stack_smoltcp_handle(esp_smoltcp_iface_t iface);

#ifdef __cplusplus
}
#endif
