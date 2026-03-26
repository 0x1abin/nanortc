/*
 * nanortc — Main state machine
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
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

int nano_accept_offer(nano_rtc_t *rtc, const char *offer,
                      char *answer_buf, size_t answer_buf_len)
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
    *out = rtc->out_queue[rtc->out_head & 7];
    rtc->out_head++;
    return NANO_OK;
}

int nano_handle_receive(nano_rtc_t *rtc, uint32_t now_ms,
                        const uint8_t *data, size_t len,
                        const nano_addr_t *src)
{
    (void)rtc;
    (void)now_ms;
    (void)data;
    (void)len;
    (void)src;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_handle_timeout(nano_rtc_t *rtc, uint32_t now_ms)
{
    (void)rtc;
    (void)now_ms;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_send_datachannel(nano_rtc_t *rtc, uint16_t stream_id,
                          const void *data, size_t len)
{
    (void)rtc;
    (void)stream_id;
    (void)data;
    (void)len;
    return NANO_ERR_NOT_IMPLEMENTED;
}

int nano_send_datachannel_string(nano_rtc_t *rtc, uint16_t stream_id,
                                 const char *str)
{
    (void)rtc;
    (void)stream_id;
    (void)str;
    return NANO_ERR_NOT_IMPLEMENTED;
}

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
int nano_send_audio(nano_rtc_t *rtc, uint32_t timestamp,
                    const void *data, size_t len)
{
    (void)rtc;
    (void)timestamp;
    (void)data;
    (void)len;
    return NANO_ERR_NOT_IMPLEMENTED;
}
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
int nano_send_video(nano_rtc_t *rtc, uint32_t timestamp,
                    const void *data, size_t len, int is_keyframe)
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
