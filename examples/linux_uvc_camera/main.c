/*
 * linux_uvc_camera — V4L2 USB camera -> ffmpeg/GStreamer encoder -> browser
 *                    with BWE-driven adaptive bitrate (+ optional ALSA -> Opus)
 *
 * Captures from a USB UVC camera, encodes the video on whichever encoder
 * the host provides — software libx264, NVIDIA NVENC (h264/hevc_nvenc),
 * or Rockchip MPP (h264_rkmpp / GStreamer mpph264enc) — and streams it
 * to one or more browser viewers using the nanortc Sans I/O WebRTC engine.
 * The encoder is chosen at runtime with -e; the same binary covers every
 * backend. Microphone audio (ALSA -> Opus) is an optional add-on.
 *
 * The send bitrate adapts to the network: nanortc's bandwidth estimator
 * (REMB + TWCC loss) feeds a coordinator that retargets the encoder so
 * playback stays real-time as available bandwidth changes. Designed for
 * a Tailscale virtual LAN (the box advertises its 100.x host candidate).
 *
 * Typical workflow (signaling server runs on your dev machine, or the
 * camera box itself):
 *   1. python3 ../browser_interop/signaling_server.py --port 8765 \
 *          --www-dir .                      (serves index.html)
 *   2. linux_uvc_camera -s <box-ip>:8765    (or auto-discover on LAN)
 *   3. Browser (Chrome >=136 / Safari):  http://<box-ip>:8765/
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
#include "bwe_coordinator.h"

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

#define DEFAULT_DEVICE     "/dev/video0"
#define DEFAULT_WIDTH      1920
#define DEFAULT_HEIGHT     1080
#define DEFAULT_FPS        30
#define DEFAULT_BITRATE    5000000
#define DEFAULT_KEYFRAME_S 2
#if defined(LINUX_UVC_CAPTURE_GSTREAMER)
#define DEFAULT_ENCODER "mpph264enc" /* GStreamer element (Rockchip MPP) */
#else
#define DEFAULT_ENCODER \
    "libx264" /* ffmpeg: universal software default; -e hevc_nvenc / h264_rkmpp for HW */
#endif
#define DEFAULT_INPUT_FMT "mjpeg"
#define DEFAULT_SIG_HOST  "" /* empty = auto-discover on LAN */
#define DEFAULT_SIG_PORT  8765

#if LINUX_UVC_HAS_AUDIO
#define DEFAULT_AUDIO_DEVICE  "default" /* ALSA default PCM; override with -A */
#define DEFAULT_AUDIO_BITRATE 64000
#define DEFAULT_AUDIO_RATE    48000
#define DEFAULT_AUDIO_CHANS   2
#define DEFAULT_AUDIO_FRAMEMS 20
#endif

/* BWE / adaptive-bitrate envelope. The BWE dynamically tracks the path
 * within [min, max]; these are just the bound defaults (override -m / -M).
 * -b sets the encoder start rate + BWE seed inside that envelope. Raise the
 * ceiling for 4K (1080p needs ~10 Mbps; 4K wants 20-30 Mbps of headroom). */
#define BWE_MIN_BPS        1000000  /* default floor (override: -m), 1 Mbps */
#define BWE_MAX_BPS        20000000 /* default ceiling (override: -M), 20 Mbps */
#define BWE_APPLY_INTERVAL 1000     /* ms between encoder retargets */
#define BWE_APPLY_DAMPEN   5        /* % dead-band around the applied rate */

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

static bool g_codec_is_h265;                  /* derived from the -e encoder name */
static int g_start_bitrate = DEFAULT_BITRATE; /* runtime -b: encoder start + BWE seed */
static int g_bwe_min_bps = BWE_MIN_BPS;       /* runtime -m: BWE lower bound */
static int g_bwe_max_bps = BWE_MAX_BPS;       /* runtime -M: BWE upper bound (raise for 4K) */
static bwe_coordinator_t g_bwe;               /* BWE -> encoder apply throttle */
static uint32_t g_pli_count;                  /* diagnostic counter */

/* ----------------------------------------------------------------
 * BWE -> encoder adaptive loop
 *
 * One global hardware encoder is broadcast to every viewer, so the
 * slowest link governs: target = min(estimate_i) over connected
 * sessions, fed through the shared rate-limit + dead-band coordinator.
 * ---------------------------------------------------------------- */

static int uvc_bwe_apply(uint32_t new_bps, void *ctx)
{
    (void)ctx;
    return capture_set_bitrate((int)new_bps); /* ffmpeg backend takes bps */
}

static void recompute_and_apply_bwe(uint32_t now_ms)
{
    uint32_t min_bps = 0;
    int contributors = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        nano_session_t *s = &g_sessions[i];
        if (!s->active || !s->media_connected || s->video_mid < 0)
            continue;
        uint32_t est = nanortc_get_estimated_bitrate(&s->rtc);
        if (est == 0)
            continue; /* no feedback yet — skip, never floor to 0 */
        if (min_bps == 0 || est < min_bps)
            min_bps = est;
        contributors++;
    }
    bwe_coordinator_try_apply(&g_bwe, min_bps, contributors, now_ms);
}

static int active_viewer_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].active && g_sessions[i].media_connected)
            n++;
    return n;
}

/* ----------------------------------------------------------------
 * Session hooks (track setup + event callback)
 * ---------------------------------------------------------------- */

/* Register local media tracks in the fixed order:
 *     1. video   (mid 0)
 *     2. audio   (mid 1, optional)
 * The browser adds transceivers in the same order (see index.html);
 * nanortc matches local pre-added tracks to offered m-lines by integer
 * mid, so the order is centralised here. */
static int uvc_track_setup(nano_session_t *s, void *userdata)
{
    (void)userdata;
#if NANORTC_FEATURE_VIDEO
    nanortc_codec_t codec = g_codec_is_h265 ? NANORTC_CODEC_H265 : NANORTC_CODEC_H264;
    s->video_mid = nanortc_add_video_track(&s->rtc, NANORTC_DIR_SENDONLY, codec);
    if (s->video_mid < 0)
        return -1;

#if NANORTC_FEATURE_H265
    if (g_codec_is_h265) {
        const uint8_t *vps, *sps, *pps;
        size_t vl, sl, pl;
        if (capture_get_h265_parameter_sets(&vps, &vl, &sps, &sl, &pps, &pl) == 0) {
            int rc = nanortc_video_set_h265_parameter_sets(&s->rtc, (uint8_t)s->video_mid, vps, vl,
                                                           sps, sl, pps, pl);
            if (rc != NANORTC_OK)
                fprintf(stderr, "[session] set_h265_parameter_sets rc=%d\n", rc);
        } else {
            fprintf(stderr, "[session] H265 parameter sets not ready — SDP uses default level-id "
                            "(viewer may drop frames)\n");
        }
    }
#endif

    /* Per-session adaptive-bitrate envelope. Must be set before accept_offer.
     * Seed the BWE with the runtime -b start rate so the coordinator does not
     * immediately override the encoder back up to a hardcoded default. */
    nanortc_set_bitrate_bounds(&s->rtc, (uint32_t)g_bwe_min_bps, (uint32_t)g_bwe_max_bps);
    nanortc_set_initial_bitrate(&s->rtc, (uint32_t)g_start_bitrate);
#endif

#if LINUX_UVC_HAS_AUDIO && NANORTC_FEATURE_AUDIO
    if (media_pipeline_audio_enabled(&g_mp)) {
        s->audio_mid =
            nanortc_add_audio_track(&s->rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
        if (s->audio_mid < 0) {
            fprintf(stderr, "[session] add_audio_track failed (%d), continuing video-only\n",
                    s->audio_mid);
            s->audio_mid = -1;
        }
    }
#endif
    return 0;
}

static void uvc_on_event(nano_session_t *s, const nanortc_event_t *evt, void *userdata)
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
        /* Rate-limit per session: at most one forced keyframe every 2 s. */
        uint32_t now_kf = nano_get_millis();
        g_pli_count++;
        if (now_kf - s->last_kf_ms >= 2000) {
            s->last_kf_ms = now_kf;
            capture_force_keyframe();
            fprintf(stderr, "[session %d] PLI -> forced keyframe\n", s->viewer_id);
        }
        break;
    }
#if NANORTC_FEATURE_VIDEO
    case NANORTC_EV_BITRATE_ESTIMATE:
        fprintf(stderr, "[session %d] BWE %s via %s: %u kbps (was %u kbps)\n", s->viewer_id,
                bwe_dir_str(evt->bitrate_estimate.direction),
                bwe_src_str(evt->bitrate_estimate.source), evt->bitrate_estimate.bitrate_bps / 1000,
                evt->bitrate_estimate.prev_bitrate_bps / 1000);
        recompute_and_apply_bwe(nano_get_millis());
        break;
#endif
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
        int rc = http_sig_recv_from(sig, type, sizeof(type), payload, sizeof(payload), &from,
                                    SIG_POLL_TIMEOUT_MS);
        if (rc == -2)
            continue;
        if (rc != 0) {
            if (!g_quit)
                sleep(1);
            continue;
        }
        if (from < 0)
            continue;

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

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void run_event_loop(http_sig_t *sig, const nanortc_config_t *cfg)
{
    int sig_pipe = g_sq.wake_pipe[0];
    uint32_t last_stats_ms = nano_get_millis();
    uint32_t last_bwe_ms = last_stats_ms;
    int prev_active = 0;

    while (!g_quit) {
        uint32_t timeout_ms = 5;

        nano_session_pool_dispatch_all(&g_pool, &timeout_ms);

        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(sig_pipe, &rset);
        int maxfd = sig_pipe;
        media_pipeline_add_fds(&g_mp, &rset, &maxfd);
        nano_session_pool_add_fds(&g_pool, &rset, &maxfd);

        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        int ret = select(maxfd + 1, &rset, NULL, NULL, &tv);
        uint32_t now = nano_get_millis();

        nano_session_pool_handle_udp(&g_pool, ret, &rset, now);

        if (ret > 0)
            media_pipeline_drain_to_sessions(&g_mp, &rset, g_sessions, MAX_SESSIONS, &timeout_ms);

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
                    if (s && msg.payload[0])
                        nanortc_add_remote_candidate(&s->rtc, msg.payload);
                }
            }
        }

        nano_session_pool_dispatch_all(&g_pool, &timeout_ms);
        nano_session_pool_cleanup(&g_pool);

        /* Periodic BWE retarget (slow-drift safety net beside the
         * event-driven path in uvc_on_event). Reset the coordinator
         * when all viewers leave so a fresh viewer is not dampen-rejected
         * against a stale low rate. */
        int active = active_viewer_count();
        if (active == 0 && prev_active > 0)
            bwe_coordinator_reset(&g_bwe);
        prev_active = active;
        if (now - last_bwe_ms >= BWE_APPLY_INTERVAL) {
            recompute_and_apply_bwe(now);
            last_bwe_ms = now;
        }

        /* Stats every 5 s. */
        if (now - last_stats_ms >= 5000) {
            uint32_t dt = now - last_stats_ms;
            uint32_t frame_count = media_pipeline_take_frame_count(&g_mp);
            uint32_t bytes_sent = media_pipeline_take_bytes_sent(&g_mp);
            uint32_t drops = media_pipeline_take_video_drops(&g_mp);
            uint32_t idr_max = media_pipeline_take_idr_max_bytes(&g_mp);
            uint32_t kbps = (uint32_t)(((uint64_t)bytes_sent * 8) / (dt ? dt : 1));
            fprintf(stderr, "[stats] %u frames ~%u kbps %d viewer(s)", frame_count, kbps, active);
            if (g_pli_count || drops || idr_max)
                fprintf(stderr, " | PLI=%u drop=%u IDR_max=%uKB", g_pli_count, drops,
                        idr_max / 1024);
#if NANORTC_FEATURE_VIDEO
            for (int i = 0; i < MAX_SESSIONS; i++) {
                nano_session_t *s = &g_sessions[i];
                if (!s->active || !s->media_connected || s->video_mid < 0)
                    continue;
                nanortc_track_stats_t ts;
                if (nanortc_get_track_stats(&s->rtc, (uint8_t)s->video_mid, &ts) == NANORTC_OK)
                    fprintf(stderr, " | rtp_sent=%u rtt=%ums bwe=%ukbps", ts.packets_sent,
                            ts.rtt_ms, ts.bitrate_bps / 1000);
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
    fprintf(stderr, "  -d DEV   V4L2 device        (default %s)\n", DEFAULT_DEVICE);
    fprintf(stderr, "  -W N     width              (default %d)\n", DEFAULT_WIDTH);
    fprintf(stderr, "  -H N     height             (default %d)\n", DEFAULT_HEIGHT);
    fprintf(stderr, "  -f N     fps                (default %d)\n", DEFAULT_FPS);
    fprintf(stderr, "  -b N     start bitrate bps  (default %d, seeds BWE)\n", DEFAULT_BITRATE);
    fprintf(stderr, "  -m N     BWE min bitrate bps (default %d)\n", BWE_MIN_BPS);
    fprintf(stderr, "  -M N     BWE max bitrate bps (default %d; raise for 4K, e.g. 25000000)\n",
            BWE_MAX_BPS);
    fprintf(stderr,
            "  -e ENC   encoder            (default %s; hevc_nvenc / h264_nvenc / h264_rkmpp)\n",
            DEFAULT_ENCODER);
    fprintf(stderr, "  -i FMT   v4l2 input format  (default %s; or yuyv422)\n", DEFAULT_INPUT_FMT);
    fprintf(stderr, "  -I IP    advertise only this host candidate (e.g. Tailscale 100.x)\n");
    fprintf(stderr, "  -s H:P   signaling server   (default: auto-discover on LAN)\n");
#if LINUX_UVC_HAS_AUDIO
    fprintf(stderr, "  -A DEV   ALSA PCM device    (default %s)\n", DEFAULT_AUDIO_DEVICE);
    fprintf(stderr, "  -R N     audio bitrate bps  (default %d)\n", DEFAULT_AUDIO_BITRATE);
#endif
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    const char *encoder = DEFAULT_ENCODER;
    const char *input_fmt = DEFAULT_INPUT_FMT;
    const char *bind_ip = NULL;
    int width = DEFAULT_WIDTH, height = DEFAULT_HEIGHT;
    int fps = DEFAULT_FPS, bitrate = DEFAULT_BITRATE;
    char sig_host[256] = DEFAULT_SIG_HOST;
    uint16_t sig_port = DEFAULT_SIG_PORT;
#if LINUX_UVC_HAS_AUDIO
    const char *audio_device = DEFAULT_AUDIO_DEVICE;
    int audio_bitrate = DEFAULT_AUDIO_BITRATE;
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            device = argv[++i];
        else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc)
            height = atoi(argv[++i]);
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
            fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            bitrate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc)
            g_bwe_min_bps = atoi(argv[++i]);
        else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc)
            g_bwe_max_bps = atoi(argv[++i]);
        else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc)
            encoder = argv[++i];
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            input_fmt = argv[++i];
        else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc)
            bind_ip = argv[++i];
#if LINUX_UVC_HAS_AUDIO
        else if (strcmp(argv[i], "-A") == 0 && i + 1 < argc)
            audio_device = argv[++i];
        else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc)
            audio_bitrate = atoi(argv[++i]);
#endif
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            nano_parse_host_port(argv[++i], sig_host, sizeof(sig_host), &sig_port);
        } else {
            usage(argv[0]);
            return strcmp(argv[i], "-h") == 0 ? 0 : 1;
        }
    }

    g_codec_is_h265 = (strstr(encoder, "hevc") != NULL) || (strstr(encoder, "265") != NULL);
    g_start_bitrate = bitrate;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (sig_host[0] == '\0') {
        if (sig_discover(sig_host, sizeof(sig_host), &sig_port, 3) < 0) {
            fprintf(stderr, "error: no signaling server found. "
                            "Run signaling_server.py, or specify -s host:port\n");
            return 1;
        }
    }

    if (sq_init(&g_sq) < 0)
        return 1;

    nano_session_pool_init(&g_pool, g_sessions, MAX_SESSIONS);

    /* Advertise host candidates. -I forces a single IP (e.g. the Tailscale
     * 100.x address) so the browser does not waste checks on unreachable
     * LAN candidates; otherwise enumerate every up interface. */
    int local_ip_count;
    if (bind_ip) {
        size_t n = strlen(bind_ip);
        if (n >= sizeof(g_local_ips[0].ip))
            n = sizeof(g_local_ips[0].ip) - 1;
        memcpy(g_local_ips[0].ip, bind_ip, n);
        g_local_ips[0].ip[n] = '\0';
        local_ip_count = 1;
        fprintf(stderr, "[net] advertising host candidate %s only\n", g_local_ips[0].ip);
    } else {
        local_ip_count = nano_enumerate_local_ipv4(g_local_ips, MAX_LOCAL_IPS);
    }
    g_pool.local_ips = g_local_ips;
    g_pool.local_ip_count = local_ip_count;
    g_pool.track_setup = uvc_track_setup;
    g_pool.on_event = uvc_on_event;

    bwe_coordinator_init(&g_bwe, BWE_APPLY_INTERVAL, BWE_APPLY_DAMPEN, uvc_bwe_apply, NULL);

    capture_config_t cap_cfg = {
        .device = device,
        .width = width,
        .height = height,
        .fps = fps,
        .bitrate_bps = bitrate,
        .keyframe_interval_s = DEFAULT_KEYFRAME_S,
        .encoder = encoder,
        .input_format = input_fmt,
    };
#if LINUX_UVC_HAS_AUDIO
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

    fprintf(stderr,
            "linux_uvc_camera: %s %dx%d@%d %dbps enc=%s(%s) in=%s BWE=[%d..%d] sig=%s:%u IPs=%d "
            "audio=%s\n",
            device, width, height, fps, bitrate, encoder, g_codec_is_h265 ? "H265" : "H264",
            input_fmt, g_bwe_min_bps, g_bwe_max_bps, sig_host, sig_port, g_pool.local_ip_count,
            media_pipeline_audio_enabled(&g_mp) ? "on" : "off");

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
        if (g_sessions[i].udp_fd >= 0)
            nano_session_destroy(&g_sessions[i]);
    media_pipeline_shutdown(&g_mp);
    http_sig_leave(&sig);
    sq_destroy(&g_sq);
    return 0;
}
