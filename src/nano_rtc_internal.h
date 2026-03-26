/*
 * nanortc — Internal state structure
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_RTC_INTERNAL_H_
#define NANO_RTC_INTERNAL_H_

#include "nanortc.h"
#include "nano_ice.h"
#include "nano_dtls.h"
#include "nano_sctp.h"
#include "nano_datachannel.h"
#include "nano_sdp.h"

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
#include "nano_rtp.h"
#include "nano_rtcp.h"
#include "nano_srtp.h"
#include "nano_jitter.h"
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
#include "nano_bwe.h"
#endif

/* ----------------------------------------------------------------
 * Connection state
 * ---------------------------------------------------------------- */

typedef enum {
    NANO_STATE_NEW,
    NANO_STATE_ICE_CHECKING,
    NANO_STATE_ICE_CONNECTED,
    NANO_STATE_DTLS_HANDSHAKING,
    NANO_STATE_DTLS_CONNECTED,
    NANO_STATE_SCTP_CONNECTING,
    NANO_STATE_CONNECTED,
    NANO_STATE_CLOSED,
} nano_conn_state_t;

/* ----------------------------------------------------------------
 * Output queue helper
 * ---------------------------------------------------------------- */

#define NANO_OUT_QUEUE_SIZE 8

/* ----------------------------------------------------------------
 * Main state machine
 * ---------------------------------------------------------------- */

struct nano_rtc {
    nano_rtc_config_t config;
    nano_conn_state_t state;
    uint32_t now_ms; /* last known time */

    /* Subsystem state */
    nano_ice_t ice;
    nano_dtls_t dtls;
    nano_sctp_t sctp;
    nano_dc_t datachannel;
    nano_sdp_t sdp;

#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    nano_rtp_t rtp;
    nano_rtcp_t rtcp;
    nano_srtp_t srtp;
    nano_jitter_t jitter;
#endif

#if NANORTC_PROFILE >= NANO_PROFILE_MEDIA
    nano_bwe_t bwe;
#endif

    /* Output queue (simple ring buffer) */
    nano_output_t out_queue[NANO_OUT_QUEUE_SIZE];
    uint8_t out_head;
    uint8_t out_tail;

    /* Scratch buffer for STUN encode/decode.
     * Sans I/O contract: caller must drain outputs before next handle_receive. */
    uint8_t stun_buf[256];

    /* Scratch buffer for DTLS output polling */
    uint8_t dtls_scratch[NANO_DTLS_BUF_SIZE];
};

/* Enqueue an output. Returns NANO_OK or NANO_ERR_BUFFER_TOO_SMALL. */
static inline int rtc_enqueue_output(nano_rtc_t *rtc, const nano_output_t *out)
{
    uint8_t used = rtc->out_tail - rtc->out_head;
    if (used >= NANO_OUT_QUEUE_SIZE) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }
    rtc->out_queue[rtc->out_tail & (NANO_OUT_QUEUE_SIZE - 1)] = *out;
    rtc->out_tail++;
    return NANO_OK;
}

#endif /* NANO_RTC_INTERNAL_H_ */
