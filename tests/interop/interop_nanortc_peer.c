/*
 * nanortc interop tests — NanoRTC peer implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_nanortc_peer.h"
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

static void nanortc_on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    interop_nanortc_peer_t *peer = (interop_nanortc_peer_t *)userdata;
    (void)rtc;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        fprintf(stderr, "[nanortc] ICE state change (%d)\n", evt->ice_state);
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            atomic_store(&peer->ice_connected, 1);
        }
        break;

    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[nanortc] Fully connected\n");
        atomic_store(&peer->dtls_connected, 1);
        atomic_store(&peer->sctp_connected, 1);
        break;

    case NANORTC_EV_DATACHANNEL_OPEN:
        fprintf(stderr, "[nanortc] DataChannel open (stream=%d)\n", evt->datachannel_open.id);
        atomic_store(&peer->dc_open, 1);
        break;

    case NANORTC_EV_DATACHANNEL_DATA:
        if (evt->datachannel_data.binary) {
            fprintf(stderr, "[nanortc] DC binary data (%zu bytes)\n", evt->datachannel_data.len);
            pthread_mutex_lock(&peer->msg_mutex);
            if (evt->datachannel_data.len <= sizeof(peer->last_msg)) {
                memcpy(peer->last_msg, evt->datachannel_data.data, evt->datachannel_data.len);
                peer->last_msg_len = evt->datachannel_data.len;
                peer->last_msg_is_string = 0;
            }
            pthread_mutex_unlock(&peer->msg_mutex);
        } else {
            fprintf(stderr, "[nanortc] DC string: %.*s\n", (int)evt->datachannel_data.len,
                    (const char *)evt->datachannel_data.data);
            pthread_mutex_lock(&peer->msg_mutex);
            if (evt->datachannel_data.len < sizeof(peer->last_msg)) {
                memcpy(peer->last_msg, evt->datachannel_data.data, evt->datachannel_data.len);
                peer->last_msg[evt->datachannel_data.len] = '\0';
                peer->last_msg_len = evt->datachannel_data.len;
                peer->last_msg_is_string = 1;
            }
            pthread_mutex_unlock(&peer->msg_mutex);
        }
        atomic_fetch_add(&peer->msg_count, 1);
        break;

    case NANORTC_EV_DATACHANNEL_CLOSE:
        fprintf(stderr, "[nanortc] DataChannel closed\n");
        break;

    case NANORTC_EV_DISCONNECTED:
        fprintf(stderr, "[nanortc] Disconnected\n");
        nano_run_loop_stop(&peer->loop);
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * Signaling: handle SDP offer from libdatachannel
 * ---------------------------------------------------------------- */

static int nanortc_do_signaling(interop_nanortc_peer_t *peer)
{
    char buf[8192];
    uint8_t msg_type;

    /* Read SDP offer */
    int len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1, INTEROP_TIMEOUT_MS);
    if (len < 0 || msg_type != SIG_MSG_SDP_OFFER) {
        fprintf(stderr, "[nanortc] Failed to receive SDP offer\n");
        return -1;
    }
    buf[len] = '\0';
    fprintf(stderr, "[nanortc] Got SDP offer (%d bytes)\n", len);

    /* Generate answer */
    char answer[8192];
    size_t answer_len = 0;
    int rc = nanortc_accept_offer(&peer->rtc, buf, answer, sizeof(answer), &answer_len);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "[nanortc] nanortc_accept_offer failed: %d\n", rc);
        return -1;
    }
    rc = interop_sig_send(peer->sig_fd, SIG_MSG_SDP_ANSWER, answer, answer_len);
    if (rc != 0) {
        fprintf(stderr, "[nanortc] Failed to send SDP answer\n");
        return -1;
    }
    fprintf(stderr, "[nanortc] Sent SDP answer (%zu bytes)\n", answer_len);

    /* Exchange ICE candidates until DONE */
    for (;;) {
        len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1, INTEROP_TIMEOUT_MS);
        if (len < 0) {
            fprintf(stderr, "[nanortc] Signaling recv error\n");
            return -1;
        }
        buf[len] = '\0';

        if (msg_type == SIG_MSG_DONE) {
            fprintf(stderr, "[nanortc] Signaling complete\n");
            break;
        }

        if (msg_type == SIG_MSG_ICE_CANDIDATE) {
            fprintf(stderr, "[nanortc] Got remote ICE candidate: %s\n", buf);
            nanortc_add_remote_candidate(&peer->rtc, buf);
        }
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Thread entry
 * ---------------------------------------------------------------- */

static void *nanortc_thread_fn(void *arg)
{
    interop_nanortc_peer_t *peer = (interop_nanortc_peer_t *)arg;

    /* Signaling phase (blocking) */
    if (nanortc_do_signaling(peer) != 0) {
        fprintf(stderr, "[nanortc] Signaling failed, exiting thread\n");
        atomic_store(&peer->running, 0);
        return NULL;
    }

    /* Event loop: drive nanortc with real UDP */
    fprintf(stderr, "[nanortc] Entering event loop (port=%d)\n", peer->port);
    while (atomic_load(&peer->running)) {
        nano_run_loop_step(&peer->loop);
    }

    fprintf(stderr, "[nanortc] Thread exiting\n");
    return NULL;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int interop_nanortc_start(interop_nanortc_peer_t *peer, int sig_fd, uint16_t port)
{
    if (!peer) {
        return -1;
    }

    memset(peer, 0, sizeof(*peer));
    peer->sig_fd = sig_fd;
    peer->port = port;
    pthread_mutex_init(&peer->msg_mutex, NULL);

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
        fprintf(stderr, "[nanortc] nanortc_init failed: %d\n", rc);
        return -1;
    }

    /* Add local candidate so SDP answer includes a=candidate: line */
    nanortc_add_local_candidate(&peer->rtc, "127.0.0.1", port);

    /* Init run loop (binds UDP socket) */
    rc = nano_run_loop_init(&peer->loop, &peer->rtc, "127.0.0.1", port);
    if (rc < 0) {
        fprintf(stderr, "[nanortc] Failed to bind UDP port %d\n", port);
        return -1;
    }
    nano_run_loop_set_event_cb(&peer->loop, nanortc_on_event, peer);

    /* Start thread */
    atomic_store(&peer->running, 1);
    rc = pthread_create(&peer->thread, NULL, nanortc_thread_fn, peer);
    if (rc != 0) {
        fprintf(stderr, "[nanortc] pthread_create failed: %d\n", rc);
        atomic_store(&peer->running, 0);
        nano_run_loop_destroy(&peer->loop);
        return -1;
    }

    return 0;
}

int interop_nanortc_stop(interop_nanortc_peer_t *peer)
{
    if (!peer) {
        return -1;
    }

    atomic_store(&peer->running, 0);
    nano_run_loop_stop(&peer->loop);
    pthread_join(peer->thread, NULL);

    nano_run_loop_destroy(&peer->loop);
    nanortc_destroy(&peer->rtc);
    pthread_mutex_destroy(&peer->msg_mutex);

    return 0;
}

int interop_nanortc_wait_flag(atomic_int *flag, int timeout_ms)
{
    uint32_t start = interop_get_millis();
    while (!atomic_load(flag)) {
        uint32_t elapsed = interop_get_millis() - start;
        if ((int)elapsed >= timeout_ms) {
            return -1;
        }
        interop_sleep_ms(10);
    }
    return 0;
}
