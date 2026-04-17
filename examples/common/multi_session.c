/*
 * nanortc examples — Multi-viewer WebRTC session pool implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "multi_session.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Per-session UDP send buffer. A 1080p (or larger) IDR frame can
 * generate 100+ RTP packets (~150 KB) in a single burst. */
#ifndef NANO_SESSION_UDP_SNDBUF
#define NANO_SESSION_UDP_SNDBUF (512 * 1024)
#endif

/* ----------------------------------------------------------------
 * IPv4 interface enumeration
 * ---------------------------------------------------------------- */

int nano_enumerate_local_ipv4(nano_local_ip_t *ips, int max_ips)
{
    if (!ips || max_ips <= 0)
        return 0;

    int count = 0;
    struct ifaddrs *ifas = NULL;

    if (getifaddrs(&ifas) == 0) {
        for (struct ifaddrs *ifa = ifas; ifa && count < max_ips; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;
            if (!(ifa->ifa_flags & IFF_UP))
                continue;

            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)))
                continue;

            /* De-duplicate */
            bool dup = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(ips[i].ip, ip) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;

            size_t len = strlen(ip);
            if (len >= sizeof(ips[0].ip))
                len = sizeof(ips[0].ip) - 1;
            memcpy(ips[count].ip, ip, len);
            ips[count].ip[len] = '\0';
            count++;
            fprintf(stderr, "[net] local IP: %s (%s)\n", ip, ifa->ifa_name);
        }
        freeifaddrs(ifas);
    } else {
        perror("getifaddrs");
    }

    if (count == 0) {
        memcpy(ips[0].ip, "127.0.0.1", 10);
        count = 1;
    }
    return count;
}

/* ----------------------------------------------------------------
 * Pool setup + lookup
 * ---------------------------------------------------------------- */

void nano_session_pool_init(nano_session_pool_t *pool, nano_session_t *storage, int max)
{
    if (!pool || !storage || max <= 0)
        return;
    pool->sessions = storage;
    pool->max_sessions = max;
    for (int i = 0; i < max; i++) {
        memset(&storage[i], 0, sizeof(storage[i]));
        storage[i].udp_fd = -1;
        storage[i].video_mid = -1;
        storage[i].audio_mid = -1;
        storage[i].viewer_id = -1;
        storage[i].pool = pool;
    }
}

nano_session_t *nano_session_find_free(nano_session_pool_t *pool)
{
    if (!pool)
        return NULL;
    for (int i = 0; i < pool->max_sessions; i++) {
        if (!pool->sessions[i].active)
            return &pool->sessions[i];
    }
    return NULL;
}

nano_session_t *nano_session_find_by_viewer(nano_session_pool_t *pool, int viewer_id)
{
    if (!pool)
        return NULL;
    for (int i = 0; i < pool->max_sessions; i++) {
        if (pool->sessions[i].active && pool->sessions[i].viewer_id == viewer_id)
            return &pool->sessions[i];
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Session lifecycle
 * ---------------------------------------------------------------- */

void nano_session_destroy(nano_session_t *s)
{
    if (!s)
        return;
    if (s->udp_fd >= 0) {
        close(s->udp_fd);
        s->udp_fd = -1;
    }
    nanortc_destroy(&s->rtc);
    s->active = 0;
    s->media_connected = 0;
    s->viewer_id = -1;
    s->video_mid = -1;
    s->audio_mid = -1;
}

int nano_session_create(nano_session_pool_t *pool, int viewer_id, const char *offer_sdp,
                        const nanortc_config_t *cfg, http_sig_t *sig)
{
    if (!pool || !offer_sdp || !cfg || !sig)
        return -1;

    nano_session_t *existing = nano_session_find_by_viewer(pool, viewer_id);
    if (existing) {
        fprintf(stderr, "[session] viewer %d reconnecting\n", viewer_id);
        nano_session_destroy(existing);
    }

    nano_session_t *s = nano_session_find_free(pool);
    if (!s) {
        fprintf(stderr, "[session] no free slots\n");
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->pool = pool;
    s->viewer_id = viewer_id;
    s->udp_fd = -1;
    s->video_mid = -1;
    s->audio_mid = -1;

    if (nanortc_init(&s->rtc, cfg) != NANORTC_OK)
        return -1;

    if (pool->track_setup) {
        if (pool->track_setup(s, pool->track_userdata) < 0) {
            nanortc_destroy(&s->rtc);
            return -1;
        }
    }

    s->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->udp_fd < 0) {
        nanortc_destroy(&s->rtc);
        return -1;
    }

    int reuse = 1;
    setsockopt(s->udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int sndbuf = NANO_SESSION_UDP_SNDBUF;
    setsockopt(s->udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY};
    if (bind(s->udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(s->udp_fd);
        s->udp_fd = -1;
        nanortc_destroy(&s->rtc);
        return -1;
    }

    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    getsockname(s->udp_fd, (struct sockaddr *)&bound, &blen);
    uint16_t port = ntohs(bound.sin_port);

    for (int i = 0; i < pool->local_ip_count; i++) {
        nanortc_add_local_candidate(&s->rtc, pool->local_ips[i].ip, port);
    }

    char answer[HTTP_SIG_BUF_SIZE];
    if (nanortc_accept_offer(&s->rtc, offer_sdp, answer, sizeof(answer), NULL) != NANORTC_OK ||
        http_sig_send_to(sig, viewer_id, "answer", answer, "sdp") < 0) {
        close(s->udp_fd);
        s->udp_fd = -1;
        nanortc_destroy(&s->rtc);
        return -1;
    }

    s->active = 1;
    fprintf(stderr, "[session] viewer %d ready (port %u, video_mid=%d, audio_mid=%d)\n", viewer_id,
            port, s->video_mid, s->audio_mid);
    return 0;
}

/* ----------------------------------------------------------------
 * Output dispatch
 * ---------------------------------------------------------------- */

static void handle_session_event(nano_session_t *s, const nanortc_event_t *evt)
{
    /* Built-in bookkeeping: track connection state automatically so
     * callers don't re-implement the state machine. */
    switch (evt->type) {
    case NANORTC_EV_CONNECTED:
        s->media_connected = 1;
        break;
    case NANORTC_EV_DISCONNECTED:
        s->media_connected = 0;
        s->active = 0; /* marked for cleanup by outer loop */
        break;
    default:
        break;
    }

    /* Delegate to the application callback for platform-specific
     * actions (e.g. force keyframe, log, update stats). */
    if (s->pool && s->pool->on_event) {
        s->pool->on_event(s, evt, s->pool->event_userdata);
    }
}

void nano_session_dispatch(nano_session_t *s, uint32_t *timeout_ms)
{
    if (!s || !s->active)
        return;

    nanortc_output_t out;
    while (nanortc_poll_output(&s->rtc, &out) == NANORTC_OK) {
        switch (out.type) {
        case NANORTC_OUTPUT_TRANSMIT: {
            if (s->udp_fd < 0)
                break;
            struct sockaddr_in dest = {.sin_family = AF_INET};
            memcpy(&dest.sin_addr, out.transmit.dest.addr, 4);
            dest.sin_port = htons(out.transmit.dest.port);
            sendto(s->udp_fd, out.transmit.data, out.transmit.len, 0, (struct sockaddr *)&dest,
                   sizeof(dest));
            break;
        }
        case NANORTC_OUTPUT_EVENT:
            handle_session_event(s, &out.event);
            break;
        case NANORTC_OUTPUT_TIMEOUT:
            if (timeout_ms && out.timeout_ms < *timeout_ms) {
                *timeout_ms = out.timeout_ms;
            }
            break;
        }
    }
}

/* ----------------------------------------------------------------
 * Pool-level helpers
 * ---------------------------------------------------------------- */

void nano_session_pool_add_fds(nano_session_pool_t *pool, fd_set *rset, int *maxfd)
{
    if (!pool || !rset || !maxfd)
        return;
    for (int i = 0; i < pool->max_sessions; i++) {
        nano_session_t *s = &pool->sessions[i];
        if (s->active && s->udp_fd >= 0) {
            FD_SET(s->udp_fd, rset);
            if (s->udp_fd > *maxfd)
                *maxfd = s->udp_fd;
        }
    }
}

void nano_session_pool_handle_udp(nano_session_pool_t *pool, int select_ret, const fd_set *rset,
                                  uint32_t now_ms)
{
    if (!pool)
        return;
    for (int i = 0; i < pool->max_sessions; i++) {
        nano_session_t *s = &pool->sessions[i];
        if (!s->active || s->udp_fd < 0)
            continue;

        if (select_ret > 0 && rset && FD_ISSET(s->udp_fd, rset)) {
            uint8_t buf[1500];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t n =
                recvfrom(s->udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                nanortc_addr_t src = {.family = 4, .port = ntohs(from.sin_port)};
                memcpy(src.addr, &from.sin_addr, 4);
                nanortc_input_t in = {
                    .now_ms = now_ms, .data = buf, .len = (size_t)n, .src = src};
                nanortc_handle_input(&s->rtc, &in);
            }
        } else {
            /* Timer tick even without data. */
            nanortc_input_t tick = {.now_ms = now_ms};
            nanortc_handle_input(&s->rtc, &tick);
        }
    }
}

void nano_session_pool_dispatch_all(nano_session_pool_t *pool, uint32_t *timeout_ms)
{
    if (!pool)
        return;
    for (int i = 0; i < pool->max_sessions; i++) {
        if (pool->sessions[i].active) {
            nano_session_dispatch(&pool->sessions[i], timeout_ms);
        }
    }
}

void nano_session_pool_cleanup(nano_session_pool_t *pool)
{
    if (!pool)
        return;
    for (int i = 0; i < pool->max_sessions; i++) {
        nano_session_t *s = &pool->sessions[i];
        if (!s->active && s->udp_fd >= 0) {
            fprintf(stderr, "[session %d] cleanup\n", s->viewer_id);
            nano_session_destroy(s);
        }
    }
}
