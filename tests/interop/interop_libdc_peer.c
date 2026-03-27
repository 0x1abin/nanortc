/*
 * nanortc interop tests — libdatachannel peer implementation
 *
 * Uses the libdatachannel C API (rtc/rtc.h) as offerer/controlling peer.
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_libdc_peer.h"
#include "interop_common.h"

#include <rtc/rtc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Callbacks (called from libdatachannel's internal threads)
 * ---------------------------------------------------------------- */

static void on_state_change(int pc, rtcState state, void *ptr)
{
    interop_libdc_peer_t *peer = (interop_libdc_peer_t *)ptr;
    (void)pc;

    const char *names[] = {"New", "Connecting", "Connected",
                           "Disconnected", "Failed", "Closed"};
    fprintf(stderr, "[libdc] State: %s\n",
            (int)state < 6 ? names[(int)state] : "?");

    if (state == RTC_CONNECTED) {
        atomic_store(&peer->connected, 1);
    }
}

static void on_gathering_state_change(int pc, rtcGatheringState state,
                                      void *ptr)
{
    interop_libdc_peer_t *peer = (interop_libdc_peer_t *)ptr;
    (void)pc;

    if (state == RTC_GATHERING_COMPLETE) {
        fprintf(stderr, "[libdc] ICE gathering complete\n");
        atomic_store(&peer->gathering_done, 1);
    }
}

static void on_local_description(int pc, const char *sdp, const char *type,
                                 void *ptr)
{
    interop_libdc_peer_t *peer = (interop_libdc_peer_t *)ptr;
    (void)pc;
    (void)type;

    fprintf(stderr, "[libdc] Local description ready (%s, %zu bytes)\n",
            type, strlen(sdp));

    /* Send the SDP offer through the signaling pipe */
    interop_sig_send(peer->sig_fd, SIG_MSG_SDP_OFFER, sdp, strlen(sdp));
}

static void on_local_candidate(int pc, const char *cand, const char *mid,
                                void *ptr)
{
    interop_libdc_peer_t *peer = (interop_libdc_peer_t *)ptr;
    (void)pc;
    (void)mid;

    if (cand && strlen(cand) > 0) {
        fprintf(stderr, "[libdc] Local ICE candidate: %s\n", cand);
        interop_sig_send(peer->sig_fd, SIG_MSG_ICE_CANDIDATE, cand,
                         strlen(cand));
    }
}

static void on_dc_open(int dc, void *ptr)
{
    interop_libdc_peer_t *peer = (interop_libdc_peer_t *)ptr;
    (void)dc;

    fprintf(stderr, "[libdc] DataChannel open\n");
    atomic_store(&peer->dc_open, 1);
}

static void on_dc_closed(int dc, void *ptr)
{
    (void)dc;
    (void)ptr;
    fprintf(stderr, "[libdc] DataChannel closed\n");
}

static void on_dc_error(int dc, const char *error, void *ptr)
{
    (void)dc;
    (void)ptr;
    fprintf(stderr, "[libdc] DataChannel error: %s\n", error);
}

static void on_dc_message(int dc, const char *message, int size, void *ptr)
{
    interop_libdc_peer_t *peer = (interop_libdc_peer_t *)ptr;
    (void)dc;

    int is_string = (size < 0); /* libdatachannel: size < 0 means string */
    size_t actual_len = is_string ? strlen(message) : (size_t)size;

    fprintf(stderr, "[libdc] DC message (%zu bytes, string=%d)\n",
            actual_len, is_string);

    pthread_mutex_lock(&peer->msg_mutex);
    if (actual_len < sizeof(peer->last_msg)) {
        memcpy(peer->last_msg, message, actual_len);
        if (is_string) {
            peer->last_msg[actual_len] = '\0';
        }
        peer->last_msg_len = actual_len;
        peer->last_msg_is_string = is_string;
    }
    pthread_mutex_unlock(&peer->msg_mutex);
    atomic_fetch_add(&peer->msg_count, 1);
}

/* ----------------------------------------------------------------
 * Signaling: wait for SDP answer from nanortc
 * ---------------------------------------------------------------- */

static int libdc_recv_answer(interop_libdc_peer_t *peer)
{
    char buf[8192];
    uint8_t msg_type;

    int len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1,
                               INTEROP_TIMEOUT_MS);
    if (len < 0 || msg_type != SIG_MSG_SDP_ANSWER) {
        fprintf(stderr, "[libdc] Failed to receive SDP answer\n");
        return -1;
    }
    buf[len] = '\0';
    fprintf(stderr, "[libdc] Got SDP answer (%d bytes)\n", len);

    int rc = rtcSetRemoteDescription(peer->pc, buf, "answer");
    if (rc < 0) {
        fprintf(stderr, "[libdc] rtcSetRemoteDescription failed: %d\n", rc);
        return -1;
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int interop_libdc_start(interop_libdc_peer_t *peer, int sig_fd,
                        const char *label, uint16_t remote_port)
{
    if (!peer || !label) {
        return -1;
    }

    memset(peer, 0, sizeof(*peer));
    peer->sig_fd = sig_fd;
    peer->pc = -1;
    peer->dc = -1;
    pthread_mutex_init(&peer->msg_mutex, NULL);

    /* Init libdatachannel logging */
    rtcInitLogger(RTC_LOG_WARNING, NULL);

    /* Create PeerConnection (no STUN/TURN — localhost only) */
    rtcConfiguration config;
    memset(&config, 0, sizeof(config));
    config.iceServers = NULL;
    config.iceServersCount = 0;

    peer->pc = rtcCreatePeerConnection(&config);
    if (peer->pc < 0) {
        fprintf(stderr, "[libdc] rtcCreatePeerConnection failed: %d\n",
                peer->pc);
        return -1;
    }

    /* Set callbacks */
    rtcSetUserPointer(peer->pc, peer);
    rtcSetStateChangeCallback(peer->pc, on_state_change);
    rtcSetGatheringStateChangeCallback(peer->pc, on_gathering_state_change);
    rtcSetLocalDescriptionCallback(peer->pc, on_local_description);
    rtcSetLocalCandidateCallback(peer->pc, on_local_candidate);

    /* Create DataChannel (triggers SDP offer generation) */
    peer->dc = rtcCreateDataChannel(peer->pc, label);
    if (peer->dc < 0) {
        fprintf(stderr, "[libdc] rtcCreateDataChannel failed: %d\n",
                peer->dc);
        rtcDeletePeerConnection(peer->pc);
        return -1;
    }

    /* Set DataChannel callbacks */
    rtcSetUserPointer(peer->dc, peer);
    rtcSetOpenCallback(peer->dc, on_dc_open);
    rtcSetClosedCallback(peer->dc, on_dc_closed);
    rtcSetErrorCallback(peer->dc, on_dc_error);
    rtcSetMessageCallback(peer->dc, on_dc_message);

    /* Wait for gathering to produce candidates + answer exchange */
    /* The on_local_description callback sends the offer automatically.
     * Now we wait for the answer from nanortc. */
    interop_sleep_ms(100); /* Give libdc time to generate offer */

    int rc = libdc_recv_answer(peer);
    if (rc != 0) {
        return -1;
    }

    /* Wait for ICE gathering to complete, then signal DONE */
    uint32_t start = interop_get_millis();
    while (!atomic_load(&peer->gathering_done)) {
        if (interop_get_millis() - start > INTEROP_TIMEOUT_MS) {
            fprintf(stderr, "[libdc] ICE gathering timeout\n");
            return -1;
        }
        interop_sleep_ms(10);
    }

    /* Signal end of signaling */
    interop_sig_send(peer->sig_fd, SIG_MSG_DONE, "", 0);
    fprintf(stderr, "[libdc] Signaling complete\n");

    return 0;
}

int interop_libdc_send_string(interop_libdc_peer_t *peer, const char *str)
{
    if (!peer || peer->dc < 0 || !str) {
        return -1;
    }
    return rtcSendMessage(peer->dc, str, -1); /* -1 = string */
}

int interop_libdc_send_binary(interop_libdc_peer_t *peer, const void *data,
                              size_t len)
{
    if (!peer || peer->dc < 0 || !data) {
        return -1;
    }
    return rtcSendMessage(peer->dc, (const char *)data, (int)len);
}

int interop_libdc_wait_flag(atomic_int *flag, int timeout_ms)
{
    uint32_t start = interop_get_millis();
    while (!atomic_load(flag)) {
        if ((int)(interop_get_millis() - start) >= timeout_ms) {
            return -1;
        }
        interop_sleep_ms(10);
    }
    return 0;
}

int interop_libdc_stop(interop_libdc_peer_t *peer)
{
    if (!peer) {
        return -1;
    }

    if (peer->dc >= 0) {
        rtcDeleteDataChannel(peer->dc);
        peer->dc = -1;
    }
    if (peer->pc >= 0) {
        rtcDeletePeerConnection(peer->pc);
        peer->pc = -1;
    }

    pthread_mutex_destroy(&peer->msg_mutex);
    rtcCleanup();

    return 0;
}
