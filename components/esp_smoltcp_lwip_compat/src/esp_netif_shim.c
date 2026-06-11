/*
 * Minimal esp_netif shim driven by smoltcp state.
 *
 * IDF networking components (esp_http_server, mDNS, esp-tls) call into
 * esp_netif for IP info, hostname, and interface enumeration. Stock IDF
 * builds esp_netif on top of lwIP; with our shim, the data plane is
 * smoltcp and esp_netif becomes a thin reporter.
 *
 * This is the API surface IDF code touches in practice. Anything we
 * don't override falls through to IDF's own esp_netif implementation
 * (which assumes lwIP) — those calls are expected to no-op or return
 * ESP_ERR_NOT_SUPPORTED. Add wraps below as you hit them.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "sdkconfig.h"

#include "esp_smoltcp.h"

/* CONFIG_APP_HOSTNAME comes from the consuming app's Kconfig (the original
 * template defines it). Standalone installs from the Component Registry
 * don't have it, so default it here. v0.2 replaces this with a proper
 * component-level Kconfig option (CONFIG_LWIP_COMPAT_HOSTNAME). */
#ifndef CONFIG_APP_HOSTNAME
#define CONFIG_APP_HOSTNAME "espressif"
#endif

__attribute__((unused))
static const char *TAG = "esp_netif_shim";

/*
 * Synthetic esp_netif handles. esp_netif_t is `typedef struct esp_netif_obj
 * esp_netif_t;` — opaque from outside. We can hand out pointers to our own
 * object as long as we wrap every API that dereferences them.
 *
 * Use a distinct struct tag (`shim_netif`) so we don't redefine
 * `esp_netif_obj` in the same translation unit as IDF's forward decl.
 */
struct shim_netif {
    net_iface_id_t iface;
    const char    *key;
};
static struct shim_netif s_netif_eth  = { NET_IFACE_ETH,  "ETH_DEF" };
static struct shim_netif s_netif_wifi = { NET_IFACE_WIFI, "WIFI_STA_DEF" };

esp_netif_t *__wrap_esp_netif_get_handle_from_ifkey(const char *ifkey)
{
    if (!ifkey) return NULL;
    if (strcmp(ifkey, "ETH_DEF") == 0)      return (esp_netif_t *)&s_netif_eth;
    if (strcmp(ifkey, "WIFI_STA_DEF") == 0) return (esp_netif_t *)&s_netif_wifi;
    return NULL;
}

esp_netif_t *__wrap_esp_netif_next_unsafe(esp_netif_t *prev)
{
#if CONFIG_NETSTACK_MODE_DUAL
    if (prev == NULL) return (esp_netif_t *)&s_netif_eth;
    if (prev == (esp_netif_t *)&s_netif_eth) return (esp_netif_t *)&s_netif_wifi;
    return NULL;
#elif CONFIG_NETSTACK_MODE_ETH
    return prev ? NULL : (esp_netif_t *)&s_netif_eth;
#elif CONFIG_NETSTACK_MODE_WIFI
    return prev ? NULL : (esp_netif_t *)&s_netif_wifi;
#else
    (void)prev; return NULL;
#endif
}

esp_err_t __wrap_esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    if (!netif || !info) return ESP_ERR_INVALID_ARG;
    struct shim_netif *o = (struct shim_netif *)netif;
    info->ip.addr      = net_stack_get_ipv4(o->iface);
    info->gw.addr      = net_stack_get_gateway(o->iface);
    info->netmask.addr = net_stack_get_netmask(o->iface);
    return ESP_OK;
}

esp_err_t __wrap_esp_netif_get_mac(esp_netif_t *netif, uint8_t mac[6])
{
    if (!netif || !mac) return ESP_ERR_INVALID_ARG;
    struct shim_netif *o = (struct shim_netif *)netif;
    return esp_read_mac(mac, o->iface == NET_IFACE_ETH ? ESP_MAC_ETH : ESP_MAC_WIFI_STA);
}

const char *__wrap_esp_netif_get_ifkey(esp_netif_t *netif)
{
    if (!netif) return "";
    return ((struct shim_netif *)netif)->key;
}

esp_err_t __wrap_esp_netif_get_hostname(esp_netif_t *netif, const char **hostname)
{
    (void)netif;
    if (!hostname) return ESP_ERR_INVALID_ARG;
    *hostname = CONFIG_APP_HOSTNAME;
    return ESP_OK;
}

esp_err_t __wrap_esp_netif_set_hostname(esp_netif_t *netif, const char *hostname)
{
    (void)netif; (void)hostname;
    /* No-op — hostname is build-time via CONFIG_APP_HOSTNAME. */
    return ESP_OK;
}

bool __wrap_esp_netif_is_netif_up(esp_netif_t *netif)
{
    if (!netif) return false;
    struct shim_netif *o = (struct shim_netif *)netif;
    return net_stack_get_ipv4(o->iface) != 0;
}
