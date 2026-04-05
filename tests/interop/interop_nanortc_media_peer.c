/*
 * nanortc interop tests — NanoRTC media peer implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_nanortc_media_peer.h"
#include "interop_common.h"
#include "nanortc_crypto.h"

#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ----------------------------------------------------------------
 * Event callback (runs in nanortc thread)
 * ---------------------------------------------------------------- */

static void nanortc_media_on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    interop_nanortc_media_peer_t *peer = (interop_nanortc_media_peer_t *)userdata;
    (void)rtc;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        fprintf(stderr, "[nanortc-media] ICE state change (%d)\n", evt->ice_state);
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            atomic_store(&peer->ice_connected, 1);
        }
        break;

    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[nanortc-media] Fully connected\n");
        atomic_store(&peer->dtls_connected, 1);
        atomic_store(&peer->connected, 1);
        break;

    case NANORTC_EV_MEDIA_ADDED:
        fprintf(stderr, "[nanortc-media] Media added (mid=%d, kind=%d)\n", evt->media_added.mid,
                evt->media_added.kind);
        atomic_store(&peer->media_added, 1);
        break;

    case NANORTC_EV_MEDIA_DATA: {
        size_t copy_len = evt->media_data.len;
        if (copy_len > INTEROP_MEDIA_FRAME_SIZE) {
            copy_len = INTEROP_MEDIA_FRAME_SIZE;
        }

        pthread_mutex_lock(&peer->frame_mutex);
        int idx = peer->frame_write_idx % INTEROP_MEDIA_MAX_FRAMES;
        memcpy(peer->frames[idx].data, evt->media_data.data, copy_len);
        peer->frames[idx].len = copy_len;
        peer->frames[idx].mid = evt->media_data.mid;
        peer->frames[idx].timestamp = evt->media_data.timestamp;
        peer->frames[idx].is_keyframe = evt->media_data.is_keyframe;
        peer->frame_write_idx++;
        pthread_mutex_unlock(&peer->frame_mutex);

        atomic_fetch_add(&peer->frame_count, 1);
        fprintf(stderr, "[nanortc-media] Media data (mid=%d, len=%zu, kf=%d)\n",
                evt->media_data.mid, evt->media_data.len, evt->media_data.is_keyframe);
        break;
    }

    case NANORTC_EV_KEYFRAME_REQUEST:
        fprintf(stderr, "[nanortc-media] Keyframe request (mid=%d)\n", evt->keyframe_request.mid);
        break;

    case NANORTC_EV_DISCONNECTED:
        fprintf(stderr, "[nanortc-media] Disconnected\n");
        nano_run_loop_stop(&peer->loop);
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * Signaling: handle SDP offer from libdatachannel
 * ---------------------------------------------------------------- */

static int nanortc_media_do_signaling(interop_nanortc_media_peer_t *peer)
{
    char buf[8192];
    uint8_t msg_type;

    /* Read SDP offer */
    int len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1, INTEROP_TIMEOUT_MS);
    if (len < 0 || msg_type != SIG_MSG_SDP_OFFER) {
        fprintf(stderr, "[nanortc-media] Failed to receive SDP offer\n");
        return -1;
    }
    buf[len] = '\0';
    fprintf(stderr, "[nanortc-media] Got SDP offer (%d bytes)\n", len);

    /* Add tracks before accepting the offer */
    for (int i = 0; i < peer->track_count; i++) {
        const interop_media_track_config_t *tc = &peer->tracks[i];
        int mid = -1;

        if (tc->kind == NANORTC_TRACK_AUDIO) {
            mid = nanortc_add_audio_track(&peer->rtc, tc->direction, tc->codec, tc->sample_rate,
                                          tc->channels);
        } else if (tc->kind == NANORTC_TRACK_VIDEO) {
            mid = nanortc_add_video_track(&peer->rtc, tc->direction, tc->codec);
        }

        if (mid < 0) {
            fprintf(stderr, "[nanortc-media] Failed to add track %d (kind=%d): %d\n", i, tc->kind,
                    mid);
            return -1;
        }
        peer->track_mids[i] = mid;
        fprintf(stderr, "[nanortc-media] Added track %d: kind=%d mid=%d\n", i, tc->kind, mid);
    }

    /* Generate answer */
    char answer[8192];
    size_t answer_len = 0;
    int rc = nanortc_accept_offer(&peer->rtc, buf, answer, sizeof(answer), &answer_len);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "[nanortc-media] nanortc_accept_offer failed: %d\n", rc);
        return -1;
    }
    rc = interop_sig_send(peer->sig_fd, SIG_MSG_SDP_ANSWER, answer, answer_len);
    if (rc != 0) {
        fprintf(stderr, "[nanortc-media] Failed to send SDP answer\n");
        return -1;
    }
    fprintf(stderr, "[nanortc-media] Sent SDP answer (%zu bytes)\n", answer_len);

    /* Exchange ICE candidates until DONE */
    for (;;) {
        len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1, INTEROP_TIMEOUT_MS);
        if (len < 0) {
            fprintf(stderr, "[nanortc-media] Signaling recv error\n");
            return -1;
        }
        buf[len] = '\0';

        if (msg_type == SIG_MSG_DONE) {
            fprintf(stderr, "[nanortc-media] Signaling complete\n");
            break;
        }

        if (msg_type == SIG_MSG_ICE_CANDIDATE) {
            fprintf(stderr, "[nanortc-media] Got remote ICE candidate: %s\n", buf);
            nanortc_add_remote_candidate(&peer->rtc, buf);
        }
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Thread entry
 * ---------------------------------------------------------------- */

static void *nanortc_media_thread_fn(void *arg)
{
    interop_nanortc_media_peer_t *peer = (interop_nanortc_media_peer_t *)arg;

    /* Signaling phase (blocking) */
    if (nanortc_media_do_signaling(peer) != 0) {
        fprintf(stderr, "[nanortc-media] Signaling failed, exiting thread\n");
        atomic_store(&peer->running, 0);
        return NULL;
    }

    /* Event loop: drive nanortc with real UDP */
    fprintf(stderr, "[nanortc-media] Entering event loop (port=%d)\n", peer->port);
    while (atomic_load(&peer->running)) {
        nano_run_loop_step(&peer->loop);
    }

    fprintf(stderr, "[nanortc-media] Thread exiting\n");
    return NULL;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int interop_nanortc_media_start(interop_nanortc_media_peer_t *peer, int sig_fd, uint16_t port,
                                const interop_media_track_config_t *tracks, int track_count)
{
    if (!peer || !tracks || track_count < 1 || track_count > 2) {
        return -1;
    }

    memset(peer, 0, sizeof(*peer));
    peer->sig_fd = sig_fd;
    peer->port = port;
    peer->track_count = track_count;
    for (int i = 0; i < track_count; i++) {
        peer->tracks[i] = tracks[i];
        peer->track_mids[i] = -1;
    }
    pthread_mutex_init(&peer->frame_mutex, NULL);

    /* Init nanortc */
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nanortc_crypto_openssl();
#else
    cfg.crypto = nanortc_crypto_mbedtls();
#endif
    cfg.role = NANORTC_ROLE_CONTROLLED; /* answerer */

    int rc = nanortc_init(&peer->rtc, &cfg);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "[nanortc-media] nanortc_init failed: %d\n", rc);
        return -1;
    }

    /* Add local candidate so SDP answer includes a=candidate: line */
    nanortc_add_local_candidate(&peer->rtc, "127.0.0.1", port);

    /* Init run loop (binds UDP socket) */
    rc = nano_run_loop_init(&peer->loop, &peer->rtc, "127.0.0.1", port);
    if (rc < 0) {
        fprintf(stderr, "[nanortc-media] Failed to bind UDP port %d\n", port);
        return -1;
    }
    nano_run_loop_set_event_cb(&peer->loop, nanortc_media_on_event, peer);

    /* Start thread */
    atomic_store(&peer->running, 1);
    rc = pthread_create(&peer->thread, NULL, nanortc_media_thread_fn, peer);
    if (rc != 0) {
        fprintf(stderr, "[nanortc-media] pthread_create failed: %d\n", rc);
        atomic_store(&peer->running, 0);
        nano_run_loop_destroy(&peer->loop);
        return -1;
    }

    return 0;
}

int interop_nanortc_media_stop(interop_nanortc_media_peer_t *peer)
{
    if (!peer) {
        return -1;
    }

    atomic_store(&peer->running, 0);
    nano_run_loop_stop(&peer->loop);
    pthread_join(peer->thread, NULL);

    nano_run_loop_destroy(&peer->loop);
    nanortc_destroy(&peer->rtc);
    pthread_mutex_destroy(&peer->frame_mutex);

    return 0;
}

int interop_nanortc_media_send_audio(interop_nanortc_media_peer_t *peer, uint8_t mid,
                                     uint32_t pts_ms, const void *data, size_t len)
{
    if (!peer || !data) {
        return -1;
    }
    return nanortc_send_audio(&peer->rtc, mid, pts_ms, data, len);
}

int interop_nanortc_media_send_video(interop_nanortc_media_peer_t *peer, uint8_t mid,
                                     uint32_t pts_ms, const void *data, size_t len)
{
    if (!peer || !data) {
        return -1;
    }
#if NANORTC_FEATURE_VIDEO
    return nanortc_send_video(&peer->rtc, mid, pts_ms, data, len);
#else
    (void)mid;
    (void)len;
    return -1;
#endif
}

int interop_nanortc_media_get_last_frame(interop_nanortc_media_peer_t *peer,
                                         interop_media_frame_t *out)
{
    if (!peer || !out) {
        return -1;
    }

    pthread_mutex_lock(&peer->frame_mutex);
    if (peer->frame_write_idx == 0) {
        pthread_mutex_unlock(&peer->frame_mutex);
        return -1;
    }
    int idx = (peer->frame_write_idx - 1) % INTEROP_MEDIA_MAX_FRAMES;
    *out = peer->frames[idx];
    pthread_mutex_unlock(&peer->frame_mutex);

    return 0;
}
