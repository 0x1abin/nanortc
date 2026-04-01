/*
 * nanortc interop tests — libdatachannel media peer implementation
 *
 * Uses the libdatachannel C API (rtc/rtc.h) as offerer/controlling peer
 * with audio/video track support via packetizers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_libdatachannel_media_peer.h"
#include "interop_common.h"

#include <rtc/rtc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Callbacks (called from libdatachannel's internal threads)
 * ---------------------------------------------------------------- */

static void on_media_state_change(int pc, rtcState state, void *ptr)
{
    interop_libdatachannel_media_peer_t *peer = (interop_libdatachannel_media_peer_t *)ptr;
    (void)pc;

    const char *names[] = {"New", "Connecting", "Connected", "Disconnected", "Failed", "Closed"};
    fprintf(stderr, "[libdatachannel-media] State: %s\n", (int)state < 6 ? names[(int)state] : "?");

    if (state == RTC_CONNECTED) {
        atomic_store(&peer->connected, 1);
    }
}

static void on_media_gathering_state_change(int pc, rtcGatheringState state, void *ptr)
{
    interop_libdatachannel_media_peer_t *peer = (interop_libdatachannel_media_peer_t *)ptr;
    (void)pc;

    if (state == RTC_GATHERING_COMPLETE) {
        fprintf(stderr, "[libdatachannel-media] ICE gathering complete\n");
        atomic_store(&peer->gathering_done, 1);
    }
}

static void on_media_local_description(int pc, const char *sdp, const char *type, void *ptr)
{
    interop_libdatachannel_media_peer_t *peer = (interop_libdatachannel_media_peer_t *)ptr;
    (void)pc;
    (void)type;

    fprintf(stderr, "[libdatachannel-media] Local description ready (%s, %zu bytes)\n", type,
            strlen(sdp));

    /* Send the SDP offer through the signaling pipe */
    interop_sig_send(peer->sig_fd, SIG_MSG_SDP_OFFER, sdp, strlen(sdp));
}

static void on_media_local_candidate(int pc, const char *cand, const char *mid, void *ptr)
{
    interop_libdatachannel_media_peer_t *peer = (interop_libdatachannel_media_peer_t *)ptr;
    (void)pc;
    (void)mid;

    if (cand && strlen(cand) > 0) {
        fprintf(stderr, "[libdatachannel-media] Local ICE candidate: %s\n", cand);
        interop_sig_send(peer->sig_fd, SIG_MSG_ICE_CANDIDATE, cand, strlen(cand));
    }
}

static void on_track_open(int tr, void *ptr)
{
    interop_libdatachannel_media_peer_t *peer = (interop_libdatachannel_media_peer_t *)ptr;
    (void)tr;

    fprintf(stderr, "[libdatachannel-media] Track open (handle=%d)\n", tr);
    atomic_store(&peer->track_open, 1);
}

static void on_track_closed(int tr, void *ptr)
{
    (void)tr;
    (void)ptr;
    fprintf(stderr, "[libdatachannel-media] Track closed\n");
}

static void on_track_error(int tr, const char *error, void *ptr)
{
    (void)tr;
    (void)ptr;
    fprintf(stderr, "[libdatachannel-media] Track error: %s\n", error);
}

static void on_track_message(int tr, const char *message, int size, void *ptr)
{
    interop_libdatachannel_media_peer_t *peer = (interop_libdatachannel_media_peer_t *)ptr;
    (void)tr;

    /* size >= 0 for binary (RTP) data */
    if (size < 0) {
        return; /* ignore string messages */
    }
    size_t actual_len = (size_t)size;

    fprintf(stderr, "[libdatachannel-media] Track message (%zu bytes, handle=%d)\n", actual_len,
            tr);

    size_t copy_len = actual_len;
    if (copy_len > INTEROP_MEDIA_FRAME_SIZE) {
        copy_len = INTEROP_MEDIA_FRAME_SIZE;
    }

    pthread_mutex_lock(&peer->frame_mutex);
    int idx = peer->frame_write_idx % INTEROP_MEDIA_MAX_FRAMES;
    memcpy(peer->frames[idx].data, message, copy_len);
    peer->frames[idx].len = copy_len;
    peer->frames[idx].mid = 0;
    peer->frames[idx].timestamp = 0;
    peer->frames[idx].is_keyframe = false;
    peer->frame_write_idx++;
    pthread_mutex_unlock(&peer->frame_mutex);

    atomic_fetch_add(&peer->frame_count, 1);
}

/* ----------------------------------------------------------------
 * Map track config to libdatachannel codec enum
 * ---------------------------------------------------------------- */

static rtcCodec ldc_to_rtc_codec(ldc_codec_t codec)
{
    switch (codec) {
    case LDC_CODEC_OPUS:
        return RTC_CODEC_OPUS;
    case LDC_CODEC_PCMA:
        return RTC_CODEC_PCMA;
    case LDC_CODEC_PCMU:
        return RTC_CODEC_PCMU;
    case LDC_CODEC_H264:
        return RTC_CODEC_H264;
    default:
        return RTC_CODEC_H264;
    }
}

static rtcDirection ldc_to_rtc_direction(ldc_direction_t dir)
{
    switch (dir) {
    case LDC_DIR_SENDRECV:
        return RTC_DIRECTION_SENDRECV;
    case LDC_DIR_SENDONLY:
        return RTC_DIRECTION_SENDONLY;
    case LDC_DIR_RECVONLY:
        return RTC_DIRECTION_RECVONLY;
    default:
        return RTC_DIRECTION_SENDRECV;
    }
}

/* ----------------------------------------------------------------
 * Signaling: wait for SDP answer from nanortc
 * ---------------------------------------------------------------- */

static int libdatachannel_media_recv_answer(interop_libdatachannel_media_peer_t *peer)
{
    char buf[8192];
    uint8_t msg_type;

    int len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1, INTEROP_TIMEOUT_MS);
    if (len < 0 || msg_type != SIG_MSG_SDP_ANSWER) {
        fprintf(stderr, "[libdatachannel-media] Failed to receive SDP answer\n");
        return -1;
    }
    buf[len] = '\0';
    fprintf(stderr, "[libdatachannel-media] Got SDP answer (%d bytes)\n", len);

    int rc = rtcSetRemoteDescription(peer->pc, buf, "answer");
    if (rc < 0) {
        fprintf(stderr, "[libdatachannel-media] rtcSetRemoteDescription failed: %d\n", rc);
        return -1;
    }

    /* Explicitly add nanortc's candidate as fallback (RFC 8839 §5.1) */
    if (peer->remote_port > 0) {
        char cand[128];
        snprintf(cand, sizeof(cand), "candidate:1 1 UDP 2122252543 127.0.0.1 %d typ host",
                 peer->remote_port);
        rtcAddRemoteCandidate(peer->pc, cand, "0");
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Set up packetizer and RTCP chain for a track
 * ---------------------------------------------------------------- */

static int setup_track_packetizer(int tr, const ldc_track_config_t *cfg)
{
    rtcPacketizerInit pkt_init;
    memset(&pkt_init, 0, sizeof(pkt_init));
    pkt_init.ssrc = cfg->ssrc;
    pkt_init.cname = "interop-test";
    pkt_init.payloadType = cfg->payload_type;
    pkt_init.maxFragmentSize = 1200;

    int rc = 0;

    switch (cfg->codec) {
    case LDC_CODEC_OPUS:
        pkt_init.clockRate = 48000;
        rc = rtcSetOpusPacketizer(tr, &pkt_init);
        break;
    case LDC_CODEC_PCMA:
    case LDC_CODEC_PCMU:
        pkt_init.clockRate = 8000;
        rc = rtcSetOpusPacketizer(tr, &pkt_init);
        break;
    case LDC_CODEC_H264:
        pkt_init.clockRate = 90000;
        pkt_init.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
        rc = rtcSetH264Packetizer(tr, &pkt_init);
        break;
    }

    if (rc < 0) {
        fprintf(stderr, "[libdatachannel-media] Failed to set packetizer: %d\n", rc);
        return -1;
    }

    /* Chain RTCP handlers */
    rtcChainRtcpSrReporter(tr);
    rtcChainRtcpReceivingSession(tr);

    return 0;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int interop_libdatachannel_media_start(interop_libdatachannel_media_peer_t *peer, int sig_fd,
                                       const ldc_track_config_t *track_configs, int track_count,
                                       uint16_t remote_port)
{
    if (!peer || !track_configs || track_count < 1 || track_count > LDC_MEDIA_MAX_TRACKS) {
        return -1;
    }

    memset(peer, 0, sizeof(*peer));
    peer->sig_fd = sig_fd;
    peer->pc = -1;
    peer->track_count = track_count;
    peer->remote_port = remote_port;
    for (int i = 0; i < LDC_MEDIA_MAX_TRACKS; i++) {
        peer->tracks[i] = -1;
    }
    pthread_mutex_init(&peer->frame_mutex, NULL);

    /* Init libdatachannel logging */
    rtcInitLogger(RTC_LOG_WARNING, NULL);

    /* Create PeerConnection (no STUN/TURN -- localhost only) */
    rtcConfiguration config;
    memset(&config, 0, sizeof(config));
    config.iceServers = NULL;
    config.iceServersCount = 0;

    peer->pc = rtcCreatePeerConnection(&config);
    if (peer->pc < 0) {
        fprintf(stderr, "[libdatachannel-media] rtcCreatePeerConnection failed: %d\n", peer->pc);
        return -1;
    }

    /* Set PeerConnection callbacks */
    rtcSetUserPointer(peer->pc, peer);
    rtcSetStateChangeCallback(peer->pc, on_media_state_change);
    rtcSetGatheringStateChangeCallback(peer->pc, on_media_gathering_state_change);
    rtcSetLocalDescriptionCallback(peer->pc, on_media_local_description);
    rtcSetLocalCandidateCallback(peer->pc, on_media_local_candidate);

    /* Add media tracks */
    for (int i = 0; i < track_count; i++) {
        const ldc_track_config_t *tc = &track_configs[i];

        rtcTrackInit init;
        memset(&init, 0, sizeof(init));
        init.direction = ldc_to_rtc_direction(tc->direction);
        init.codec = ldc_to_rtc_codec(tc->codec);
        init.payloadType = (int)tc->payload_type;
        init.ssrc = tc->ssrc;

        /* Use MID "0" for first track, "1" for second */
        char mid_str[4];
        snprintf(mid_str, sizeof(mid_str), "%d", i);
        init.mid = mid_str;

        /* Set profile for H.264 */
        if (tc->codec == LDC_CODEC_H264) {
            init.profile = "profile-level-id=42e01f";
        }

        int tr = rtcAddTrackEx(peer->pc, &init);
        if (tr < 0) {
            fprintf(stderr, "[libdatachannel-media] rtcAddTrackEx failed: %d\n", tr);
            rtcDeletePeerConnection(peer->pc);
            return -1;
        }
        peer->tracks[i] = tr;

        /* Set track callbacks */
        rtcSetUserPointer(tr, peer);
        rtcSetOpenCallback(tr, on_track_open);
        rtcSetClosedCallback(tr, on_track_closed);
        rtcSetErrorCallback(tr, on_track_error);
        rtcSetMessageCallback(tr, on_track_message);

        /* Set up packetizer for sending */
        if (tc->direction != LDC_DIR_RECVONLY) {
            if (setup_track_packetizer(tr, tc) != 0) {
                rtcDeletePeerConnection(peer->pc);
                return -1;
            }
        }

        fprintf(stderr, "[libdatachannel-media] Added track %d: kind=%d handle=%d\n", i, tc->kind,
                tr);
    }

    /* libdatachannel needs setLocalDescription to trigger offer generation
     * when only tracks (no DataChannels) are added */
    rtcSetLocalDescription(peer->pc, "offer");

    /* Give libdatachannel time to generate offer */
    interop_sleep_ms(100);

    /* Wait for answer from nanortc */
    int rc = libdatachannel_media_recv_answer(peer);
    if (rc != 0) {
        return -1;
    }

    /* Wait for ICE gathering to complete, then signal DONE */
    uint32_t start = interop_get_millis();
    while (!atomic_load(&peer->gathering_done)) {
        if (interop_get_millis() - start > INTEROP_TIMEOUT_MS) {
            fprintf(stderr, "[libdatachannel-media] ICE gathering timeout\n");
            return -1;
        }
        interop_sleep_ms(10);
    }

    /* Signal end of signaling */
    interop_sig_send(peer->sig_fd, SIG_MSG_DONE, "", 0);
    fprintf(stderr, "[libdatachannel-media] Signaling complete\n");

    return 0;
}

int interop_libdatachannel_media_send(interop_libdatachannel_media_peer_t *peer, int track_idx,
                                      const void *data, size_t len)
{
    if (!peer || track_idx < 0 || track_idx >= peer->track_count || peer->tracks[track_idx] < 0 ||
        !data) {
        return -1;
    }
    return rtcSendMessage(peer->tracks[track_idx], (const char *)data, (int)len);
}

int interop_libdatachannel_media_get_last_frame(interop_libdatachannel_media_peer_t *peer,
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

int interop_libdatachannel_media_wait_flag(atomic_int *flag, int timeout_ms)
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

int interop_libdatachannel_media_stop(interop_libdatachannel_media_peer_t *peer)
{
    if (!peer) {
        return -1;
    }

    for (int i = 0; i < LDC_MEDIA_MAX_TRACKS; i++) {
        if (peer->tracks[i] >= 0) {
            rtcDeleteTrack(peer->tracks[i]);
            peer->tracks[i] = -1;
        }
    }
    if (peer->pc >= 0) {
        rtcDeletePeerConnection(peer->pc);
        peer->pc = -1;
    }

    pthread_mutex_destroy(&peer->frame_mutex);
    rtcCleanup();

    return 0;
}
