/*
 * BSD socket wrappers — every IDF call to socket()/bind()/etc. ends up
 * here via the linker --wrap mechanism (see CMakeLists.txt).
 */

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "fd_table.h"
#include "loopback.h"
#include "esp_smoltcp.h"
#include "esp_smoltcp_socket.h"
#include "smoltcp_glue.h"

static const char *TAG = "sock_wrap";

#define SET_ERRNO(e) do { errno = (e); } while (0)

/* ---------------------------------------------------------------------- */
int __wrap_lwip_socket(int domain, int type, int protocol)
{
    bool v6;
    if      (domain == AF_INET)  v6 = false;
    else if (domain == AF_INET6) v6 = true;
    else { SET_ERRNO(EAFNOSUPPORT); return -1; }
    fd_kind_t k;
    switch (type & 0xFF) {
        case SOCK_STREAM: k = FD_TCP; break;
        case SOCK_DGRAM:  k = FD_UDP; break;
        default: SET_ERRNO(EPROTONOSUPPORT); return -1;
    }
    int fd = fd_alloc(k);
    if (fd < 0) { SET_ERRNO(ENFILE); return -1; }

    fd_entry_t *e = fd_get(fd);
    /* SOCK_NONBLOCK / SOCK_CLOEXEC are Linux-specific; newlib doesn't
     * define them. Callers can request non-blocking via fcntl(O_NONBLOCK). */
    e->nonblocking = false;
    e->is_v6 = v6;

    if (k == FD_UDP) {
        e->sock = net_udp_open(e->iface, 0);
        if (!net_sock_valid(e->sock)) { fd_free(fd); SET_ERRNO(ENOBUFS); return -1; }
    }
    /* TCP smoltcp socket is opened lazily on connect()/listen() — smoltcp
     * needs the role decided up front. */
    return fd;
}

/* ---------------------------------------------------------------------- */
int __wrap_lwip_bind(int fd, const struct sockaddr *addr, socklen_t alen)
{
    fd_entry_t *e = fd_get(fd);
    if (!e || !addr) { SET_ERRNO(EINVAL); return -1; }

    if (addr->sa_family == AF_INET6 && alen >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const void *)addr;
        memcpy(e->local_ipv6, &sin6->sin6_addr, 16);
        e->local_port = ntohs(sin6->sin6_port);
        e->is_v6 = true;
        if (e->kind == FD_UDP) {
            /* No in-RAM ::1 loopback for now; route v6 through smoltcp. */
            if (net_sock_valid(e->sock)) net_udp_close(e->sock);
            e->sock = net_udp_open(e->iface, e->local_port);
            if (!net_sock_valid(e->sock)) { SET_ERRNO(EADDRINUSE); return -1; }
        }
        return 0;
    }

    if (addr->sa_family != AF_INET || alen < (socklen_t)sizeof(struct sockaddr_in)) {
        SET_ERRNO(EINVAL); return -1;
    }
    const struct sockaddr_in *sin = (const void *)addr;
    e->local_ipv4_be = sin->sin_addr.s_addr;
    e->local_port    = ntohs(sin->sin_port);

    if (e->kind == FD_UDP) {
        /* 127.0.0.0/8 binds get an in-process loopback registration
         * instead of a smoltcp socket — see components/lwip_compat/
         * src/loopback.c for the rationale. */
        if (is_loopback_ip(e->local_ipv4_be)) {
            return lo_bind(e->local_port, e) == ESP_OK ? 0 : -1;
        }
        /* Re-open on the requested local port. */
        if (net_sock_valid(e->sock)) net_udp_close(e->sock);
        e->sock = net_udp_open(e->iface, e->local_port);
        if (!net_sock_valid(e->sock)) { SET_ERRNO(EADDRINUSE); return -1; }
    }
    /* TCP: stored, applied on listen()/connect(). */
    return 0;
}

/* ---------------------------------------------------------------------- */
int __wrap_lwip_listen(int fd, int backlog)
{
    fd_entry_t *e = fd_get(fd);
    if (!e || e->kind != FD_TCP) { SET_ERRNO(EBADF); return -1; }
    if (e->local_port == 0)      { SET_ERRNO(EINVAL); return -1; }

    int pool_size = backlog > 0 ? backlog : CONFIG_LWIP_COMPAT_LISTENER_POOL;
    if (pool_size > CONFIG_LWIP_COMPAT_LISTENER_POOL)
        pool_size = CONFIG_LWIP_COMPAT_LISTENER_POOL;

    e->pool = calloc(pool_size, sizeof(net_sock_t));
    if (!e->pool) { SET_ERRNO(ENOMEM); return -1; }
    e->pool_len  = pool_size;
    e->pool_next = 0;
    e->port      = e->local_port;
    e->kind      = FD_TCP_LISTEN;

    /* Pre-arm every slot. smoltcp puts the socket in LISTEN state. */
    for (int i = 0; i < pool_size; i++) {
        e->pool[i] = net_tcp_open(e->iface);
        if (!net_sock_valid(e->pool[i])) { SET_ERRNO(ENOBUFS); return -1; }
        if (net_tcp_listen(e->pool[i], e->port) != ESP_OK) {
            SET_ERRNO(EADDRINUSE); return -1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
int __wrap_lwip_accept(int fd, struct sockaddr *addr, socklen_t *alen)
{
    fd_entry_t *e = fd_get(fd);
    if (!e || e->kind != FD_TCP_LISTEN) { SET_ERRNO(EBADF); return -1; }

    /* Spin until one of the pool sockets transitions to ESTABLISHED.
     * TODO: replace with a smoltcp_glue listen-state event so we can
     * block on a semaphore instead of polling. */
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        for (int i = 0; i < e->pool_len; i++) {
            int idx = (e->pool_next + i) % e->pool_len;
            net_sock_t cand = e->pool[idx];
            if (net_tcp_is_connected(cand)) {
                /* "Accepted" — hand this socket over to a new FD and
                 * replenish the pool slot. */
                int newfd = fd_alloc(FD_TCP);
                if (newfd < 0) { SET_ERRNO(ENFILE); return -1; }
                fd_entry_t *ne = fd_get(newfd);
                ne->sock = cand;
                ne->connected = true;
                ne->iface = e->iface;
                ne->nonblocking = false;

                /* Replenish */
                e->pool[idx] = net_tcp_open(e->iface);
                if (net_sock_valid(e->pool[idx])) {
                    net_tcp_listen(e->pool[idx], e->port);
                }
                e->pool_next = (idx + 1) % e->pool_len;

                if (addr && alen && *alen >= sizeof(struct sockaddr_in)) {
                    /* TODO: surface peer address from smoltcp.
                     * Returning 0.0.0.0:0 is technically allowed by BSD. */
                    struct sockaddr_in sin = { .sin_family = AF_INET };
                    memcpy(addr, &sin, sizeof(sin));
                    *alen = sizeof(sin);
                }
                ESP_LOGD(TAG, "accept: listen_fd=%d -> conn_fd=%d (port=%u)",
                         fd, newfd, e->port);
                return newfd;
            }
        }
        if (e->nonblocking) { SET_ERRNO(EWOULDBLOCK); return -1; }
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(60000)) {
            SET_ERRNO(ETIMEDOUT); return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* ---------------------------------------------------------------------- */
int __wrap_lwip_connect(int fd, const struct sockaddr *addr, socklen_t alen)
{
    fd_entry_t *e = fd_get(fd);
    if (!e || !addr) { SET_ERRNO(EINVAL); return -1; }

    if (addr->sa_family == AF_INET6 && alen >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const void *)addr;
        uint16_t port = ntohs(sin6->sin6_port);
        if (e->kind == FD_TCP) {
            e->sock = net_tcp_open(e->iface);
            if (!net_sock_valid(e->sock)) { SET_ERRNO(ENOBUFS); return -1; }
            esp_err_t er = net_tcp_connect6(e->sock,
                                            (const uint8_t *)&sin6->sin6_addr,
                                            port,
                                            e->snd_timeout_ms ? e->snd_timeout_ms : 30000);
            if (er != ESP_OK) { SET_ERRNO(ETIMEDOUT); return -1; }
            e->connected = true;
            e->is_v6 = true;
            return 0;
        }
        if (e->kind == FD_UDP) {
            memcpy(e->local_ipv6, &sin6->sin6_addr, 16);
            e->local_port = port;
            e->connected = true;
            e->is_v6 = true;
            return 0;
        }
        SET_ERRNO(EBADF); return -1;
    }

    if (alen < (socklen_t)sizeof(struct sockaddr_in)) { SET_ERRNO(EINVAL); return -1; }
    const struct sockaddr_in *sin = (const void *)addr;
    if (e->kind == FD_TCP) {
        e->sock = net_tcp_open(e->iface);
        if (!net_sock_valid(e->sock)) { SET_ERRNO(ENOBUFS); return -1; }
        esp_err_t er = net_tcp_connect(e->sock, sin->sin_addr.s_addr,
                                       ntohs(sin->sin_port),
                                       e->snd_timeout_ms ? e->snd_timeout_ms : 30000);
        if (er != ESP_OK) { SET_ERRNO(ETIMEDOUT); return -1; }
        e->connected = true;
        return 0;
    }
    if (e->kind == FD_UDP) {
        /* "Connected" UDP just stores the default destination; we tag
         * it by treating subsequent send()/recv() like sendto/recvfrom. */
        e->local_ipv4_be = sin->sin_addr.s_addr;
        e->local_port    = ntohs(sin->sin_port);
        e->connected = true;
        return 0;
    }
    SET_ERRNO(EBADF); return -1;
}

/* ---------------------------------------------------------------------- */
ssize_t __wrap_lwip_send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    fd_entry_t *e = fd_get(fd);
    if (!e || !e->connected) { SET_ERRNO(ENOTCONN); return -1; }
    if (e->kind == FD_TCP) {
        return net_tcp_send(e->sock, buf, len, e->snd_timeout_ms ? e->snd_timeout_ms : 5000);
    }
    if (e->kind == FD_UDP) {
        if (e->is_v6) {
            return net_udp_sendto6(e->sock, buf, len, e->local_ipv6, e->local_port);
        }
        return net_udp_sendto(e->sock, buf, len, e->local_ipv4_be, e->local_port);
    }
    SET_ERRNO(EBADF); return -1;
}

ssize_t __wrap_lwip_recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    fd_entry_t *e = fd_get(fd);
    if (!e) { SET_ERRNO(EBADF); return -1; }
    uint32_t to = e->rcv_timeout_ms ? e->rcv_timeout_ms : 30000;
    if (e->nonblocking) to = 0;
    if (e->kind == FD_TCP) {
        int n = net_tcp_recv(e->sock, buf, len, to);
        if (n == 0 && e->nonblocking) { SET_ERRNO(EWOULDBLOCK); return -1; }
        return n;
    }
    if (e->kind == FD_UDP) {
        uint32_t src; uint16_t sp;
        int n = net_udp_recvfrom(e->sock, buf, len, &src, &sp, to);
        if (n == 0 && e->nonblocking) { SET_ERRNO(EWOULDBLOCK); return -1; }
        return n;
    }
    SET_ERRNO(EBADF); return -1;
}

ssize_t __wrap_lwip_sendto(int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *to, socklen_t tolen)
{
    (void)flags;
    fd_entry_t *e = fd_get(fd);
    if (!e || e->kind != FD_UDP || !to) { SET_ERRNO(EBADF); return -1; }

    if (to->sa_family == AF_INET6 && tolen >= (socklen_t)sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sin6 = (const void *)to;
        return net_udp_sendto6(e->sock, buf, len,
                               (const uint8_t *)&sin6->sin6_addr,
                               ntohs(sin6->sin6_port));
    }

    if (tolen < (socklen_t)sizeof(struct sockaddr_in)) { SET_ERRNO(EINVAL); return -1; }
    const struct sockaddr_in *sin = (const void *)to;
    uint32_t dst_be = sin->sin_addr.s_addr;
    uint16_t dst_port = ntohs(sin->sin_port);
    /* Loopback short-circuit — keep 127.x.x.x traffic in RAM. */
    if (is_loopback_ip(dst_be)) {
        return lo_deliver(dst_port, e->local_ipv4_be, e->local_port, buf, len);
    }
    return net_udp_sendto(e->sock, buf, len, dst_be, dst_port);
}

ssize_t __wrap_lwip_recvfrom(int fd, void *buf, size_t len, int flags,
                        struct sockaddr *from, socklen_t *fromlen)
{
    (void)flags;
    fd_entry_t *e = fd_get(fd);
    if (!e || e->kind != FD_UDP) { SET_ERRNO(EBADF); return -1; }
    uint32_t to = e->rcv_timeout_ms ? e->rcv_timeout_ms : 30000;
    if (e->nonblocking) to = 0;

    /* IPv6-capable path: ask smoltcp for the v6 src. If the actual
     * datagram source was v4, smoltcp tells us via is_v6_out and we
     * fall back to the v4 src reporter. */
    if (e->is_v6) {
        uint8_t src6[16] = {0};
        uint16_t sp = 0;
        int is_v6_out = 0;
        int n = net_udp_recvfrom6(e->sock, buf, len, src6, &sp, &is_v6_out, to);
        if (n == 0 && e->nonblocking) { SET_ERRNO(EWOULDBLOCK); return -1; }
        if (n > 0 && from && fromlen && *fromlen >= sizeof(struct sockaddr_in6)) {
            struct sockaddr_in6 sin6 = {
                .sin6_family = AF_INET6,
                .sin6_port   = htons(sp),
            };
            if (is_v6_out) {
                memcpy(&sin6.sin6_addr, src6, 16);
            }
            /* else: src is v4, but the caller expects sockaddr_in6.
             * Encode as v4-mapped IPv6 (::ffff:a.b.c.d). For now leave
             * as :: — caller-side dual-stack code rarely depends on this. */
            memcpy(from, &sin6, sizeof(sin6));
            *fromlen = sizeof(sin6);
        }
        return n;
    }

    uint32_t src_be = 0; uint16_t sp = 0;
    int n;
    if (e->loopback && e->lo_rx) {
        lo_msg_t *m = NULL;
        if (xQueueReceive(e->lo_rx, &m, pdMS_TO_TICKS(to)) != pdTRUE) {
            if (e->nonblocking) { SET_ERRNO(EWOULDBLOCK); return -1; }
            return 0;
        }
        size_t copy = m->len < len ? m->len : len;
        memcpy(buf, m->data, copy);
        src_be = m->src_be;
        sp     = m->src_port;
        n      = (int)copy;
        free(m);
    } else {
        n = net_udp_recvfrom(e->sock, buf, len, &src_be, &sp, to);
    }

    if (n > 0 && from && fromlen && *fromlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in sin = {
            .sin_family = AF_INET,
            .sin_port = htons(sp),
            .sin_addr.s_addr = src_be,
        };
        memcpy(from, &sin, sizeof(sin));
        *fromlen = sizeof(sin);
    }
    return n;
}

/* ---------------------------------------------------------------------- */
int __wrap_lwip_close(int fd)
{
    if (!fd_is_socket(fd)) {
        extern int __real_lwip_close(int);
        return __real_lwip_close(fd);
    }
    fd_free(fd);
    return 0;
}

int __wrap_lwip_shutdown(int fd, int how)
{
    (void)how;
    fd_entry_t *e = fd_get(fd);
    if (!e) { SET_ERRNO(EBADF); return -1; }
    /* smoltcp's TCP socket close() is graceful (sends FIN); approximate
     * SHUT_WR/SHUT_RDWR with that. SHUT_RD is best-effort no-op. */
    if (e->kind == FD_TCP && net_sock_valid(e->sock)) {
        net_tcp_close(e->sock);
        e->sock = NET_SOCK_INVALID;
        e->connected = false;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
int __wrap_lwip_setsockopt(int fd, int level, int optname,
                      const void *optval, socklen_t optlen)
{
    fd_entry_t *e = fd_get(fd);
    if (!e) { SET_ERRNO(EBADF); return -1; }
    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_RCVTIMEO: {
                if (optlen >= sizeof(struct timeval)) {
                    const struct timeval *tv = optval;
                    e->rcv_timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
                }
                return 0;
            }
            case SO_SNDTIMEO: {
                if (optlen >= sizeof(struct timeval)) {
                    const struct timeval *tv = optval;
                    e->snd_timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
                }
                return 0;
            }
            case SO_REUSEADDR:
            case SO_KEEPALIVE:
            case SO_BROADCAST:
            case SO_LINGER:
            case SO_SNDBUF:
            case SO_RCVBUF:
                return 0;  /* accept silently */
        }
    }
    if (level == IPPROTO_TCP) {
        switch (optname) {
            case TCP_NODELAY: return 0;  /* smoltcp default-on; ignore */
            case TCP_KEEPIDLE: case TCP_KEEPINTVL: case TCP_KEEPCNT: return 0;
        }
    }
    if (level == IPPROTO_IP) {
        switch (optname) {
            case IP_ADD_MEMBERSHIP: {
                if (optlen >= sizeof(struct ip_mreq)) {
                    const struct ip_mreq *mr = optval;
                    return net_mcast_join(e->iface, mr->imr_multiaddr.s_addr) == ESP_OK ? 0 : -1;
                }
                break;
            }
            case IP_DROP_MEMBERSHIP: {
                if (optlen >= sizeof(struct ip_mreq)) {
                    const struct ip_mreq *mr = optval;
                    return net_mcast_leave(e->iface, mr->imr_multiaddr.s_addr) == ESP_OK ? 0 : -1;
                }
                break;
            }
            case IP_MULTICAST_TTL: case IP_MULTICAST_LOOP: case IP_MULTICAST_IF:
                return 0;
        }
    }
    ESP_LOGD(TAG, "setsockopt level=%d opt=%d ignored", level, optname);
    return 0;
}

int __wrap_lwip_getsockopt(int fd, int level, int optname,
                      void *optval, socklen_t *optlen)
{
    (void)level; (void)optname; (void)optval; (void)optlen;
    fd_entry_t *e = fd_get(fd);
    if (!e) { SET_ERRNO(EBADF); return -1; }
    /* Minimum: return 0 for SO_ERROR so esp_http_client doesn't spin. */
    if (level == SOL_SOCKET && optname == SO_ERROR && optval && optlen && *optlen >= sizeof(int)) {
        *(int*)optval = 0;
        *optlen = sizeof(int);
    }
    return 0;
}

int __wrap_lwip_getsockname(int fd, struct sockaddr *addr, socklen_t *alen)
{
    fd_entry_t *e = fd_get(fd);
    if (!e || !addr || !alen) { SET_ERRNO(EINVAL); return -1; }

    if (e->is_v6 && *alen >= sizeof(struct sockaddr_in6)) {
        struct sockaddr_in6 sin6 = {
            .sin6_family = AF_INET6,
            .sin6_port   = htons(e->local_port),
        };
        /* If the app bound to a specific v6 addr, return it; else
         * fill in the iface's link-local. */
        bool any = true;
        for (int i = 0; i < 16; i++) if (e->local_ipv6[i]) { any = false; break; }
        if (any) {
            esp_smoltcp_get_ipv6_link_local(e->iface, (uint8_t *)&sin6.sin6_addr);
        } else {
            memcpy(&sin6.sin6_addr, e->local_ipv6, 16);
        }
        memcpy(addr, &sin6, sizeof(sin6));
        *alen = sizeof(sin6);
        return 0;
    }

    if (*alen < sizeof(struct sockaddr_in)) { SET_ERRNO(EINVAL); return -1; }
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(e->local_port),
        .sin_addr.s_addr = net_stack_get_ipv4(e->iface),
    };
    memcpy(addr, &sin, sizeof(sin));
    *alen = sizeof(sin);
    return 0;
}

int __wrap_lwip_getpeername(int fd, struct sockaddr *addr, socklen_t *alen)
{
    /* TODO: track peer; returning EOPNOTSUPP for now. */
    (void)fd; (void)addr; (void)alen;
    SET_ERRNO(EOPNOTSUPP); return -1;
}

/* ---------------------------------------------------------------------- */
int __wrap_lwip_fcntl(int fd, int cmd, int arg)
{
    fd_entry_t *e = fd_get(fd);
    if (!e) {
        extern int __real_lwip_fcntl(int, int, int);
        return __real_lwip_fcntl(fd, cmd, arg);
    }
    switch (cmd) {
        case F_GETFL: return e->nonblocking ? O_NONBLOCK : 0;
        case F_SETFL:
            e->nonblocking = (arg & O_NONBLOCK) != 0;
            return 0;
    }
    return 0;
}

int __wrap_lwip_ioctl(int fd, long cmd, void *arg)
{
    /* Most callers want FIONBIO. */
    if (cmd == FIONBIO) {
        fd_entry_t *e = fd_get(fd);
        if (!e) { SET_ERRNO(EBADF); return -1; }
        e->nonblocking = arg && *(int*)arg;
        return 0;
    }
    extern int __real_lwip_ioctl(int, long, void *);
    return __real_lwip_ioctl(fd, cmd, arg);
}

/* read()/write() on socket FDs route here; on regular FDs fall through. */
ssize_t __wrap_lwip_read(int fd, void *buf, size_t len)
{
    if (fd_is_socket(fd)) return __wrap_lwip_recv(fd, buf, len, 0);
    extern ssize_t __real_lwip_read(int, void *, size_t);
    return __real_lwip_read(fd, buf, len);
}
ssize_t __wrap_lwip_write(int fd, const void *buf, size_t len)
{
    if (fd_is_socket(fd)) return __wrap_lwip_send(fd, buf, len, 0);
    extern ssize_t __real_lwip_write(int, const void *, size_t);
    return __real_lwip_write(fd, buf, len);
}
