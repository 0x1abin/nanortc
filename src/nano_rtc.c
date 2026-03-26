/*
 * nanortc — Main state machine
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_ice.h"
#include "nano_stun.h"
#include <string.h>

int nano_rtc_init(nano_rtc_t *rtc, const nano_rtc_config_t *cfg)
{
    if (!rtc || !cfg) {
        return NANO_ERR_INVALID_PARAM;
    }

    memset(rtc, 0, sizeof(*rtc));
    rtc->config = *cfg;
    rtc->state = NANO_STATE_NEW;

    ice_init(&rtc->ice, cfg->role == NANO_ROLE_CONTROLLING);
    dtls_init(&rtc->dtls);
    sctp_init(&rtc->sctp);
    dc_init(&rtc->datachannel);
    sdp_init(&rtc->sdp);

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    rtp_init(&rtc->rtp, 0, 0);
    rtcp_init(&rtc->rtcp, 0);
    srtp_init(&rtc->srtp);
    jitter_init(&rtc->jitter, cfg->jitter_depth_ms);
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
    bwe_init(&rtc->bwe);
#endif

    return NANO_OK;
}

void nano_rtc_destroy(nano_rtc_t *rtc)
{
    if (!rtc) {
        return;
    }
    rtc->state = NANO_STATE_CLOSED;
}

int nano_accept_offer(nano_rtc_t *rtc, const char *offer, char *answer_buf, size_t answer_buf_len)
{
    (void)rtc;
    (void)offer;
    (void)answer_buf;
    (void)answer_buf_len;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_create_offer(nano_rtc_t *rtc, char *offer_buf, size_t offer_buf_len)
{
    (void)rtc;
    (void)offer_buf;
    (void)offer_buf_len;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_accept_answer(nano_rtc_t *rtc, const char *answer)
{
    (void)rtc;
    (void)answer;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_add_local_candidate(nano_rtc_t *rtc, const char *ip, uint16_t port)
{
    (void)rtc;
    (void)ip;
    (void)port;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_add_remote_candidate(nano_rtc_t *rtc, const char *candidate_str)
{
    (void)rtc;
    (void)candidate_str;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_poll_output(nano_rtc_t *rtc, nano_output_t *out)
{
    if (!rtc || !out) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (rtc->out_head == rtc->out_tail) {
        return NANO_ERR_NO_DATA;
    }
    *out = rtc->out_queue[rtc->out_head & (NANO_OUT_QUEUE_SIZE - 1)];
    rtc->out_head++;
    return NANO_OK;
}

/* ----------------------------------------------------------------
 * nano_handle_receive — RFC 7983 demux
 * ---------------------------------------------------------------- */

int nano_handle_receive(nano_rtc_t *rtc, uint32_t now_ms, const uint8_t *data, size_t len,
                        const nano_addr_t *src)
{
    if (!rtc || !data || len == 0 || !src) {
        return NANO_ERR_INVALID_PARAM;
    }

    rtc->now_ms = now_ms;
    uint8_t first = data[0];

    /* RFC 7983 §3: demultiplexing by first byte */
    if (first <= 3) {
        /* STUN [0x00-0x03] */
        size_t resp_len = 0;
        int rc = ice_handle_stun(&rtc->ice, data, len, src, rtc->config.crypto, rtc->stun_buf,
                                 sizeof(rtc->stun_buf), &resp_len);
        if (rc != NANO_OK) {
            return rc;
        }

        /* Enqueue STUN response for transmission */
        if (resp_len > 0) {
            nano_output_t out;
            memset(&out, 0, sizeof(out));
            out.type = NANO_OUTPUT_TRANSMIT;
            out.transmit.data = rtc->stun_buf;
            out.transmit.len = resp_len;
            out.transmit.dest = *src; /* reply to sender */
            rtc_enqueue_output(rtc, &out);
        }

        /* Check for ICE state transition → emit event */
        if (rtc->ice.state == NANO_ICE_STATE_CONNECTED && rtc->state < NANO_STATE_ICE_CONNECTED) {
            rtc->state = NANO_STATE_ICE_CONNECTED;

            nano_output_t evt;
            memset(&evt, 0, sizeof(evt));
            evt.type = NANO_OUTPUT_EVENT;
            evt.event.type = NANO_EVENT_ICE_CONNECTED;
            rtc_enqueue_output(rtc, &evt);
        }

        return NANO_OK;

    } else if (first >= 20 && first <= 63) {
        /* DTLS [0x14-0x3F] */
        return NANO_ERR_NOT_IMPLEMENTED; /* Phase 1 Step 2 */

    } else if (first >= 128 && first <= 191) {
        /* RTP/RTCP [0x80-0xBF] */
        return NANO_ERR_NOT_IMPLEMENTED; /* Phase 2 */
    }

    return NANO_ERR_PROTOCOL; /* Unknown packet type */
}

/* ----------------------------------------------------------------
 * nano_handle_timeout — timer-driven state transitions
 * ---------------------------------------------------------------- */

int nano_handle_timeout(nano_rtc_t *rtc, uint32_t now_ms)
{
    if (!rtc) {
        return NANO_ERR_INVALID_PARAM;
    }

    rtc->now_ms = now_ms;

    /* ICE: generate connectivity checks (controlling role) */
    if (rtc->ice.is_controlling && rtc->ice.state != NANO_ICE_STATE_CONNECTED &&
        rtc->ice.state != NANO_ICE_STATE_FAILED) {
        size_t out_len = 0;
        int rc = ice_generate_check(&rtc->ice, now_ms, rtc->config.crypto, rtc->stun_buf,
                                    sizeof(rtc->stun_buf), &out_len);
        if (rc != NANO_OK) {
            return rc;
        }

        if (out_len > 0) {
            nano_output_t out;
            memset(&out, 0, sizeof(out));
            out.type = NANO_OUTPUT_TRANSMIT;
            out.transmit.data = rtc->stun_buf;
            out.transmit.len = out_len;
            /* Destination: remote candidate address */
            out.transmit.dest.family = rtc->ice.remote_family;
            memcpy(out.transmit.dest.addr, rtc->ice.remote_addr, 16);
            out.transmit.dest.port = rtc->ice.remote_port;
            rtc_enqueue_output(rtc, &out);
        }

        /* Schedule next timeout */
        if (rtc->ice.state == NANO_ICE_STATE_CHECKING) {
            nano_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANO_OUTPUT_TIMEOUT;
            tout.timeout_ms = rtc->ice.check_interval_ms;
            rtc_enqueue_output(rtc, &tout);
        }

        /* Propagate ICE failure */
        if (rtc->ice.state == NANO_ICE_STATE_FAILED) {
            rtc->state = NANO_STATE_CLOSED;
        }
    }

    return NANO_OK;
}

/* ----------------------------------------------------------------
 * DataChannel API stubs
 * ---------------------------------------------------------------- */

int nano_send_datachannel(nano_rtc_t *rtc, uint16_t stream_id, const void *data, size_t len)
{
    (void)rtc;
    (void)stream_id;
    (void)data;
    (void)len;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_send_datachannel_string(nano_rtc_t *rtc, uint16_t stream_id, const char *str)
{
    (void)rtc;
    (void)stream_id;
    (void)str;
    return NANO_ERR_NOT_IMPLEMENTED;
}

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
int nano_send_audio(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len)
{
    (void)rtc;
    (void)timestamp;
    (void)data;
    (void)len;
    return NANO_ERR_NOT_IMPLEMENTED;
}
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
int nano_send_video(nano_rtc_t *rtc, uint32_t timestamp, const void *data, size_t len,
                    int is_keyframe)
{
    (void)rtc;
    (void)timestamp;
    (void)data;
    (void)len;
    (void)is_keyframe;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_request_keyframe(nano_rtc_t *rtc)
{
    (void)rtc;
    return NANO_ERR_NOT_IMPLEMENTED;
}
#endif
