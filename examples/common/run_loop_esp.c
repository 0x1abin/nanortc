/*
 * nanortc examples — ESP-IDF event loop (lwIP select-based)
 *
 * Functionally identical to run_loop_linux.c, adapted for ESP-IDF:
 *   - lwIP socket headers instead of POSIX
 *   - esp_timer_get_time() for monotonic clock
 *   - Single interface (WiFi STA), uses fds[0] only
 *
 * SPDX-License-Identifier: MIT
 */

#include "run_loop.h"

#include <stdio.h>
#include <string.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_timer.h>

uint32_t nano_get_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int nano_run_loop_init(nano_run_loop_t *loop, nanortc_t *rtc, uint16_t port)
{
    if (!loop || !rtc) {
        return -1;
    }

    memset(loop, 0, sizeof(*loop));
    loop->rtc = rtc;
    loop->port = port;
    for (int i = 0; i < NANORTC_MAX_LOCAL_CANDIDATES; i++) {
        loop->fds[i] = -1;
    }

#if NANORTC_FEATURE_IPV6 && defined(CONFIG_LWIP_IPV6)
    /* Dual-stack: AF_INET6 with IPV6_V6ONLY=0 accepts both v4 and v6 */
    int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        printf("[run_loop] socket(AF_INET6) failed: %d\n", errno);
        return -1;
    }
    int v6only = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in6 local;
    memset(&local, 0, sizeof(local));
    local.sin6_family = AF_INET6;
    local.sin6_port = htons(port);

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        printf("[run_loop] bind(IPv6) failed: %d\n", errno);
        close(fd);
        return -1;
    }
#else
    /* IPv4-only */
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        printf("[run_loop] socket() failed: %d\n", errno);
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        printf("[run_loop] bind() failed: %d\n", errno);
        close(fd);
        return -1;
    }
#endif

    loop->fds[0] = fd;
    loop->fd_count = 1;

    return 0;
}

int nano_run_loop_auto_candidates(nano_run_loop_t *loop, nanortc_t *rtc, uint16_t port)
{
    /* ESP-IDF typically has a single interface (WiFi STA).
     * Delegate to nano_run_loop_init (binds INADDR_ANY). */
    return nano_run_loop_init(loop, rtc, port);
}

void nano_run_loop_set_event_cb(nano_run_loop_t *loop, nano_event_cb cb, void *userdata)
{
    if (loop) {
        loop->event_cb = cb;
        loop->event_userdata = userdata;
    }
}

static void dispatch_outputs(nano_run_loop_t *loop, uint32_t *timeout_ms)
{
    nanortc_output_t out;

    while (nanortc_poll_output(loop->rtc, &out) == NANORTC_OK) {
        switch (out.type) {
        case NANORTC_OUTPUT_TRANSMIT: {
            /* Build destination sockaddr from nanortc_addr_t */
#if NANORTC_FEATURE_IPV6 && defined(CONFIG_LWIP_IPV6)
            if (out.transmit.dest.family == 6) {
                struct sockaddr_in6 dest6;
                memset(&dest6, 0, sizeof(dest6));
                dest6.sin6_family = AF_INET6;
                memcpy(&dest6.sin6_addr, out.transmit.dest.addr, 16);
                dest6.sin6_port = htons(out.transmit.dest.port);
                sendto(loop->fds[0], out.transmit.data, out.transmit.len, 0,
                       (struct sockaddr *)&dest6, sizeof(dest6));
            } else {
                /* IPv4: wrap in IPv4-mapped IPv6 for dual-stack socket */
                struct sockaddr_in6 dest6;
                memset(&dest6, 0, sizeof(dest6));
                dest6.sin6_family = AF_INET6;
                /* ::ffff:a.b.c.d */
                dest6.sin6_addr.s6_addr[10] = 0xff;
                dest6.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&dest6.sin6_addr.s6_addr[12], out.transmit.dest.addr, 4);
                dest6.sin6_port = htons(out.transmit.dest.port);
                sendto(loop->fds[0], out.transmit.data, out.transmit.len, 0,
                       (struct sockaddr *)&dest6, sizeof(dest6));
            }
#else
            {
                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family = AF_INET;
                memcpy(&dest.sin_addr, out.transmit.dest.addr, 4);
                dest.sin_port = htons(out.transmit.dest.port);
                sendto(loop->fds[0], out.transmit.data, out.transmit.len, 0,
                       (struct sockaddr *)&dest, sizeof(dest));
            }
#endif
            break;
        }
        case NANORTC_OUTPUT_EVENT:
            if (loop->event_cb) {
                loop->event_cb(loop->rtc, &out.event, loop->event_userdata);
            }
            break;

        case NANORTC_OUTPUT_TIMEOUT:
            if (out.timeout_ms < *timeout_ms) {
                *timeout_ms = out.timeout_ms;
            }
            break;
        }
    }
}

int nano_run_loop_step(nano_run_loop_t *loop)
{
    if (!loop || loop->fd_count == 0) {
        return -1;
    }

    uint32_t timeout_ms = loop->max_poll_ms ? loop->max_poll_ms : 100;

    /* Drain output queue */
    dispatch_outputs(loop, &timeout_ms);

    /* Wait for network input or timeout */
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(loop->fds[0], &rset);

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    int ret = select(loop->fds[0] + 1, &rset, NULL, NULL, &tv);
    uint32_t now = nano_get_millis();

    if (ret > 0 && FD_ISSET(loop->fds[0], &rset)) {
        uint8_t buf[1500];
#if NANORTC_FEATURE_IPV6 && defined(CONFIG_LWIP_IPV6)
        struct sockaddr_in6 from6;
        socklen_t fromlen = sizeof(from6);
        int n = recvfrom(loop->fds[0], buf, sizeof(buf), 0, (struct sockaddr *)&from6, &fromlen);
        if (n > 0) {
            nanortc_input_t in = {.now_ms = now, .data = buf, .len = (size_t)n};
            /* Check for IPv4-mapped IPv6 (::ffff:x.x.x.x) */
            const uint8_t *a = from6.sin6_addr.s6_addr;
            if (from6.sin6_family == AF_INET6 && a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0 &&
                a[4] == 0 && a[5] == 0 && a[6] == 0 && a[7] == 0 && a[8] == 0 && a[9] == 0 &&
                a[10] == 0xff && a[11] == 0xff) {
                /* IPv4-mapped → extract v4 address */
                in.src.family = 4;
                memcpy(in.src.addr, &a[12], 4);
            } else {
                in.src.family = 6;
                memcpy(in.src.addr, a, 16);
            }
            in.src.port = ntohs(from6.sin6_port);
            nanortc_handle_input(loop->rtc, &in);
        }
#else
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(loop->fds[0], buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            nanortc_input_t in = {.now_ms = now, .data = buf, .len = (size_t)n};
            in.src.family = 4;
            memcpy(in.src.addr, &from.sin_addr, 4);
            in.src.port = ntohs(from.sin_port);
            nanortc_handle_input(loop->rtc, &in);
        }
#endif
    } else {
        nanortc_input_t tick = {.now_ms = now};
        nanortc_handle_input(loop->rtc, &tick);
    }

    /* Dispatch any new output generated by handle_input */
    dispatch_outputs(loop, &timeout_ms);

    return 0;
}

int nano_run_loop_run(nano_run_loop_t *loop)
{
    if (!loop) {
        return -1;
    }

    loop->running = 1;
    while (loop->running) {
        int rc = nano_run_loop_step(loop);
        if (rc < 0) {
            return rc;
        }
    }
    return 0;
}

void nano_run_loop_stop(nano_run_loop_t *loop)
{
    if (loop) {
        loop->running = 0;
    }
}

void nano_run_loop_destroy(nano_run_loop_t *loop)
{
    if (!loop)
        return;
    for (uint8_t i = 0; i < loop->fd_count; i++) {
        if (loop->fds[i] >= 0) {
            close(loop->fds[i]);
            loop->fds[i] = -1;
        }
    }
    loop->fd_count = 0;
}
