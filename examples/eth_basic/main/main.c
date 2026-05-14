/*
 * Minimal esp_smoltcp bring-up: ETH driver, DHCP, log the IP.
 *
 * Demonstrates the user contract:
 *   1. App configures and installs its own esp_eth driver (PHY, pins,
 *      everything board-specific stays here in the app).
 *   2. esp_smoltcp_init() + esp_smoltcp_attach_eth(handle) and you're
 *      online — DHCP starts automatically.
 *   3. From here on, BSD sockets just work.
 *
 * Pins below are for the Waveshare ESP32-P4-Nano. Change to match your
 * board — esp_smoltcp doesn't care about pins, the eth driver setup
 * lives entirely in the app.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_phy_ip101.h"   // IDF v6.0+: from registry component
#include "nvs_flash.h"
#include "esp_smoltcp.h"

static const char *TAG = "eth_basic";

static esp_eth_handle_t install_ethernet(void)
{
    /* MAC: ETH_ESP32_EMAC_DEFAULT_CONFIG() for esp32p4 already matches
     * the Waveshare ESP32-P4-Nano pinout exactly:
     *   TX_EN  = GPIO49
     *   TXD0   = GPIO34
     *   TXD1   = GPIO35
     *   CRS_DV = GPIO28
     *   RXD0   = GPIO29
     *   RXD1   = GPIO30
     *   REF_CLK source = EMAC_CLK_EXT_IN on GPIO50 (50 MHz from the
     *                    PHY's 25 MHz crystal × 2)
     *   MDC    = GPIO31
     *   MDIO   = GPIO52
     *
     * We re-assign mdc/mdio for documentation; values are the same as
     * the macro defaults. PHY reset on GPIO51 below. */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = 31;
    emac_cfg.smi_gpio.mdio_num = 52;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = 1;
    phy_cfg.reset_gpio_num = 51;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t h;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &h));

    /* Hardware flow control: send 802.3x PAUSE frames when the RX
     * ring is filling and honour incoming PAUSE from the peer. Pairs
     * with CONFIG_ETH_SOFT_FLOW_CONTROL=y to keep the link healthy
     * under sustained ingress load. */
    bool flow_ctrl_enable = true;
    ESP_ERROR_CHECK(esp_eth_ioctl(h, ETH_CMD_S_FLOW_CTRL, &flow_ctrl_enable));

    return h;
}

static void on_evt(esp_smoltcp_iface_t iface, esp_smoltcp_event_t evt, void *user)
{
    (void)user;
    if (evt == ESP_SMOLTCP_EVT_GOT_IP) {
        uint32_t ip = esp_smoltcp_get_ipv4(iface);
        uint8_t b[4]; memcpy(b, &ip, 4);
        ESP_LOGI(TAG, "iface=%d got IP %u.%u.%u.%u", (int)iface,
                 b[0], b[1], b[2], b[3]);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_eth_handle_t eth = install_ethernet();

    ESP_ERROR_CHECK(esp_smoltcp_init());
    esp_smoltcp_register_event_cb(on_evt, NULL);
    ESP_ERROR_CHECK(esp_smoltcp_attach_eth(eth));

    if (esp_smoltcp_wait_for_ip(ESP_SMOLTCP_IFACE_ETH, 15000) == ESP_OK) {
        ESP_LOGI(TAG, "online — BSD sockets are now functional");
    } else {
        ESP_LOGW(TAG, "no IP after 15 s — check link / DHCP");
    }
}
