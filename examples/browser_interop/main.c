/*
 * nanortc browser_interop — DataChannel echo via HTTP signaling
 *
 * Tests nanortc ↔ browser WebRTC interoperability.
 * Supports both offerer (CONTROLLING) and answerer (CONTROLLED) roles.
 *
 * Usage:
 *   ./browser_interop [-p udp_port] [-s host:port] [--offer | --answer]
 *
 * Default: --answer mode, signaling at localhost:8765, UDP port 9999.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "http_signaling.h"
#include "media_source.h"
#include "h264_utils.h"
#include "ice_server_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>

typedef struct {
    int offer_mode;
    int media_connected; /* DTLS+SRTP ready for media */
    int audio_mid;
    int video_mid;
} app_ctx_t;

static nano_run_loop_t loop;
static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
    nano_run_loop_stop(&loop);
}

static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    app_ctx_t *ctx = (app_ctx_t *)userdata;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            fprintf(stderr, "[event] ICE connected\n");
        }
        break;

    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[event] Connected\n");
        ctx->media_connected = 1;
        /* Offerer must create the DataChannel after connection is ready */
        if (ctx->offer_mode) {
            int drc = nanortc_create_datachannel(rtc, "test", NULL);
            if (drc >= 0) {
                fprintf(stderr, "[event] Created DataChannel 'test'\n");
            } else {
                fprintf(stderr, "[event] Failed to create DataChannel: %d\n", drc);
            }
        }
        break;

    case NANORTC_EV_DATACHANNEL_OPEN:
        fprintf(stderr, "[event] DataChannel open (id=%d)\n", evt->datachannel_open.id);
        nanortc_datachannel_send_string(rtc, evt->datachannel_open.id, "hello");
        break;

    case NANORTC_EV_DATACHANNEL_DATA:
        if (evt->datachannel_data.binary) {
            fprintf(stderr, "[event] DC data (%zu bytes), echoing back\n",
                    evt->datachannel_data.len);
            nanortc_datachannel_send(rtc, evt->datachannel_data.id, evt->datachannel_data.data,
                                     evt->datachannel_data.len);
        } else {
            fprintf(stderr, "[event] DC string: %.*s\n", (int)evt->datachannel_data.len,
                    (char *)evt->datachannel_data.data);
            nanortc_datachannel_send_string(rtc, evt->datachannel_data.id,
                                            (const char *)evt->datachannel_data.data);
        }
        break;

    case NANORTC_EV_DATACHANNEL_CLOSE:
        fprintf(stderr, "[event] DataChannel closed\n");
        break;

    case NANORTC_EV_DISCONNECTED:
        fprintf(stderr, "[event] Disconnected\n");
        nano_run_loop_stop(&loop);
        break;

    default:
        break;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  -p PORT        UDP port (default: 9999)\n");
    fprintf(stderr, "  -b IP          Bind/candidate IP (default: auto-detect)\n");
    fprintf(stderr, "  -s HOST:PORT   Signaling server (default: localhost:8765)\n");
    fprintf(stderr, "  -a DIR         Opus frame directory for audio send\n");
    fprintf(stderr, "  -v DIR         H.264 frame directory for video send\n");
    fprintf(stderr, "  --turn-server IP:PORT  TURN relay server\n");
    fprintf(stderr, "  --turn-user USER       TURN username\n");
    fprintf(stderr, "  --turn-pass PASS       TURN password/credential\n");
    fprintf(stderr, "  --offer                Act as offerer (CONTROLLING)\n");
    fprintf(stderr, "  --answer               Act as answerer (CONTROLLED, default)\n");
}

/* ----------------------------------------------------------------
 * Signaling: answer mode (CONTROLLED)
 *   Wait for offer → generate answer → send answer
 * ---------------------------------------------------------------- */
static int do_answer_signaling(http_sig_t *sig, nanortc_t *rtc)
{
    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    fprintf(stderr, "Waiting for SDP offer...\n");
    while (!g_quit) {
        int rc = http_sig_recv(sig, type, sizeof(type), payload, sizeof(payload), 2000);
        if (rc == -2)
            continue; /* timeout, retry */
        if (rc < 0) {
            if (g_quit)
                return -1; /* interrupted by signal */
            fprintf(stderr, "Signaling error waiting for offer\n");
            return -1;
        }
        if (strcmp(type, "offer") == 0)
            break;
        fprintf(stderr, "[sig] Ignoring '%s' (waiting for offer)\n", type);
    }

    fprintf(stderr, "[sig] Got SDP offer (%zu bytes)\n", strlen(payload));

    char answer[HTTP_SIG_BUF_SIZE];
    size_t answer_len = 0;
    int rc = nanortc_accept_offer(rtc, payload, answer, sizeof(answer), &answer_len);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_accept_offer failed: %d\n", rc);
        return rc;
    }
    fprintf(stderr, "[sig] Generated SDP answer (%zu bytes)\n", answer_len);

    rc = http_sig_send(sig, "answer", answer, "sdp");
    if (rc < 0) {
        fprintf(stderr, "Failed to send SDP answer\n");
        return -1;
    }
    fprintf(stderr, "[sig] Sent SDP answer\n");
    return 0;
}

/* ----------------------------------------------------------------
 * Signaling: offer mode (CONTROLLING)
 *   Generate offer → send offer → wait for answer
 * ---------------------------------------------------------------- */
static int do_offer_signaling(http_sig_t *sig, nanortc_t *rtc)
{
    char offer[HTTP_SIG_BUF_SIZE];
    int rc = nanortc_create_offer(rtc, offer, sizeof(offer), NULL);
    if (rc < 0) {
        fprintf(stderr, "nanortc_create_offer failed: %d\n", rc);
        return rc;
    }

    rc = http_sig_send(sig, "offer", offer, "sdp");
    if (rc < 0) {
        fprintf(stderr, "Failed to send SDP offer\n");
        return -1;
    }
    fprintf(stderr, "[sig] Sent SDP offer\n");

    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    fprintf(stderr, "Waiting for SDP answer...\n");
    while (!g_quit) {
        rc = http_sig_recv(sig, type, sizeof(type), payload, sizeof(payload), 2000);
        if (rc == -2)
            continue;
        if (rc < 0) {
            fprintf(stderr, "Signaling error waiting for answer\n");
            return -1;
        }
        if (strcmp(type, "answer") == 0)
            break;
        /* Buffer any early ICE candidates */
        if (strcmp(type, "candidate") == 0 && payload[0] != '\0') {
            fprintf(stderr, "[sig] Early ICE candidate: %.60s...\n", payload);
            nanortc_add_remote_candidate(rtc, payload);
        }
    }

    fprintf(stderr, "[sig] Got SDP answer (%zu bytes)\n", strlen(payload));
    rc = nanortc_accept_answer(rtc, payload);
    if (rc < 0) {
        fprintf(stderr, "nanortc_accept_answer failed: %d\n", rc);
        return rc;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Trickle ICE: poll signaling for remote candidates periodically
 * ---------------------------------------------------------------- */
static uint32_t last_poll_ms;

static void poll_trickle_ice(http_sig_t *sig, nanortc_t *rtc)
{
    uint32_t now = nano_get_millis();
    if (now - last_poll_ms < 500)
        return; /* throttle to every 500ms */
    last_poll_ms = now;

    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    int rc = http_sig_recv(sig, type, sizeof(type), payload, sizeof(payload), 0);
    if (rc == 0) {
        if (strcmp(type, "candidate") == 0 && payload[0] != '\0') {
            fprintf(stderr, "[sig] Trickle ICE: %.60s...\n", payload);
            nanortc_add_remote_candidate(rtc, payload);
        } else if (strcmp(type, "candidate") == 0) {
            fprintf(stderr, "[sig] End-of-candidates\n");
        }
    }
}

int main(int argc, char *argv[])
{
    uint16_t port = 9999;
    char bind_ip[64] = "";
    char sig_host[256] = "localhost";
    uint16_t sig_port = 8765;
    int offer_mode = 0;
    const char *audio_dir = NULL;
    const char *video_dir = NULL;
    char turn_ip[64] = "";
    uint16_t turn_port = 3478;
    const char *turn_user = NULL;
    const char *turn_pass = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            size_t len = strlen(argv[++i]);
            if (len >= sizeof(bind_ip))
                len = sizeof(bind_ip) - 1;
            memcpy(bind_ip, argv[i], len);
            bind_ip[len] = '\0';
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            i++;
            char *colon = strrchr(argv[i], ':');
            if (colon) {
                size_t hlen = (size_t)(colon - argv[i]);
                if (hlen >= sizeof(sig_host))
                    hlen = sizeof(sig_host) - 1;
                memcpy(sig_host, argv[i], hlen);
                sig_host[hlen] = '\0';
                sig_port = (uint16_t)atoi(colon + 1);
            } else {
                size_t hlen = strlen(argv[i]);
                if (hlen >= sizeof(sig_host))
                    hlen = sizeof(sig_host) - 1;
                memcpy(sig_host, argv[i], hlen);
                sig_host[hlen] = '\0';
            }
        } else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            audio_dir = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            video_dir = argv[++i];
        } else if (strcmp(argv[i], "--turn-server") == 0 && i + 1 < argc) {
            i++;
            char *colon = strrchr(argv[i], ':');
            if (colon) {
                size_t iplen = (size_t)(colon - argv[i]);
                if (iplen < sizeof(turn_ip)) {
                    memcpy(turn_ip, argv[i], iplen);
                    turn_ip[iplen] = '\0';
                }
                turn_port = (uint16_t)atoi(colon + 1);
            } else {
                size_t len = strlen(argv[i]);
                if (len < sizeof(turn_ip)) {
                    memcpy(turn_ip, argv[i], len);
                    turn_ip[len] = '\0';
                }
            }
        } else if (strcmp(argv[i], "--turn-user") == 0 && i + 1 < argc) {
            turn_user = argv[++i];
        } else if (strcmp(argv[i], "--turn-pass") == 0 && i + 1 < argc) {
            turn_pass = argv[++i];
        } else if (strcmp(argv[i], "--offer") == 0) {
            offer_mode = 1;
        } else if (strcmp(argv[i], "--answer") == 0) {
            offer_mode = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    /* Default TURN: PeerJS public server (if no CLI override) */
    if (!turn_ip[0]) {
        memcpy(turn_ip, "eu-0.turn.peerjs.com", 21);
        turn_port = 3478;
    }
    if (!turn_user)
        turn_user = "peerjs";
    if (!turn_pass)
        turn_pass = "peerjsp";

    app_ctx_t app_ctx = {.offer_mode = offer_mode, .audio_mid = -1, .video_mid = -1};
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1. Join signaling server */
    http_sig_t sig;
    int rc = http_sig_join(&sig, sig_host, sig_port);
    if (rc < 0) {
        fprintf(stderr, "Failed to join signaling server %s:%u\n", sig_host, sig_port);
        return 1;
    }

    /* 2. Init nanortc */
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nanortc_crypto_openssl();
#else
    cfg.crypto = nanortc_crypto_mbedtls();
#endif
    cfg.role = offer_mode ? NANORTC_ROLE_CONTROLLING : NANORTC_ROLE_CONTROLLED;

    /* ICE servers (WebRTC-style, from CLI args) */
    static char turn_url[128];
    static const char *stun_url = "stun:stun.cloudflare.com:3478";
    static const char *turn_url_ptr;
    nanortc_ice_server_t ice_servers[2];
    static char ice_scratch[512];
    size_t ice_server_count = 0;

    ice_servers[0] = (nanortc_ice_server_t){.urls = &stun_url, .url_count = 1};
    ice_server_count = 1;

    if (turn_ip[0] && turn_user && turn_pass) {
        snprintf(turn_url, sizeof(turn_url), "turn:%s:%u", turn_ip, turn_port);
        turn_url_ptr = turn_url;
        ice_servers[1] = (nanortc_ice_server_t){
            .urls = &turn_url_ptr, .url_count = 1, .username = turn_user, .credential = turn_pass};
        ice_server_count = 2;
    }

    /* Resolve domain names to IPs */
    nano_resolve_ice_servers(ice_servers, ice_server_count, ice_scratch, sizeof(ice_scratch));
    cfg.ice_servers = ice_servers;
    cfg.ice_server_count = ice_server_count;

    if (turn_ip[0] && turn_user) {
        fprintf(stderr, "ICE servers: STUN + TURN %s user=%s\n",
                ice_server_count > 1 ? ice_servers[1].urls[0] : "(none)", turn_user);
    }

    rc = nanortc_init(&rtc, &cfg);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_init failed: %d\n", rc);
        http_sig_leave(&sig);
        return 1;
    }

#if NANORTC_FEATURE_AUDIO
    if (audio_dir) {
        app_ctx.audio_mid =
            nanortc_add_audio_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_OPUS, 48000, 2);
        if (app_ctx.audio_mid < 0)
            fprintf(stderr, "nanortc_add_audio_track failed: %d\n", app_ctx.audio_mid);
    }
#endif

#if NANORTC_FEATURE_VIDEO
    if (video_dir) {
        app_ctx.video_mid = nanortc_add_video_track(&rtc, NANORTC_DIR_SENDONLY, NANORTC_CODEC_H264);
        if (app_ctx.video_mid < 0)
            fprintf(stderr, "nanortc_add_video_track failed: %d\n", app_ctx.video_mid);
    }
#endif

    /* 3. Auto-detect IP if not specified */
    if (bind_ip[0] == '\0') {
        struct ifaddrs *ifas, *ifa;
        if (getifaddrs(&ifas) == 0) {
            for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                if (ifa->ifa_flags & IFF_LOOPBACK)
                    continue;
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sa->sin_addr, bind_ip, sizeof(bind_ip));
                break;
            }
            freeifaddrs(ifas);
        }
        if (bind_ip[0] == '\0') {
            memcpy(bind_ip, "127.0.0.1", 10);
        }
    }

    /* 4. Bind UDP on 0.0.0.0, register real IP as candidate */
    rc = nano_run_loop_init(&loop, &rtc, NULL, port);
    if (rc < 0) {
        fprintf(stderr, "Failed to bind UDP port %d\n", port);
        http_sig_leave(&sig);
        nanortc_destroy(&rtc);
        return 1;
    }
    /* Override 0.0.0.0 candidate with actual IP for SDP */
    nanortc_add_local_candidate(&rtc, bind_ip, port);
    nano_run_loop_set_event_cb(&loop, on_event, &app_ctx);

    /* Audio media source */
    nano_media_source_t audio_src;
    int has_audio_src = 0;
#if NANORTC_FEATURE_AUDIO
    if (audio_dir) {
        if (nano_media_source_init(&audio_src, NANORTC_MEDIA_OPUS, audio_dir) == 0) {
            has_audio_src = 1;
            fprintf(stderr, "Audio source: %s (Opus, 20ms frames)\n", audio_dir);
        } else {
            fprintf(stderr, "Warning: failed to init audio source from %s\n", audio_dir);
        }
    }
#endif

    /* Video media source */
    nano_media_source_t video_src;
    int has_video_src = 0;
#if NANORTC_FEATURE_VIDEO
    if (video_dir) {
        if (nano_media_source_init(&video_src, NANORTC_MEDIA_H264, video_dir) == 0) {
            has_video_src = 1;
            fprintf(stderr, "Video source: %s (H.264, 25fps)\n", video_dir);
        } else {
            fprintf(stderr, "Warning: failed to init video source from %s\n", video_dir);
        }
    }
#endif

    fprintf(stderr, "nanortc browser_interop (mode=%s, udp=%s:%d, sig=%s:%u)\n",
            offer_mode ? "offer" : "answer", bind_ip, port, sig_host, sig_port);

    /* 5. SDP exchange */
    if (offer_mode) {
        rc = do_offer_signaling(&sig, &rtc);
    } else {
        rc = do_answer_signaling(&sig, &rtc);
    }
    if (rc != 0) {
        goto cleanup;
    }

    /* 6. Event loop with trickle ICE polling + media send */
    fprintf(stderr, "Entering event loop...\n");
    uint32_t audio_epoch_ms = 0;    /* wall-clock start time for audio */
    uint32_t audio_frame_count = 0; /* frames sent since epoch */
    uint32_t video_epoch_ms = 0;    /* wall-clock start time for video */
    uint32_t video_frame_count = 0; /* frames sent since epoch */
    if (has_audio_src || has_video_src) {
        loop.max_poll_ms = 5; /* 5ms poll for smooth media pacing */
    }
    loop.running = 1;
    while (loop.running) {
        nano_run_loop_step(&loop);
        poll_trickle_ice(&sig, &rtc);

#if NANORTC_FEATURE_AUDIO
        /* Send audio frames at 20ms intervals (epoch-based for drift-free timing) */
        if (has_audio_src && app_ctx.media_connected) {
            uint32_t now = nano_get_millis();
            if (audio_epoch_ms == 0)
                audio_epoch_ms = now;
            uint32_t target_frames = (now - audio_epoch_ms) / 20;
            /* Prevent burst: if we fell behind by >2 frames, skip ahead */
            if (target_frames - audio_frame_count > 2) {
                audio_frame_count = target_frames - 1;
            }
            if (audio_frame_count < target_frames) {
                uint8_t frame_buf[1024];
                size_t frame_len = 0;
                uint32_t ts_ms = 0;
                if (nano_media_source_next_frame(&audio_src, frame_buf, sizeof(frame_buf),
                                                 &frame_len, &ts_ms) == 0) {
                    nanortc_send_audio(&rtc, (uint8_t)app_ctx.audio_mid, ts_ms, frame_buf,
                                       frame_len);
                    audio_frame_count++;
                }
            }
        }
#endif

#if NANORTC_FEATURE_VIDEO
        /* Send video frames at 25fps (epoch-based for drift-free timing) */
        if (has_video_src && app_ctx.media_connected) {
            uint32_t now = nano_get_millis();
            if (video_epoch_ms == 0)
                video_epoch_ms = now;
            uint32_t target_frames = (now - video_epoch_ms) / 40; /* 25fps = 40ms */
            if (target_frames - video_frame_count > 2) {
                video_frame_count = target_frames - 1; /* skip burst */
            }
            if (video_frame_count < target_frames) {
                uint8_t frame_buf[NANORTC_MEDIA_MAX_FRAME_SIZE];
                size_t frame_len = 0;
                uint32_t ts_ms = 0;
                if (nano_media_source_next_frame(&video_src, frame_buf, sizeof(frame_buf),
                                                 &frame_len, &ts_ms) == 0) {
                    nanortc_send_video(&rtc, (uint8_t)app_ctx.video_mid, nano_get_millis(),
                                       frame_buf, frame_len);
                    video_frame_count++;
                }
            }
        }
#endif
    }

cleanup:
    if (has_audio_src) {
        nano_media_source_destroy(&audio_src);
    }
    if (has_video_src) {
        nano_media_source_destroy(&video_src);
    }
    http_sig_leave(&sig);
    nano_run_loop_destroy(&loop);
    nanortc_destroy(&rtc);

    fprintf(stderr, "Done.\n");
    return 0;
}
