/*
 * getaddrinfo() / gethostbyname() wrappers — route to the smoltcp DNS
 * resolver. esp-tls and esp_http_client both use getaddrinfo, so this
 * is the gate that lets HTTPS work end-to-end.
 */

#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "sdkconfig.h"

#include "fd_table.h"
#include "esp_smoltcp.h"
#include "smoltcp_glue.h"

static const char *TAG = "netdb_wrap";

/* net_stack_smoltcp_handle returns a void* to keep esp_netif/etc. headers
 * out of net_stack.h; we cast it back to the smoltcp_iface_t the resolver
 * expects. */
static smoltcp_iface_t pick_iface(void)
{
    return (smoltcp_iface_t)net_stack_smoltcp_handle(fd_default_iface());
}

static uint32_t dns_server_be(void)
{
    uint32_t be = 0;
    smoltcp_parse_ipv4(CONFIG_NETSTACK_DNS_PRIMARY, &be);
    return be;
}

int __wrap_lwip_getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints, struct addrinfo **res)
{
    if (!node || !res) return EAI_FAIL;
    smoltcp_iface_t h = pick_iface();
    if (!h) return EAI_AGAIN;

    uint32_t addr_be = 0;
    if (smoltcp_resolve(h, node, dns_server_be(), CONFIG_LWIP_COMPAT_DNS_TIMEOUT_MS,
                        &addr_be) != 0) {
        ESP_LOGW(TAG, "resolve %s failed", node);
        return EAI_NONAME;
    }

    uint16_t port = 0;
    if (service) port = (uint16_t)atoi(service);

    struct addrinfo *ai = calloc(1, sizeof(*ai));
    struct sockaddr_in *sin = calloc(1, sizeof(*sin));
    if (!ai || !sin) { free(ai); free(sin); return EAI_MEMORY; }
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = addr_be;
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = 0;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    *res = ai;
    return 0;
}

void __wrap_lwip_freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}

struct hostent *__wrap_lwip_gethostbyname(const char *name)
{
    static struct hostent he;
    static char            name_copy[64];
    static uint32_t        addr_be;
    static char           *addr_list[2] = { (char *)&addr_be, NULL };

    smoltcp_iface_t h = pick_iface();
    if (!h) return NULL;
    if (smoltcp_resolve(h, name, dns_server_be(),
                        CONFIG_LWIP_COMPAT_DNS_TIMEOUT_MS, &addr_be) != 0) return NULL;

    strncpy(name_copy, name, sizeof(name_copy) - 1);
    he.h_name = name_copy;
    he.h_aliases = NULL;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = addr_list;
    return &he;
}

/*
 * inet_pton / inet_ntop only — IDF's <lwip/inet.h> exposes these as
 * lwip_inet_pton / lwip_inet_ntop. inet_aton and inet_ntoa are libc-only
 * legacy forms; we don't need them and they aren't `--wrap`able through
 * the lwIP path.
 */
int __wrap_lwip_inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET) return -1;
    uint32_t be;
    if (smoltcp_parse_ipv4(src, &be) != 0) return 0;
    *(uint32_t *)dst = be;
    return 1;
}
const char *__wrap_lwip_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if (af != AF_INET || size < 16) return NULL;
    const uint8_t *b = src;
    snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}
