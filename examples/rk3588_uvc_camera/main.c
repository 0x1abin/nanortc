/*
 * rk3588_uvc_camera — V4L2 USB camera -> H.264 encoder (+ ALSA -> Opus)
 *                     -> browser via nanortc
 *
 * Captures from a USB UVC camera, encodes to H.264, optionally
 * captures audio from an ALSA device, encodes to Opus, and streams
 * both to one or more browser viewers using the nanortc Sans I/O
 * WebRTC engine.
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
#include "cli_helpers.h"
#include "http_signaling.h"
#include "capture.h"
#include "sig_discovery.h"
#include "sig_queue.h"
#include "multi_session.h"
#include "media_pipeline.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
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
#define DEFAULT_WIDTH       3840
#define DEFAULT_HEIGHT      2160
#define DEFAULT_FPS         30
#define DEFAULT_BITRATE     20000000
#define DEFAULT_KEYFRAME_S  2
#if defined(RK3588_CAPTURE_FFMPEG)
#define DEFAULT_ENCODER     "h264_rkmpp"
#else
#define DEFAULT_ENCODER     "mpph264enc"
#endif
#define DEFAULT_SIG_HOST    ""     /* empty = auto-discover on LAN */
#define DEFAULT_SIG_PORT    8765

#if RK3588_HAS_AUDIO
#define DEFAULT_AUDIO_DEVICE  "plughw:CARD=U4K,DEV=0"
#define DEFAULT_AUDIO_BITRATE 64000
#define DEFAULT_AUDIO_RATE    48000
#define DEFAULT_AUDIO_CHANS   2
#define DEFAULT_AUDIO_FRAMEMS 20
#endif

#define SIG_POLL_TIMEOUT_MS 2000

#define MAX_SESSIONS  4
#define MAX_LOCAL_IPS 8

uint32_t nano_get_millis(void); /* from run_loop_linux.c */

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static media_pipeline_t g_mp;
static sig_queue_t g_sq;
static volatile sig_atomic_t g_quit;

static nano_session_t g_sessions[MAX_SESSIONS];
static nano_local_ip_t g_local_ips[MAX_LOCAL_IPS];
static nano_session_pool_t g_pool;
static uint32_t g_pli_count; /* rk3588-specific diagnostic counter */

/* ----------------------------------------------------------------
 * Session hooks (track setup + event callback)
 * ---------------------------------------------------------------- */

/* Register local media tracks in the fixed order:
 *
 *     1. video   (mid 0)
 *     2. audio   (mid 1, optional)
 *
 * The browser adds transceivers in the same order (see index.html).
 * nanortc matches local pre-added tracks to offered m-lines by
 * integer mid — the payload-type / name are only used for codec
 * negotiation, not for track identification. Reordering or skipping
 * video would silently mis-route packets, so we centralise the
 * order here. */
static int rk3588_track_setup(nano_session_t *s, void *userdata)
{
    (void)userdata;
#if NANORTC_FEATURE_VIDEO
    s->video_mid = nanortc_add_video_track(&s->rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
    if (s->video_mid < 0) return -1;
#endif

#if RK3588_HAS_AUDIO && NANORTC_FEATURE_AUDIO
    if (media_pipeline_audio_enabled(&g_mp)) {
        s->audio_mid = nanortc_add_audio_track(&s->rtc, NANORTC_DIR_SENDONLY,
                                                NANORTC_CODEC_OPUS, 48000, 2);
        if (s->audio_mid < 0) {
            fprintf(stderr, "[session] add_audio_track failed (%d), continuing video-only\n",
                    s->audio_mid);
            s->audio_mid = -1;
        }
    }
#endif
    return 0;
}

/* Per-session event callback: rk3588-specific keyframe handling +
 * PLI rate limiting. Pool has already updated active/media_connected. */
static void rk3588_on_event(nano_session_t *s, const nanortc_event_t *evt, void *userdata)
{
    (void)userdata;
    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        fprintf(stderr, "[session %d] ICE state -> %u\n", s->viewer_id, evt->ice_state);
        break;
    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[session %d] connected\n", s->viewer_id);
        capture_force_keyframe();
        break;
    case NANORTC_EV_KEYFRAME_REQUEST: {
        /* Rate-limit per session: max one forced keyframe every 2 s
         * to avoid disrupting the encoder. Per-session so one viewer's
         * packet loss doesn't starve another viewer's recovery. */
        uint32_t now_kf = nano_get_millis();
        g_pli_count++;
        if (now_kf - s->last_kf_ms >= 2000) {
            s->last_kf_ms = now_kf;
            capture_force_keyframe();
            fprintf(stderr, "[session %d] PLI -> forced keyframe\n", s->viewer_id);
        }
        break;
    }
    case NANORTC_EV_DISCONNECTED:
        fprintf(stderr, "[session %d] disconnected\n", s->viewer_id);
        break;
    default:
        break;
    }
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
    int sig_pipe = g_sq.wake_pipe[0];
    uint32_t last_stats_ms = nano_get_millis();

    while (!g_quit) {
        uint32_t timeout_ms = 5;

        /* Pre-select: drain outputs */
        nano_session_pool_dispatch_all(&g_pool, &timeout_ms);

        /* Build fd_set */
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sig_pipe, &rset);
        int maxfd = sig_pipe;
        media_pipeline_add_fds(&g_mp, &rset, &maxfd);
        nano_session_pool_add_fds(&g_pool, &rset, &maxfd);

        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        int ret = select(maxfd + 1, &rset, NULL, NULL, &tv);
        uint32_t now = nano_get_millis();

        /* UDP recv */
        nano_session_pool_handle_udp(&g_pool, ret, &rset, now);

        /* Video + audio queues -> broadcast to connected viewers */
        if (ret > 0)
            media_pipeline_drain_to_sessions(&g_mp, &rset, g_sessions, MAX_SESSIONS, &timeout_ms);

        /* Signaling queue -> process offers/candidates */
        if (ret > 0 && FD_ISSET(sig_pipe, &rset)) {
            char drain[64];
            ssize_t r = read(sig_pipe, drain, sizeof(drain));
            (void)r;
            sig_msg_t msg;
            while (sq_pop(&g_sq, &msg) == 0) {
                if (msg.msg_type == 0) {
                    fprintf(stderr, "[sig] offer from viewer %d\n", msg.from);
                    nano_session_create(&g_pool, msg.from, msg.payload, cfg, sig);
                } else {
                    nano_session_t *s = nano_session_find_by_viewer(&g_pool, msg.from);
                    if (s && msg.payload[0]) nanortc_add_remote_candidate(&s->rtc, msg.payload);
                }
            }
        }

        /* Post-select: drain again */
        nano_session_pool_dispatch_all(&g_pool, &timeout_ms);

        /* Session cleanup (any session whose active flag was cleared by
         * the event callback but still holds an fd). */
        nano_session_pool_cleanup(&g_pool);

        /* Stats — print diagnostics every 5 s. */
        if (now - last_stats_ms >= 5000) {
            uint32_t dt = now - last_stats_ms;
            uint32_t frame_count = media_pipeline_take_frame_count(&g_mp);
            uint32_t bytes_sent = media_pipeline_take_bytes_sent(&g_mp);
            uint32_t drops = media_pipeline_take_video_drops(&g_mp);
            uint32_t idr_max = media_pipeline_take_idr_max_bytes(&g_mp);
            uint32_t kbps = (uint32_t)(((uint64_t)bytes_sent * 8) / (dt ? dt : 1));
            int active = 0;
            for (int i = 0; i < MAX_SESSIONS; i++)
                if (g_sessions[i].active && g_sessions[i].media_connected) active++;
            fprintf(stderr, "[stats] %u frames ~%u kbps %d viewer(s)",
                    frame_count, kbps, active);
            if (g_pli_count || drops || idr_max)
                fprintf(stderr, " | PLI=%u drop=%u IDR_max=%uKB",
                        g_pli_count, drops, idr_max / 1024);
#if NANORTC_FEATURE_VIDEO
            /* Per-session RTP stats from first active session */
            for (int i = 0; i < MAX_SESSIONS; i++) {
                nano_session_t *s = &g_sessions[i];
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
            last_stats_ms = now;
            g_pli_count = 0;
        }
    }
}

/* ----------------------------------------------------------------
 * CLI
 * ---------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  -d DEV   V4L2 device       (default %s)\n", DEFAULT_DEVICE);
    fprintf(stderr, "  -W N     width              (default %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -H N     height             (default %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -f N     fps                (default %d)\n", DEFAULT_FPS);
    fprintf(stderr, "  -b N     video bitrate bps  (default %d)\n", DEFAULT_BITRATE);
    fprintf(stderr, "  -e ENC   encoder element    (default %s)\n", DEFAULT_ENCODER);
    fprintf(stderr, "  -s H:P   signaling server   (default: auto-discover on LAN)\n");
#if RK3588_HAS_AUDIO
    fprintf(stderr, "  -A DEV   ALSA PCM device    (default %s)\n", DEFAULT_AUDIO_DEVICE);
    fprintf(stderr, "  -R N     audio bitrate bps  (default %d)\n", DEFAULT_AUDIO_BITRATE);
#endif
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    const char *encoder = DEFAULT_ENCODER;
    int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
    int fps = DEFAULT_FPS, bitrate = DEFAULT_BITRATE;
    char sig_host[256] = DEFAULT_SIG_HOST;
    uint16_t sig_port = DEFAULT_SIG_PORT;
#if RK3588_HAS_AUDIO
    const char *audio_device = DEFAULT_AUDIO_DEVICE;
    int audio_bitrate = DEFAULT_AUDIO_BITRATE;
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i+1 < argc)      device = argv[++i];
        else if (strcmp(argv[i], "-W") == 0 && i+1 < argc)  width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-H") == 0 && i+1 < argc)  height = atoi(argv[++i]);
        else if (strcmp(argv[i], "-f") == 0 && i+1 < argc)  fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i+1 < argc)  bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0 && i+1 < argc)  encoder = argv[++i];
#if RK3588_HAS_AUDIO
        else if (strcmp(argv[i], "-A") == 0 && i+1 < argc)  audio_device = argv[++i];
        else if (strcmp(argv[i], "-R") == 0 && i+1 < argc)  audio_bitrate = atoi(argv[++i]);
#endif
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) {
            nano_parse_host_port(argv[++i], sig_host, sizeof(sig_host), &sig_port);
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

    if (sq_init(&g_sq) < 0) return 1;

    /* Initialise session pool (zeroes storage, sets pool back-pointers) */
    nano_session_pool_init(&g_pool, g_sessions, MAX_SESSIONS);

    int local_ip_count = nano_enumerate_local_ipv4(g_local_ips, MAX_LOCAL_IPS);
    g_pool.local_ips = g_local_ips;
    g_pool.local_ip_count = local_ip_count;
    g_pool.track_setup = rk3588_track_setup;
    g_pool.on_event = rk3588_on_event;

    capture_config_t cap_cfg = {
        .device = device, .width = width, .height = height, .fps = fps,
        .bitrate_bps = bitrate, .keyframe_interval_s = DEFAULT_KEYFRAME_S,
        .encoder = encoder,
    };
#if RK3588_HAS_AUDIO
    audio_config_t aud_cfg = {
        .device = audio_device,
        .sample_rate = DEFAULT_AUDIO_RATE,
        .channels = DEFAULT_AUDIO_CHANS,
        .frame_ms = DEFAULT_AUDIO_FRAMEMS,
        .bitrate_bps = audio_bitrate,
    };
    if (media_pipeline_init(&g_mp, &cap_cfg, &aud_cfg) < 0) {
        sq_destroy(&g_sq);
        return 1;
    }
#else
    if (media_pipeline_init(&g_mp, &cap_cfg) < 0) {
        sq_destroy(&g_sq);
        return 1;
    }
#endif

    http_sig_t sig;
    if (http_sig_join_as_host(&sig, sig_host, sig_port) < 0) {
        media_pipeline_shutdown(&g_mp);
        sq_destroy(&g_sq);
        return 1;
    }

    nanortc_config_t rtc_cfg = NANORTC_CONFIG_DEFAULT();
#if defined(NANORTC_CRYPTO_OPENSSL)
    rtc_cfg.crypto = nanortc_crypto_openssl();
#else
    rtc_cfg.crypto = nanortc_crypto_mbedtls();
#endif

    fprintf(stderr, "rk3588_uvc_camera: %s %dx%d@%d %dbps enc=%s sig=%s:%u IPs=%d audio=%s\n",
            device, width, height, fps, bitrate, encoder, sig_host, sig_port,
            g_pool.local_ip_count, media_pipeline_audio_enabled(&g_mp) ? "on" : "off");

    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, sig_thread_fn, &sig) != 0) {
        http_sig_leave(&sig);
        media_pipeline_shutdown(&g_mp);
        sq_destroy(&g_sq);
        return 1;
    }

    run_event_loop(&sig, &rtc_cfg);

    g_quit = 1;
    pthread_join(sig_tid, NULL);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].udp_fd >= 0) nano_session_destroy(&g_sessions[i]);
    media_pipeline_shutdown(&g_mp);
    http_sig_leave(&sig);
    sq_destroy(&g_sq);
    return 0;
}
