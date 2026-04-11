/*
 * rk3588_uvc_camera — V4L2 USB camera → H.264 encoder → browser via nanortc
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

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
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

#define DEFAULT_DEVICE      "/dev/video2"
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define DEFAULT_FPS         30
#define DEFAULT_BITRATE     8000000
#define DEFAULT_KEYFRAME_S  2
#if defined(RK3588_CAPTURE_FFMPEG)
#define DEFAULT_ENCODER     "h264_rkmpp"
#else
#define DEFAULT_ENCODER     "mpph264enc"
#endif
#define DEFAULT_SIG_HOST    ""     /* empty = auto-discover on LAN */
#define DEFAULT_SIG_PORT    8765

#define FRAME_QUEUE_SIZE    16
#define SIG_QUEUE_SIZE      8
#define MAX_SESSIONS        4
#define MAX_LOCAL_IPS       8
#define SIG_POLL_TIMEOUT_MS 2000

uint32_t nano_get_millis(void); /* from run_loop_linux.c */

/* Diagnostic counters — reset every stats interval */
static uint32_t g_pli_count;
static uint32_t g_fq_drop_count;
static uint32_t g_idr_max_bytes;

/* ----------------------------------------------------------------
 * Thread-safe frame queue (mutex + wake pipe)
 * ---------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t pts_ms;
    bool is_keyframe;
} queued_frame_t;

typedef struct {
    queued_frame_t frames[FRAME_QUEUE_SIZE];
    int head;
    int count;
    pthread_mutex_t lock;
    int wake_pipe[2];
} frame_queue_t;

static int fq_init(frame_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    if (pipe(q->wake_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    pthread_mutex_init(&q->lock, NULL);
    return 0;
}

static void fq_destroy(frame_queue_t *q)
{
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % FRAME_QUEUE_SIZE;
        free(q->frames[idx].data);
    }
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    close(q->wake_pipe[0]);
    close(q->wake_pipe[1]);
}

static void fq_push(frame_queue_t *q, const uint8_t *data, size_t len,
                    uint32_t pts_ms, bool is_keyframe)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == FRAME_QUEUE_SIZE) {
        free(q->frames[q->head].data);
        q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
        q->count--;
        g_fq_drop_count++;
    }
    int idx = (q->head + q->count) % FRAME_QUEUE_SIZE;
    q->frames[idx].data = malloc(len);
    if (q->frames[idx].data) {
        memcpy(q->frames[idx].data, data, len);
        q->frames[idx].len = len;
        q->frames[idx].pts_ms = pts_ms;
        q->frames[idx].is_keyframe = is_keyframe;
        q->count++;
    }
    pthread_mutex_unlock(&q->lock);

    char c = 'F';
    ssize_t w = write(q->wake_pipe[1], &c, 1);
    (void)w;
}

static int fq_pop(frame_queue_t *q, queued_frame_t *out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->frames[q->head];
    q->frames[q->head].data = NULL;
    q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* ----------------------------------------------------------------
 * Thread-safe signaling message queue
 *
 * The signaling thread pushes incoming offers/candidates here;
 * the main event loop pops and processes them without blocking.
 * ---------------------------------------------------------------- */

typedef struct {
    int msg_type; /* 0 = offer, 1 = candidate */
    int from;
    char payload[HTTP_SIG_BUF_SIZE];
} sig_msg_t;

typedef struct {
    sig_msg_t msgs[SIG_QUEUE_SIZE];
    int head;
    int count;
    pthread_mutex_t lock;
    int wake_pipe[2];
} sig_queue_t;

static int sq_init(sig_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    if (pipe(q->wake_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    pthread_mutex_init(&q->lock, NULL);
    return 0;
}

static void sq_destroy(sig_queue_t *q)
{
    pthread_mutex_destroy(&q->lock);
    close(q->wake_pipe[0]);
    close(q->wake_pipe[1]);
}

static void sq_push(sig_queue_t *q, int msg_type, int from, const char *payload)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == SIG_QUEUE_SIZE) {
        q->head = (q->head + 1) % SIG_QUEUE_SIZE;
        q->count--;
    }
    int idx = (q->head + q->count) % SIG_QUEUE_SIZE;
    q->msgs[idx].msg_type = msg_type;
    q->msgs[idx].from = from;
    size_t len = strlen(payload);
    if (len >= HTTP_SIG_BUF_SIZE) len = HTTP_SIG_BUF_SIZE - 1;
    memcpy(q->msgs[idx].payload, payload, len);
    q->msgs[idx].payload[len] = '\0';
    q->count++;
    pthread_mutex_unlock(&q->lock);

    char c = 'S';
    ssize_t w = write(q->wake_pipe[1], &c, 1);
    (void)w;
}

static int sq_pop(sig_queue_t *q, sig_msg_t *out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->msgs[q->head];
    q->head = (q->head + 1) % SIG_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* ----------------------------------------------------------------
 * Local interface enumeration (IPv4)
 * ---------------------------------------------------------------- */

typedef struct { char ip[INET_ADDRSTRLEN]; } local_ip_t;
static local_ip_t g_local_ips[MAX_LOCAL_IPS];
static int g_local_ip_count;

static void enumerate_local_ipv4(void)
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
 * Per-viewer session
 * ---------------------------------------------------------------- */

typedef struct {
    int active;
    int viewer_id;
    nanortc_t rtc;
    int udp_fd;
    int video_mid;
    int media_connected;
} session_t;

static session_t g_sessions[MAX_SESSIONS];

static session_t *find_free_session(void)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!g_sessions[i].active) return &g_sessions[i];
    return NULL;
}

static session_t *find_session_by_viewer(int viewer_id)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].active && g_sessions[i].viewer_id == viewer_id) return &g_sessions[i];
    return NULL;
}

static void destroy_session(session_t *s)
{
    if (!s) return;
    if (s->udp_fd >= 0) { close(s->udp_fd); s->udp_fd = -1; }
    nanortc_destroy(&s->rtc);
    s->active = 0;
    s->media_connected = 0;
    s->viewer_id = -1;
    s->video_mid = -1;
}

/* ----------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------- */

static frame_queue_t g_fq;
static sig_queue_t g_sq;
static volatile sig_atomic_t g_quit;

static void on_encoded_video(void *ctx, const uint8_t *annex_b, size_t len,
                             uint32_t pts_ms, bool is_keyframe)
{
    (void)ctx;
    fq_push(&g_fq, annex_b, len, pts_ms, is_keyframe);
}

/* ----------------------------------------------------------------
 * Per-session event handler
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
            fprintf(stderr, "[session %d] PLI → forced keyframe\n", s->viewer_id);
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
 * Session creation
 * ---------------------------------------------------------------- */

static int create_session(int viewer_id, const char *offer_sdp,
                          const nanortc_config_t *cfg, http_sig_t *sig)
{
    session_t *existing = find_session_by_viewer(viewer_id);
    if (existing) {
        fprintf(stderr, "[session] viewer %d reconnecting\n", viewer_id);
        destroy_session(existing);
    }

    session_t *s = find_free_session();
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
 * Output dispatch
 * ---------------------------------------------------------------- */

static void dispatch_outputs(session_t *s, uint32_t *timeout_ms)
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
            if (g_sessions[i].active) dispatch_outputs(&g_sessions[i], &timeout_ms);

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
                    dispatch_outputs(s, &timeout_ms);
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
                    create_session(msg.from, msg.payload, cfg, sig);
                } else {
                    session_t *s = find_session_by_viewer(msg.from);
                    if (s && msg.payload[0]) nanortc_add_remote_candidate(&s->rtc, msg.payload);
                }
            }
        }

        /* Post-select: drain again */
        for (int i = 0; i < MAX_SESSIONS; i++)
            if (g_sessions[i].active) dispatch_outputs(&g_sessions[i], &timeout_ms);

        /* Session cleanup */
        for (int i = 0; i < MAX_SESSIONS; i++) {
            session_t *s = &g_sessions[i];
            if (!s->active && s->udp_fd >= 0) {
                fprintf(stderr, "[session %d] cleanup\n", s->viewer_id);
                destroy_session(s);
            }
        }

        /* Stats — print diagnostics every 5 s.
         *
         * Troubleshooting guide:
         *   drop>0         Frame queue overflow — main loop too slow to drain.
         *                  Check if dispatch_outputs is blocked or encoder too fast.
         *   PLI>0          Browser requested keyframe — usually means packet loss
         *                  exceeded NACK recovery. Check network, increase bitrate
         *                  headroom, or shorten GOP (DEFAULT_KEYFRAME_S).
         *   kbps<<target   Encoder under-producing. Check encoder load, rc-mode,
         *                  or camera capture rate.
         *   IDR>200KB      Large keyframes risk burst loss. Increase
         *                  NANORTC_OUT_QUEUE_SIZE or reduce resolution/quality.
         *   RTT>100ms      High latency path. Consider TURN relay or closer server.
         *   BWE<bitrate    Network can't sustain target bitrate. Lower -b or
         *                  let the app adapt encoder to BWE (not yet implemented).
         */
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
        if (g_sessions[i].udp_fd >= 0) destroy_session(&g_sessions[i]);
    capture_stop();
    http_sig_leave(&sig);
    sq_destroy(&g_sq);
    fq_destroy(&g_fq);
    return 0;
}
