/*
 * select() / poll() over our BSD-socket FDs — event-driven.
 *
 * Wakes on real activity rather than ticking. The smoltcp poll task
 * signals `PROGRESS_BIT` after every poll cycle (see
 * esp_smoltcp.c::poll_iface); esp_smoltcp_wait_progress() blocks until
 * that bit is set (clear-on-exit). So select() loops:
 *
 *   1. Snapshot readiness of every fd in the input sets.
 *   2. If any ready, return immediately.
 *   3. Otherwise wait on the progress bit with the remaining deadline.
 *   4. Re-scan; goto 2.
 *
 * This eliminates the `vTaskDelay(1)` per-tick spin that v0.1 used and
 * means tail latency on a freshly-readable socket is bounded by the
 * smoltcp poll cycle time, not a FreeRTOS tick (≥1 ms at the default
 * 1 kHz tick, 10 ms at 100 Hz).
 *
 * Notes:
 *  - The per-fd `evt` EventGroup in fd_table.h is reserved but unused
 *    here. The single global progress bit is the simpler design and
 *    scales fine for a few dozen fds. Per-fd granularity would only
 *    pay off if waking the poll task were expensive (it isn't — one
 *    xTaskNotifyGive).
 *  - Spurious wakes are POSIX-allowed; the rescan handles them.
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

/* One scan over the requested fd_sets. Writes the ready bits into
 * `rd`/`wr` (out) and returns the count. The caller masks against the
 * original input sets. */
static int scan_once(int nfds,
                     const fd_set *rd_in, const fd_set *wr_in,
                     fd_set *rd, fd_set *wr)
{
    int ready = 0;
    for (int fd = 0; fd < nfds; fd++) {
        if (!fd_is_socket(fd)) continue;
        fd_entry_t *e = fd_get(fd);
        if (!e) continue;
        if (rd && FD_ISSET(fd, rd_in) && fd_readable(e)) {
            FD_SET(fd, rd); ready++;
        }
        if (wr && FD_ISSET(fd, wr_in) && fd_writable(e)) {
            FD_SET(fd, wr); ready++;
        }
    }
    return ready;
}

int __wrap_lwip_select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *to)
{
    (void)ex;

    bool block_forever = (to == NULL);
    TickType_t now = xTaskGetTickCount();
    TickType_t deadline = 0;
    if (!block_forever) {
        uint32_t ms = to->tv_sec * 1000u + to->tv_usec / 1000u;
        deadline = now + pdMS_TO_TICKS(ms);
    }

    fd_set rd_in, wr_in;
    if (rd) rd_in = *rd; else FD_ZERO(&rd_in);
    if (wr) wr_in = *wr; else FD_ZERO(&wr_in);
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);

    for (;;) {
        int ready = scan_once(nfds, &rd_in, &wr_in, rd, wr);
        if (ready) return ready;

        if (!block_forever) {
            now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) return 0;   /* timed out */
        }

        /* Compute remaining time for this wait. Clamp to 1 s so callers
         * with portMAX_DELAY-equivalent timeouts still get periodic
         * rescans (cheap) and so we never feed an unsafe value into
         * pdMS_TO_TICKS. */
        uint32_t wait_ms;
        if (block_forever) {
            wait_ms = 1000;
        } else {
            TickType_t rem = deadline - now;
            uint32_t rem_ms = (uint32_t)((uint64_t)rem * 1000 / configTICK_RATE_HZ);
            wait_ms = rem_ms < 1000 ? rem_ms : 1000;
            if (wait_ms == 0) wait_ms = 1;
        }

        esp_smoltcp_wait_progress(wait_ms);
        /* Loop: rescan to see if we have readiness. */

        /* Clear the partial-write from the prior scan_once so the next
         * iteration starts clean. (scan_once only ever SETs bits — if
         * one wasn't ready before but is now, we still need to ensure
         * earlier "not ready" bits don't linger as set.) Actually
         * scan_once never sets a bit that wasn't already in the input
         * mask anyway, so the only bits in rd/wr are bits we set this
         * iteration. Re-zero them to avoid duplicate counting on the
         * next pass. */
        if (rd) FD_ZERO(rd);
        if (wr) FD_ZERO(wr);
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
