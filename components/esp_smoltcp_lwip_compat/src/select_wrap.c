/*
 * select() / poll() over our BSD-socket FDs.
 *
 * Implementation: poll-based with a short sleep between scans. This is
 * the same shape as lwIP's older `lwip_select()` and is good enough for
 * esp_http_server, esp-mqtt, and the like — all of which use select()
 * with multi-second timeouts on a small handful of FDs.
 *
 * For sub-millisecond responsiveness, replace the inner sleep with an
 * EventGroup wait fed by per-FD readability/writability bits set from
 * the net_stack task on each smoltcp poll cycle. (TODO.)
 */

#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/poll.h>     /* newlib has it here, not at <poll.h> */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fd_table.h"
#include "esp_smoltcp.h"
#include "esp_smoltcp_socket.h"

static bool fd_readable(fd_entry_t *e)
{
    if (e->kind == FD_TCP_LISTEN) {
        for (int i = 0; i < e->pool_len; i++) {
            if (net_tcp_is_connected(e->pool[i])) return true;
        }
        return false;
    }
    if (e->loopback && e->lo_rx) {
        return uxQueueMessagesWaiting(e->lo_rx) > 0;
    }
    if (e->kind == FD_TCP) {
        /* Ready when actual bytes are buffered for recv. Also report
         * ready on a half-closed/closing socket so the caller can drain
         * the close (read returns 0). */
        if (net_tcp_recv_queue(e->sock) > 0) return true;
        return !net_tcp_is_connected(e->sock) && e->connected;
    }
    if (e->kind == FD_UDP) {
        /* Probe with a zero-length read against smoltcp's UDP. */
        uint8_t b;
        int n = net_udp_recvfrom(e->sock, &b, 0, NULL, NULL, 0);
        return n > 0;
    }
    return false;
}

static bool fd_writable(fd_entry_t *e)
{
    if (e->kind == FD_TCP) {
        return e->connected && net_tcp_send_capacity(e->sock) > 0;
    }
    if (e->kind == FD_UDP) return net_sock_valid(e->sock) || e->loopback;
    return false;
}

int __wrap_lwip_select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *to)
{
    (void)ex;
    TickType_t deadline;
    if (to) {
        uint32_t ms = to->tv_sec * 1000u + to->tv_usec / 1000u;
        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    } else {
        deadline = portMAX_DELAY;
    }

    fd_set rd_in, wr_in;
    if (rd) rd_in = *rd; else FD_ZERO(&rd_in);
    if (wr) wr_in = *wr; else FD_ZERO(&wr_in);
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);

    for (;;) {
        int ready = 0;
        for (int fd = 0; fd < nfds; fd++) {
            if (!fd_is_socket(fd)) continue;
            fd_entry_t *e = fd_get(fd);
            if (!e) continue;
            if (FD_ISSET(fd, &rd_in) && fd_readable(e)) {
                FD_SET(fd, rd); ready++;
            }
            if (FD_ISSET(fd, &wr_in) && fd_writable(e)) {
                FD_SET(fd, wr); ready++;
            }
        }
        if (ready) return ready;
        if (deadline != portMAX_DELAY && xTaskGetTickCount() >= deadline) return 0;
        vTaskDelay(1);   /* 1 tick — minimum FreeRTOS yield */
    }
}

int __wrap_lwip_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    /* Synthesize over select() — sufficient for esp_http_server. */
    fd_set rd, wr;
    FD_ZERO(&rd); FD_ZERO(&wr);
    int max = -1;
    for (nfds_t i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        if (fds[i].events & POLLIN)  FD_SET(fd, &rd);
        if (fds[i].events & POLLOUT) FD_SET(fd, &wr);
        if (fd > max) max = fd;
        fds[i].revents = 0;
    }
    struct timeval tv = { .tv_sec = timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    int rc = __wrap_lwip_select(max + 1, &rd, &wr, NULL, timeout_ms < 0 ? NULL : &tv);
    if (rc <= 0) return rc;
    int ready = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        if (FD_ISSET(fds[i].fd, &rd)) { fds[i].revents |= POLLIN;  ready++; }
        if (FD_ISSET(fds[i].fd, &wr)) { fds[i].revents |= POLLOUT; ready++; }
    }
    return ready;
}
