/*
 * session.c — Per-viewer WebRTC session management
 *
 * SPDX-License-Identifier: MIT
 */

#include "session.h"
#include "capture.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

uint32_t nano_get_millis(void); /* from run_loop_linux.c */

/* ----------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------- */

local_ip_t g_local_ips[MAX_LOCAL_IPS];
int g_local_ip_count;

session_t g_sessions[MAX_SESSIONS];

uint32_t g_pli_count;

/* ----------------------------------------------------------------
 * Local interface enumeration (IPv4)
 * ---------------------------------------------------------------- */

void enumerate_local_ipv4(void)
{
    struct ifaddrs *ifas, *ifa;
    g_local_ip_count = 0;

    if (getifaddrs(&ifas) != 0) { perror("getifaddrs"); return; }

    for (ifa = ifas; ifa && g_local_ip_count < MAX_LOCAL_IPS; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) continue;

        bool dup = false;
        for (int i = 0; i < g_local_ip_count; i++)
            if (strcmp(g_local_ips[i].ip, ip) == 0) { dup = true; break; }
        if (dup) continue;

        size_t len = strlen(ip);
        if (len >= sizeof(g_local_ips[0].ip)) len = sizeof(g_local_ips[0].ip) - 1;
        memcpy(g_local_ips[g_local_ip_count].ip, ip, len);
        g_local_ips[g_local_ip_count].ip[len] = '\0';
        g_local_ip_count++;
        fprintf(stderr, "[net] local IP: %s (%s)\n", ip, ifa->ifa_name);
    }
    freeifaddrs(ifas);

    if (g_local_ip_count == 0) {
        memcpy(g_local_ips[0].ip, "127.0.0.1", 10);
        g_local_ip_count = 1;
    }
}

/* ----------------------------------------------------------------
 * Session lookup
 * ---------------------------------------------------------------- */

session_t *session_find_free(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!g_sessions[i].active) return &g_sessions[i];
    return NULL;
}

session_t *session_find_by_viewer(int viewer_id)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].active && g_sessions[i].viewer_id == viewer_id) return &g_sessions[i];
    return NULL;
}

/* ----------------------------------------------------------------
 * Session lifecycle
 * ---------------------------------------------------------------- */

void session_destroy(session_t *s)
{
    if (!s) return;
    if (s->udp_fd >= 0) { close(s->udp_fd); s->udp_fd = -1; }
    nanortc_destroy(&s->rtc);
    s->active = 0;
    s->media_connected = 0;
    s->viewer_id = -1;
    s->video_mid = -1;
}

int session_create(int viewer_id, const char *offer_sdp,
                   const nanortc_config_t *cfg, http_sig_t *sig)
{
    session_t *existing = session_find_by_viewer(viewer_id);
    if (existing) {
        fprintf(stderr, "[session] viewer %d reconnecting\n", viewer_id);
        session_destroy(existing);
    }

    session_t *s = session_find_free();
    if (!s) {
        fprintf(stderr, "[session] no free slots\n");
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->viewer_id = viewer_id;
    s->udp_fd = -1;
    s->video_mid = -1;

    if (nanortc_init(&s->rtc, cfg) != NANORTC_OK) return -1;

#if NANORTC_FEATURE_VIDEO
    s->video_mid = nanortc_add_video_track(&s->rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    if (s->video_mid < 0) { nanortc_destroy(&s->rtc); return -1; }
#endif

    s->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->udp_fd < 0) { nanortc_destroy(&s->rtc); return -1; }

    int reuse = 1;
    setsockopt(s->udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Enlarge send buffer for bursty IDR frame transmission.
     * A 1080p IDR can produce 100+ RTP packets (~150KB) in a burst. */
    int sndbuf = 512 * 1024;
    setsockopt(s->udp_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY};
    if (bind(s->udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(s->udp_fd); s->udp_fd = -1; nanortc_destroy(&s->rtc); return -1;
    }

    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    getsockname(s->udp_fd, (struct sockaddr *)&bound, &blen);
    uint16_t port = ntohs(bound.sin_port);

    for (int i = 0; i < g_local_ip_count; i++)
        nanortc_add_local_candidate(&s->rtc, g_local_ips[i].ip, port);

    char answer[HTTP_SIG_BUF_SIZE];
    size_t answer_len = 0;
    if (nanortc_accept_offer(&s->rtc, offer_sdp, answer, sizeof(answer), &answer_len) != NANORTC_OK ||
        http_sig_send_to(sig, viewer_id, "answer", answer, "sdp") < 0) {
        close(s->udp_fd); s->udp_fd = -1; nanortc_destroy(&s->rtc); return -1;
    }

    s->active = 1;
    fprintf(stderr, "[session] viewer %d ready (port %u)\n", viewer_id, port);
    return 0;
}

/* ----------------------------------------------------------------
 * Event handler
 * ---------------------------------------------------------------- */

static void on_session_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)rtc;
    session_t *s = (session_t *)userdata;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        fprintf(stderr, "[session %d] ICE state -> %u\n", s->viewer_id, evt->ice_state);
        break;
    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[session %d] connected\n", s->viewer_id);
        s->media_connected = 1;
        capture_force_keyframe();
        break;
    case NANORTC_EV_KEYFRAME_REQUEST: {
        /* Rate-limit: max once per 2 s to avoid disrupting the encoder */
        static uint32_t last_kf_ms;
        uint32_t now_kf = nano_get_millis();
        g_pli_count++;
        if (now_kf - last_kf_ms >= 2000) {
            last_kf_ms = now_kf;
            capture_force_keyframe();
            fprintf(stderr, "[session %d] PLI -> forced keyframe\n", s->viewer_id);
        }
        break;
    }
    case NANORTC_EV_DISCONNECTED:
        fprintf(stderr, "[session %d] disconnected\n", s->viewer_id);
        s->media_connected = 0;
        s->active = 0;
        break;
    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * Output dispatch
 * ---------------------------------------------------------------- */

void session_dispatch_outputs(session_t *s, uint32_t *timeout_ms)
{
    nanortc_output_t out;
    while (nanortc_poll_output(&s->rtc, &out) == NANORTC_OK) {
        switch (out.type) {
        case NANORTC_OUTPUT_TRANSMIT: {
            struct sockaddr_in dest = {.sin_family = AF_INET};
            memcpy(&dest.sin_addr, out.transmit.dest.addr, 4);
            dest.sin_port = htons(out.transmit.dest.port);
            sendto(s->udp_fd, out.transmit.data, out.transmit.len, 0,
                   (struct sockaddr *)&dest, sizeof(dest));
            break;
        }
        case NANORTC_OUTPUT_EVENT:
            on_session_event(&s->rtc, &out.event, s);
            break;
        case NANORTC_OUTPUT_TIMEOUT:
            if (out.timeout_ms < *timeout_ms) *timeout_ms = out.timeout_ms;
            break;
        }
    }
}
