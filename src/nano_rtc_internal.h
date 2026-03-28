/*
 * nanortc — Internal state structure
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_RTC_INTERNAL_H_
#define NANORTC_RTC_INTERNAL_H_

#include "nanortc.h"
#include "nano_ice.h"
#include "nano_dtls.h"
#include "nano_sdp.h"

#if NANORTC_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#include "nano_datachannel.h"
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_rtp.h"
#include "nano_rtcp.h"
#include "nano_srtp.h"
#endif

#if NANORTC_FEATURE_AUDIO
#include "nano_jitter.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_bwe.h"
#endif

/* ----------------------------------------------------------------
 * Connection state
 * ---------------------------------------------------------------- */

typedef enum {
    NANORTC_STATE_NEW,
    NANORTC_STATE_ICE_CHECKING,
    NANORTC_STATE_ICE_CONNECTED,
    NANORTC_STATE_DTLS_HANDSHAKING,
    NANORTC_STATE_DTLS_CONNECTED,
    NANORTC_STATE_SCTP_CONNECTING,
    NANORTC_STATE_CONNECTED,
    NANORTC_STATE_CLOSED,
} nano_conn_state_t;

/* NANORTC_OUT_QUEUE_SIZE is defined in nanortc_config.h */

/* ----------------------------------------------------------------
 * Main state machine
 * ---------------------------------------------------------------- */

struct nanortc {
    nanortc_config_t config;
    nano_conn_state_t state;
    uint32_t now_ms; /* last known time */

    /* Subsystem state */
    nano_ice_t ice;
    nano_dtls_t dtls;
    nano_sdp_t sdp;

#if NANORTC_FEATURE_DATACHANNEL
    nano_sctp_t sctp;
    nano_dc_t datachannel;
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    nano_rtp_t rtp;
    nano_rtcp_t rtcp;
    nano_srtp_t srtp;
#endif

#if NANORTC_FEATURE_AUDIO
    nano_jitter_t jitter;
#endif

#if NANORTC_FEATURE_VIDEO
    nano_bwe_t bwe;
#endif

    /* Output queue (simple ring buffer) */
    nanortc_output_t out_queue[NANORTC_OUT_QUEUE_SIZE];
    uint8_t out_head;
    uint8_t out_tail;

    /* Scratch buffer for STUN encode/decode.
     * Sans I/O contract: caller must drain outputs before next handle_receive. */
    uint8_t stun_buf[NANORTC_STUN_BUF_SIZE];

    /* Scratch buffer for DTLS output polling */
    uint8_t dtls_scratch[NANORTC_DTLS_BUF_SIZE];

    /* Stored remote address for SCTP output routing */
    nanortc_addr_t remote_addr;
};

/* Enqueue an output. Returns NANORTC_OK or NANORTC_ERR_BUFFER_TOO_SMALL. */
static inline int rtc_enqueue_output(nanortc_t *rtc, const nanortc_output_t *out)
{
    uint8_t used = rtc->out_tail - rtc->out_head;
    if (used >= NANORTC_OUT_QUEUE_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    rtc->out_queue[rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1)] = *out;
    rtc->out_tail++;
    return NANORTC_OK;
}

#endif /* NANORTC_RTC_INTERNAL_H_ */
