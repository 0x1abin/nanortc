/*
 * rk3588_uvc_camera — V4L2 USB camera -> H.264 encoder -> browser via nanortc
 *
 * Captures from a USB UVC camera, encodes to H.264, and streams to one
 * or more browser viewers using the nanortc Sans I/O WebRTC engine.
 *
 * Typical workflow (signaling server runs on your dev machine):
 *   1. Dev machine:  python3 signaling_server.py --port 8765
 *   2. Device:       rk3588_uvc_camera            (auto-discovers signaling)
 *   3. Browser:      http://localhost:8765/
 *
 * The camera auto-discovers the signaling server on the LAN via UDP
 * broadcast (port 19730). Use -s host:port to override.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "http_signaling.h"
#include "capture.h"
#include "sig_discovery.h"
#include "frame_queue.h"
#include "sig_queue.h"
#include "session.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * Tunables
 * ---------------------------------------------------------------- */

#define DEFAULT_DEVICE      "/dev/video1"
#define DEFAULT_WIDTH       2592
#define DEFAULT_HEIGHT      1520
#define DEFAULT_FPS         30
#define DEFAULT_BITRATE     12000000
#define DEFAULT_KEYFRAME_S  2
#if defined(RK3588_CAPTURE_FFMPEG)
#define DEFAULT_ENCODER     "h264_rkmpp"
#else
#define DEFAULT_ENCODER     "mpph264enc"
#endif
#define DEFAULT_SIG_HOST    ""     /* empty = auto-discover on LAN */
#define DEFAULT_SIG_PORT    8765

#define SIG_POLL_TIMEOUT_MS 2000

uint32_t nano_get_millis(void); /* from run_loop_linux.c */

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static frame_queue_t g_fq;
static sig_queue_t g_sq;
static volatile sig_atomic_t g_quit;

/* Diagnostic counter — reset every stats interval */
static uint32_t g_idr_max_bytes;

static void on_encoded_video(void *ctx, const uint8_t *annex_b, size_t len,
                             uint32_t pts_ms, bool is_keyframe)
{
    (void)ctx;
    fq_push(&g_fq, annex_b, len, pts_ms, is_keyframe);
}

/* ----------------------------------------------------------------
 * Signaling thread
 * ---------------------------------------------------------------- */

static void *sig_thread_fn(void *arg)
{
    http_sig_t *sig = (http_sig_t *)arg;
    fprintf(stderr, "[sig-thread] started\n");

    while (!g_quit) {
        char type[32], payload[HTTP_SIG_BUF_SIZE];
        int from = -1;
        int rc = http_sig_recv_from(sig, type, sizeof(type), payload, sizeof(payload),
                                    &from, SIG_POLL_TIMEOUT_MS);
        if (rc == -2) continue;                    /* timeout */
        if (rc != 0) { if (!g_quit) sleep(1); continue; } /* error */
        if (from < 0) continue;

        if (strcmp(type, "offer") == 0) {
            fprintf(stderr, "[sig-thread] offer from viewer %d\n", from);
            sq_push(&g_sq, 0, from, payload);
        } else if (strcmp(type, "candidate") == 0) {
            sq_push(&g_sq, 1, from, payload);
        }
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Main event loop
 * ---------------------------------------------------------------- */

static void on_signal(int sig) { (void)sig; g_quit = 1; }

static void run_event_loop(http_sig_t *sig, const nanortc_config_t *cfg)
{
    int frame_pipe = g_fq.wake_pipe[0];
    int sig_pipe = g_sq.wake_pipe[0];
    uint32_t frame_count = 0, bytes_sent = 0;
    uint32_t last_stats_ms = nano_get_millis();

    while (!g_quit) {
        uint32_t timeout_ms = 5;

        /* Pre-select: drain outputs */
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (g_sessions[i].active) session_dispatch_outputs(&g_sessions[i], &timeout_ms);

        /* Build fd_set */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(frame_pipe, &rset);
        FD_SET(sig_pipe, &rset);
        int maxfd = frame_pipe > sig_pipe ? frame_pipe : sig_pipe;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].active && g_sessions[i].udp_fd >= 0) {
                FD_SET(g_sessions[i].udp_fd, &rset);
                if (g_sessions[i].udp_fd > maxfd) maxfd = g_sessions[i].udp_fd;
            }
        }

        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        int ret = select(maxfd + 1, &rset, NULL, NULL, &tv);
        uint32_t now = nano_get_millis();

        /* UDP recv */
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session_t *s = &g_sessions[i];
            if (!s->active || s->udp_fd < 0) continue;
            if (ret > 0 && FD_ISSET(s->udp_fd, &rset)) {
                uint8_t buf[1500];
                struct sockaddr_in from;
                socklen_t fromlen = sizeof(from);
                ssize_t n = recvfrom(s->udp_fd, buf, sizeof(buf), 0,
                                     (struct sockaddr *)&from, &fromlen);
                if (n > 0) {
                    nanortc_addr_t src = {.family = 4, .port = ntohs(from.sin_port)};
                    memcpy(src.addr, &from.sin_addr, 4);
                    nanortc_handle_input(&s->rtc, now, buf, (size_t)n, &src);
                }
            } else {
                nanortc_handle_input(&s->rtc, now, NULL, 0, NULL);
            }
        }

        /* Frame queue -> broadcast to connected viewers */
        if (ret > 0 && FD_ISSET(frame_pipe, &rset)) {
            char drain[64];
            ssize_t r = read(frame_pipe, drain, sizeof(drain));
            (void)r;
            queued_frame_t frame;
            while (fq_pop(&g_fq, &frame) == 0) {
                bool sent = false;
                for (int i = 0; i < MAX_SESSIONS; i++) {
                    session_t *s = &g_sessions[i];
                    if (!s->active || !s->media_connected || s->video_mid < 0) continue;
#if NANORTC_FEATURE_VIDEO
                    nanortc_send_video(&s->rtc, (uint8_t)s->video_mid,
                                       frame.pts_ms, frame.data, frame.len);
                    session_dispatch_outputs(s, &timeout_ms);
                    sent = true;
#endif
                }
                if (sent) {
                    frame_count++;
                    bytes_sent += (uint32_t)frame.len;
                    if (frame.is_keyframe && frame.len > g_idr_max_bytes)
                        g_idr_max_bytes = (uint32_t)frame.len;
                }
                free(frame.data);
            }
        }

        /* Signaling queue -> process offers/candidates */
        if (ret > 0 && FD_ISSET(sig_pipe, &rset)) {
            char drain[64];
            ssize_t r = read(sig_pipe, drain, sizeof(drain));
            (void)r;
            sig_msg_t msg;
            while (sq_pop(&g_sq, &msg) == 0) {
                if (msg.msg_type == 0) {
                    fprintf(stderr, "[sig] offer from viewer %d\n", msg.from);
                    session_create(msg.from, msg.payload, cfg, sig);
                } else {
                    session_t *s = session_find_by_viewer(msg.from);
                    if (s && msg.payload[0]) nanortc_add_remote_candidate(&s->rtc, msg.payload);
                }
            }
        }

        /* Post-select: drain again */
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (g_sessions[i].active) session_dispatch_outputs(&g_sessions[i], &timeout_ms);

        /* Session cleanup */
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session_t *s = &g_sessions[i];
            if (!s->active && s->udp_fd >= 0) {
                fprintf(stderr, "[session %d] cleanup\n", s->viewer_id);
                session_destroy(s);
            }
        }

        /* Stats — print diagnostics every 5 s. */
        if (now - last_stats_ms >= 5000) {
            uint32_t dt = now - last_stats_ms;
            uint32_t kbps = (uint32_t)(((uint64_t)bytes_sent * 8) / (dt ? dt : 1));
            int active = 0;
            for (int i = 0; i < MAX_SESSIONS; i++)
                if (g_sessions[i].active && g_sessions[i].media_connected) active++;
            fprintf(stderr, "[stats] %u frames ~%u kbps %d viewer(s)",
                    frame_count, kbps, active);
            if (g_pli_count || g_fq_drop_count || g_idr_max_bytes)
                fprintf(stderr, " | PLI=%u drop=%u IDR_max=%uKB",
                        g_pli_count, g_fq_drop_count, g_idr_max_bytes / 1024);
#if NANORTC_FEATURE_VIDEO
            /* Per-session RTP stats from first active session */
            for (int i = 0; i < MAX_SESSIONS; i++) {
                session_t *s = &g_sessions[i];
                if (!s->active || !s->media_connected || s->video_mid < 0) continue;
                nanortc_track_stats_t ts;
                if (nanortc_get_track_stats(&s->rtc, (uint8_t)s->video_mid, &ts) == NANORTC_OK) {
                    fprintf(stderr, " | rtp_sent=%u rtt=%ums bwe=%ukbps",
                            ts.packets_sent, ts.rtt_ms, ts.bitrate_bps / 1000);
                }
                break;
            }
#endif
            fprintf(stderr, "\n");
            frame_count = 0; bytes_sent = 0; last_stats_ms = now;
            g_pli_count = 0; g_fq_drop_count = 0; g_idr_max_bytes = 0;
        }
    }
}

/* ----------------------------------------------------------------
 * CLI
 * ---------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  -d DEV   V4L2 device      (default %s)\n", DEFAULT_DEVICE);
    fprintf(stderr, "  -W N     width             (default %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -H N     height            (default %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -f N     fps               (default %d)\n", DEFAULT_FPS);
    fprintf(stderr, "  -b N     bitrate (bps)     (default %d)\n", DEFAULT_BITRATE);
    fprintf(stderr, "  -e ENC   encoder element   (default %s)\n", DEFAULT_ENCODER);
    fprintf(stderr, "  -s H:P   signaling server  (default: auto-discover on LAN)\n");
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    const char *encoder = DEFAULT_ENCODER;
    int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
    int fps = DEFAULT_FPS, bitrate = DEFAULT_BITRATE;
    char sig_host[256] = DEFAULT_SIG_HOST;
    uint16_t sig_port = DEFAULT_SIG_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i+1 < argc)      device = argv[++i];
        else if (strcmp(argv[i], "-W") == 0 && i+1 < argc)  width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-H") == 0 && i+1 < argc)  height = atoi(argv[++i]);
        else if (strcmp(argv[i], "-f") == 0 && i+1 < argc)  fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc)  bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0 && i+1 < argc)  encoder = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) {
            i++;
            const char *colon = strrchr(argv[i], ':');
            if (colon) {
                size_t hlen = (size_t)(colon - argv[i]);
                if (hlen >= sizeof(sig_host)) hlen = sizeof(sig_host) - 1;
                memcpy(sig_host, argv[i], hlen);
                sig_host[hlen] = '\0';
                sig_port = (uint16_t)atoi(colon + 1);
            } else {
                size_t hlen = strlen(argv[i]);
                if (hlen >= sizeof(sig_host)) hlen = sizeof(sig_host) - 1;
                memcpy(sig_host, argv[i], hlen);
                sig_host[hlen] = '\0';
            }
        } else { usage(argv[0]); return strcmp(argv[i], "-h") == 0 ? 0 : 1; }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Auto-discover signaling server if -s not specified */
    if (sig_host[0] == '\0') {
        if (sig_discover(sig_host, sizeof(sig_host), &sig_port, 3) < 0) {
            fprintf(stderr, "error: no signaling server found. "
                    "Run signaling_server.py on your dev machine, "
                    "or specify -s host:port\n");
            return 1;
        }
    }

    if (fq_init(&g_fq) < 0) return 1;
    if (sq_init(&g_sq) < 0) { fq_destroy(&g_fq); return 1; }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        g_sessions[i].udp_fd = -1;
        g_sessions[i].video_mid = -1;
        g_sessions[i].viewer_id = -1;
    }

    enumerate_local_ipv4();

    capture_config_t cap_cfg = {
        .device = device, .width = width, .height = height, .fps = fps,
        .bitrate_bps = bitrate, .keyframe_interval_s = DEFAULT_KEYFRAME_S,
        .encoder = encoder, .callback = on_encoded_video,
    };
    if (capture_start(&cap_cfg) < 0) {
        sq_destroy(&g_sq); fq_destroy(&g_fq); return 1;
    }

    http_sig_t sig;
    if (http_sig_join_as_host(&sig, sig_host, sig_port) < 0) {
        capture_stop(); sq_destroy(&g_sq); fq_destroy(&g_fq); return 1;
    }

    nanortc_config_t rtc_cfg = NANORTC_CONFIG_DEFAULT();
#if defined(NANORTC_CRYPTO_OPENSSL)
    rtc_cfg.crypto = nanortc_crypto_openssl();
#else
    rtc_cfg.crypto = nanortc_crypto_mbedtls();
#endif

    fprintf(stderr, "rk3588_uvc_camera: %s %dx%d@%d %dbps enc=%s sig=%s:%u IPs=%d\n",
            device, width, height, fps, bitrate, encoder, sig_host, sig_port, g_local_ip_count);

    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, sig_thread_fn, &sig) != 0) {
        capture_stop(); http_sig_leave(&sig);
        sq_destroy(&g_sq); fq_destroy(&g_fq); return 1;
    }

    run_event_loop(&sig, &rtc_cfg);

    g_quit = 1;
    pthread_join(sig_tid, NULL);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].udp_fd >= 0) session_destroy(&g_sessions[i]);
    capture_stop();
    http_sig_leave(&sig);
    sq_destroy(&g_sq);
    fq_destroy(&g_fq);
    return 0;
}
