/*
 * nanortc examples — Linux event loop (select-based)
 *
 * Reference: design doc §5.3
 *
 * SPDX-License-Identifier: MIT
 */

#include "run_loop.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <ifaddrs.h>
#include <net/if.h>

uint32_t nano_get_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Bind a UDP socket on INADDR_ANY (or dual-stack with IPv6).
 * Returns fd on success, -1 on failure. */
static int bind_udp_any(uint16_t port)
{
#if NANORTC_FEATURE_IPV6
    /* Dual-stack: AF_INET6 with IPV6_V6ONLY=0 accepts both v4 and v6 */
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(AF_INET6)");
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
    local.sin6_addr = in6addr_any;

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind(IPv6)");
        close(fd);
        return -1;
    }
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
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
        perror("bind");
        close(fd);
        return -1;
    }
#endif
    return fd;
}

/* Bind a UDP socket to a specific IPv4 address and port (for auto_candidates).
 * Returns fd on success, -1 on failure. */
static int bind_udp_ipv4(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    inet_pton(AF_INET, ip, &local.sin_addr);

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    return fd;
}

#if NANORTC_FEATURE_IPV6
/* Bind a UDP socket to a specific IPv6 address and port.
 * IPV6_V6ONLY=1 so v4-mapped traffic goes through the dedicated v4 fd.
 * Returns fd on success, -1 on failure. */
static int bind_udp_ipv6(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(AF_INET6)");
        return -1;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int v6only = 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    struct sockaddr_in6 local;
    memset(&local, 0, sizeof(local));
    local.sin6_family = AF_INET6;
    local.sin6_port = htons(port);
    if (inet_pton(AF_INET6, ip, &local.sin6_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind(IPv6)");
        close(fd);
        return -1;
    }
    return fd;
}
#endif

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

    int fd = bind_udp_any(port);
    if (fd < 0) {
        return -1;
    }

    loop->fds[0] = fd;
    loop->fd_count = 1;
    /* local_addrs[0].family stays 0 = wildcard (matches any transmit src) */

    return 0;
}

int nano_run_loop_auto_candidates(nano_run_loop_t *loop, nanortc_t *rtc, uint16_t port)
{
    if (!loop || !rtc) {
        return -1;
    }

    memset(loop, 0, sizeof(*loop));
    loop->rtc = rtc;
    loop->port = port;
    loop->running = 0;
    for (int i = 0; i < NANORTC_MAX_LOCAL_CANDIDATES; i++) {
        loop->fds[i] = -1;
    }

    struct ifaddrs *ifas, *ifa;
    if (getifaddrs(&ifas) != 0) {
        perror("getifaddrs");
        /* Fall back to wildcard bind */
        int fd = bind_udp_any(port);
        if (fd < 0)
            return -1;
        loop->fds[0] = fd;
        loop->fd_count = 1;
        return 0;
    }

    uint8_t count = 0;
    for (ifa = ifas; ifa && count < NANORTC_MAX_LOCAL_CANDIDATES; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sa->sin_addr, ip_str, sizeof(ip_str));

            int fd = bind_udp_ipv4(ip_str, port);
            if (fd < 0)
                continue;

            loop->fds[count] = fd;
            loop->local_addrs[count].family = 4;
            memcpy(loop->local_addrs[count].addr, &sa->sin_addr, 4);
            loop->local_addrs[count].port = port;

            nanortc_add_local_candidate(rtc, ip_str, port);
            count++;
            continue;
        }

#if NANORTC_FEATURE_IPV6
        if (ifa->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            /* Skip link-local (needs scope_id, unsupported by ICE wire format)
             * and multicast; loopback already filtered above by IFF_LOOPBACK. */
            if (IN6_IS_ADDR_LINKLOCAL(&sa6->sin6_addr))
                continue;
            if (IN6_IS_ADDR_MULTICAST(&sa6->sin6_addr))
                continue;
            if (IN6_IS_ADDR_UNSPECIFIED(&sa6->sin6_addr))
                continue;

            char ip_str[INET6_ADDRSTRLEN];
            if (!inet_ntop(AF_INET6, &sa6->sin6_addr, ip_str, sizeof(ip_str)))
                continue;

            int fd = bind_udp_ipv6(ip_str, port);
            if (fd < 0)
                continue;

            loop->fds[count] = fd;
            loop->local_addrs[count].family = 6;
            memcpy(loop->local_addrs[count].addr, &sa6->sin6_addr, 16);
            loop->local_addrs[count].port = port;

            nanortc_add_local_candidate(rtc, ip_str, port);
            count++;
            continue;
        }
#endif
    }
    freeifaddrs(ifas);

    if (count == 0) {
        /* No interfaces found — fall back to wildcard */
        int fd = bind_udp_any(port);
        if (fd < 0)
            return -1;
        loop->fds[0] = fd;
        loop->fd_count = 1;
        return 0;
    }

    loop->fd_count = count;
    return 0;
}

void nano_run_loop_set_event_cb(nano_run_loop_t *loop, nano_event_cb cb, void *userdata)
{
    if (loop) {
        loop->event_cb = cb;
        loop->event_userdata = userdata;
    }
}

/* Find the fd that matches a transmit source address.
 * Returns the matching fd, or fds[0] as fallback. */
static int find_fd_for_src(nano_run_loop_t *loop, const nanortc_addr_t *src)
{
    if (src->family == 0 || loop->fd_count <= 1) {
        return loop->fds[0];
    }
    for (uint8_t i = 0; i < loop->fd_count; i++) {
        if (loop->local_addrs[i].family == src->family &&
            memcmp(loop->local_addrs[i].addr, src->addr, src->family == 4 ? 4u : 16u) == 0) {
            return loop->fds[i];
        }
    }
    return loop->fds[0]; /* fallback */
}

static void dispatch_outputs(nano_run_loop_t *loop, uint32_t *timeout_ms)
{
    nanortc_output_t out;

    while (nanortc_poll_output(loop->rtc, &out) == NANORTC_OK) {
        switch (out.type) {
        case NANORTC_OUTPUT_TRANSMIT: {
            int fd = find_fd_for_src(loop, &out.transmit.src);
#if NANORTC_FEATURE_IPV6
            if (out.transmit.dest.family == 6) {
                struct sockaddr_in6 dest6;
                memset(&dest6, 0, sizeof(dest6));
                dest6.sin6_family = AF_INET6;
                memcpy(&dest6.sin6_addr, out.transmit.dest.addr, 16);
                dest6.sin6_port = htons(out.transmit.dest.port);
                sendto(fd, out.transmit.data, out.transmit.len, 0, (struct sockaddr *)&dest6,
                       sizeof(dest6));
            } else {
                /* IPv4: wrap as IPv4-mapped IPv6 for dual-stack socket */
                struct sockaddr_in6 dest6;
                memset(&dest6, 0, sizeof(dest6));
                dest6.sin6_family = AF_INET6;
                dest6.sin6_addr.s6_addr[10] = 0xff;
                dest6.sin6_addr.s6_addr[11] = 0xff;
                memcpy(&dest6.sin6_addr.s6_addr[12], out.transmit.dest.addr, 4);
                dest6.sin6_port = htons(out.transmit.dest.port);
                sendto(fd, out.transmit.data, out.transmit.len, 0, (struct sockaddr *)&dest6,
                       sizeof(dest6));
            }
#else
            {
                struct sockaddr_in dest;
                memset(&dest, 0, sizeof(dest));
                dest.sin_family = AF_INET;
                memcpy(&dest.sin_addr, out.transmit.dest.addr, 4);
                dest.sin_port = htons(out.transmit.dest.port);
                sendto(fd, out.transmit.data, out.transmit.len, 0, (struct sockaddr *)&dest,
                       sizeof(dest));
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

    /* Caller's preferred poll cadence is the upper bound (default 100 ms,
     * which also bounds cooperative-shutdown latency for callers that
     * tear the loop down by flipping `peer->running`). The library's
     * next protocol deadline and any legacy NANORTC_OUTPUT_TIMEOUT
     * entries can only *shorten* this — never extend it. */
    uint32_t timeout_ms = loop->max_poll_ms ? loop->max_poll_ms : 100;

    uint32_t deadline_ms = 0;
    if (nanortc_next_timeout_ms(loop->rtc, nano_get_millis(), &deadline_ms) == NANORTC_OK &&
        deadline_ms < timeout_ms) {
        timeout_ms = deadline_ms;
    }

    dispatch_outputs(loop, &timeout_ms);

    /* Wait for network input or timeout */
    fd_set rset;
    FD_ZERO(&rset);
    int max_fd = -1;
    for (uint8_t i = 0; i < loop->fd_count; i++) {
        if (loop->fds[i] >= 0) {
            FD_SET(loop->fds[i], &rset);
            if (loop->fds[i] > max_fd)
                max_fd = loop->fds[i];
        }
    }

    if (max_fd < 0) {
        return -1;
    }

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };

    int ret = select(max_fd + 1, &rset, NULL, NULL, &tv);
    uint32_t now = nano_get_millis();

    if (ret > 0) {
        for (uint8_t i = 0; i < loop->fd_count; i++) {
            if (loop->fds[i] < 0 || !FD_ISSET(loop->fds[i], &rset))
                continue;

            uint8_t buf[1500];
#if NANORTC_FEATURE_IPV6
            struct sockaddr_in6 from6;
            socklen_t fromlen = sizeof(from6);
            ssize_t n =
                recvfrom(loop->fds[i], buf, sizeof(buf), 0, (struct sockaddr *)&from6, &fromlen);
            if (n > 0) {
                nanortc_input_t in = {.now_ms = now, .data = buf, .len = (size_t)n};
                const uint8_t *a = from6.sin6_addr.s6_addr;
                if (from6.sin6_family == AF_INET6 && a[0] == 0 && a[1] == 0 && a[2] == 0 &&
                    a[3] == 0 && a[4] == 0 && a[5] == 0 && a[6] == 0 && a[7] == 0 && a[8] == 0 &&
                    a[9] == 0 && a[10] == 0xff && a[11] == 0xff) {
                    in.src.family = 4;
                    memcpy(in.src.addr, &a[12], 4);
                } else {
                    in.src.family = 6;
                    memcpy(in.src.addr, a, 16);
                }
                in.src.port = ntohs(from6.sin6_port);
                /* dst = the local socket the packet arrived on. With one
                 * socket, local_addrs[0] is wildcard (family==0) and ICE
                 * falls back to selected_local_idx=0; with auto_candidates,
                 * each socket has its specific bound IP and dst lets ICE
                 * record the right candidate type on USE-CANDIDATE. */
                if (loop->local_addrs[i].family != 0) {
                    in.dst = loop->local_addrs[i];
                }
                nanortc_handle_input(loop->rtc, &in);
            }
#else
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t n =
                recvfrom(loop->fds[i], buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                nanortc_input_t in = {.now_ms = now, .data = buf, .len = (size_t)n};
                in.src.family = 4;
                memcpy(in.src.addr, &from.sin_addr, 4);
                in.src.port = ntohs(from.sin_port);
                if (loop->local_addrs[i].family != 0) {
                    in.dst = loop->local_addrs[i];
                }
                nanortc_handle_input(loop->rtc, &in);
            }
#endif
        }
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
