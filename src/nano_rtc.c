/*
 * nanortc — Main state machine
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_ice.h"
#include "nano_stun.h"
#include "nano_sdp.h"
#include "nano_log.h"
#include "nanortc_util.h"

#if NANORTC_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#include "nano_datachannel.h"
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_rtp.h"
#include "nano_srtp.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_h264.h"
#endif

#include <string.h>

/* Shared hex alphabet for ICE credential generation */
static const char hex_chars[] = "0123456789abcdef";

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

/* ----------------------------------------------------------------
 * Static helpers — deduplicated from accept_offer/create_offer/accept_answer
 * ---------------------------------------------------------------- */

/* A1: Generate random ICE ufrag+pwd into SDP and ICE state */
static int rtc_generate_ice_credentials(nanortc_t *rtc)
{
    if (!rtc->config.crypto) {
        return NANORTC_OK;
    }

    /* Generate ufrag as hex (NANORTC_ICE_UFRAG_LEN/2 random bytes) */
    uint8_t rnd[NANORTC_ICE_UFRAG_LEN / 2];
    rtc->config.crypto->random_bytes(rnd, sizeof(rnd));
    for (int i = 0; i < (int)sizeof(rnd); i++) {
        rtc->sdp.local_ufrag[i * 2] = hex_chars[(rnd[i] >> 4) & 0xF];
        rtc->sdp.local_ufrag[i * 2 + 1] = hex_chars[rnd[i] & 0xF];
    }
    rtc->sdp.local_ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';

    /* Generate pwd as hex (NANORTC_ICE_PWD_LEN/2 random bytes) */
    uint8_t rnd2[NANORTC_ICE_PWD_LEN / 2];
    rtc->config.crypto->random_bytes(rnd2, sizeof(rnd2));
    for (int i = 0; i < (int)sizeof(rnd2); i++) {
        rtc->sdp.local_pwd[i * 2] = hex_chars[(rnd2[i] >> 4) & 0xF];
        rtc->sdp.local_pwd[i * 2 + 1] = hex_chars[rnd2[i] & 0xF];
    }
    rtc->sdp.local_pwd[NANORTC_ICE_PWD_LEN] = '\0';

    /* Copy to ICE state */
    memcpy(rtc->ice.local_ufrag, rtc->sdp.local_ufrag, sizeof(rtc->ice.local_ufrag));
    memcpy(rtc->ice.local_pwd, rtc->sdp.local_pwd, sizeof(rtc->ice.local_pwd));
    rtc->ice.local_ufrag_len = NANORTC_ICE_UFRAG_LEN;
    rtc->ice.local_pwd_len = NANORTC_ICE_PWD_LEN;

    return NANORTC_OK;
}

/* A5: Apply remote ICE/SCTP credentials from parsed SDP to subsystem state */
static void rtc_apply_remote_sdp(nanortc_t *rtc)
{
    /* Copy remote ICE credentials to ICE state */
    memcpy(rtc->ice.remote_ufrag, rtc->sdp.remote_ufrag, sizeof(rtc->ice.remote_ufrag));
    memcpy(rtc->ice.remote_pwd, rtc->sdp.remote_pwd, sizeof(rtc->ice.remote_pwd));
    rtc->ice.remote_ufrag_len =
        nanortc_strnlen(rtc->sdp.remote_ufrag, sizeof(rtc->sdp.remote_ufrag));
    rtc->ice.remote_pwd_len = nanortc_strnlen(rtc->sdp.remote_pwd, sizeof(rtc->sdp.remote_pwd));

#if NANORTC_FEATURE_DATACHANNEL
    /* Set SCTP remote port from SDP */
    if (rtc->sdp.remote_sctp_port > 0) {
        rtc->sctp.remote_port = rtc->sdp.remote_sctp_port;
    }
    /* Set crypto provider on SCTP for cookie generation */
    rtc->sctp.crypto = rtc->config.crypto;
#endif
}

/* A2: Auto-add ICE candidates embedded in SDP (RFC 8839) */
static void rtc_add_sdp_candidates(nanortc_t *rtc)
{
    for (uint8_t i = 0; i < rtc->sdp.candidate_count; i++) {
        const nano_sdp_candidate_t *c = &rtc->sdp.remote_candidates[i];
        char cand_str[NANORTC_IPV6_STR_SIZE + 16];
        size_t addr_len = 0;
        while (c->addr[addr_len] && addr_len < NANORTC_IPV6_STR_SIZE)
            addr_len++;
        /* Format: "<addr> <port>" (simple format) */
        if (addr_len + 8 < sizeof(cand_str)) {
            memcpy(cand_str, c->addr, addr_len);
            cand_str[addr_len] = ' ';
            /* Convert port to decimal string manually */
            size_t pos = addr_len + 1;
            char tmp[8];
            int ti = 0;
            uint16_t v = c->port;
            if (v == 0) {
                tmp[ti++] = '0';
            } else {
                char rev[8];
                int ri = 0;
                while (v > 0) {
                    rev[ri++] = '0' + (v % 10);
                    v /= 10;
                }
                while (ri > 0)
                    tmp[ti++] = rev[--ri];
            }
            memcpy(cand_str + pos, tmp, (size_t)ti);
            cand_str[pos + (size_t)ti] = '\0';
            nanortc_add_remote_candidate(rtc, cand_str);
        }
    }
}

/* A3: Drain DTLS output into the transmit queue */
static void rtc_drain_dtls_output(nanortc_t *rtc, const nanortc_addr_t *dest)
{
    size_t dout_len = 0;
    while (dtls_poll_output(&rtc->dtls, rtc->dtls_scratch, sizeof(rtc->dtls_scratch), &dout_len) ==
               NANORTC_OK &&
           dout_len > 0) {
        nanortc_output_t tout;
        memset(&tout, 0, sizeof(tout));
        tout.type = NANORTC_OUTPUT_TRANSMIT;
        tout.transmit.data = rtc->dtls_scratch;
        tout.transmit.len = dout_len;
        tout.transmit.dest = *dest;
        rtc_enqueue_output(rtc, &tout);
        dout_len = 0;
    }
}

/* A4: Emit an event with optional stream_id */
static int rtc_emit_event(nanortc_t *rtc, nanortc_event_type_t type, uint16_t stream_id)
{
    nanortc_output_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = NANORTC_OUTPUT_EVENT;
    evt.event.type = type;
    evt.event.stream_id = stream_id;
    return rtc_enqueue_output(rtc, &evt);
}

int nanortc_init(nanortc_t *rtc, const nanortc_config_t *cfg)
{
    if (!rtc || !cfg) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    memset(rtc, 0, sizeof(*rtc));
    rtc->config = *cfg;
    rtc->state = NANORTC_STATE_NEW;

    /* Initialize logging (process-global callback) */
    nano_log_init(&cfg->log);

    NANORTC_LOGI("RTC", "nanortc_init");

    ice_init(&rtc->ice, cfg->role == NANORTC_ROLE_CONTROLLING);
    /* DTLS context is created early in accept_offer (for SDP fingerprint);
     * handshake starts when ICE connects. */
    sdp_init(&rtc->sdp);

#if NANORTC_FEATURE_DATACHANNEL
    nsctp_init(&rtc->sctp);
    dc_init(&rtc->datachannel);
#endif

    /* Default DTLS setup from ICE role (overridden by SDP negotiation in accept_offer) */
    if (cfg->role == NANORTC_ROLE_CONTROLLING) {
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    }

#if NANORTC_HAVE_MEDIA_TRANSPORT
    rtp_init(&rtc->rtp, 0, 0);
    rtcp_init(&rtc->rtcp, 0);
    srtp_init(&rtc->srtp, cfg->crypto, 0);
#endif

#if NANORTC_FEATURE_AUDIO
    jitter_init(&rtc->jitter, cfg->jitter_depth_ms);
    if (cfg->audio_codec != NANORTC_CODEC_NONE) {
        rtc->sdp.has_audio = true;
        rtc->sdp.audio_pt = 111; /* Opus standard dynamic PT */
        rtc->sdp.audio_sample_rate = cfg->audio_sample_rate;
        rtc->sdp.audio_channels = cfg->audio_channels;
        rtc->sdp.audio_direction = cfg->audio_direction;
    }
#endif

#if NANORTC_FEATURE_VIDEO
    bwe_init(&rtc->bwe);
    h264_depkt_init(&rtc->h264_depkt);
    if (cfg->video_codec != NANORTC_CODEC_NONE) {
        /* Generate a random SSRC for video (different from audio) */
        uint32_t video_ssrc = 0;
        if (cfg->crypto) {
            uint8_t r[4];
            cfg->crypto->random_bytes(r, 4);
            video_ssrc = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
        }
        rtp_init(&rtc->video_rtp, video_ssrc, NANORTC_VIDEO_DEFAULT_PT);
        rtc->sdp.has_video = true;
        rtc->sdp.video_pt = NANORTC_VIDEO_DEFAULT_PT;
        rtc->sdp.video_direction = cfg->video_direction;
    }
#endif

    return NANORTC_OK;
}

void nanortc_destroy(nanortc_t *rtc)
{
    if (!rtc) {
        return;
    }
    NANORTC_LOGI("RTC", "nanortc_destroy");
    dtls_destroy(&rtc->dtls);
    nano_log_cleanup();
    rtc->state = NANORTC_STATE_CLOSED;
}

#if NANORTC_HAVE_MEDIA_TRANSPORT
/* Compute RFC 3264 §6 direction complement:
 * recvonly ↔ sendonly, inactive → inactive, sendrecv → sendrecv. */
static nanortc_direction_t direction_complement(nanortc_direction_t remote)
{
    switch (remote) {
    case NANORTC_DIR_RECVONLY:
        return NANORTC_DIR_SENDONLY;
    case NANORTC_DIR_SENDONLY:
        return NANORTC_DIR_RECVONLY;
    case NANORTC_DIR_INACTIVE:
        return NANORTC_DIR_INACTIVE;
    default:
        return NANORTC_DIR_SENDRECV;
    }
}
#endif

/* Apply negotiated media parameters from parsed SDP to subsystem state.
 * Called from both accept_offer (answerer) and accept_answer (offerer). */
static void rtc_apply_negotiated_media(nanortc_t *rtc)
{
#if NANORTC_HAVE_MEDIA_TRANSPORT
    if (rtc->sdp.has_audio) {
        rtc->sdp.audio_direction = direction_complement(rtc->sdp.remote_audio_direction);
    }
    if (rtc->sdp.has_video) {
        rtc->sdp.video_direction = direction_complement(rtc->sdp.remote_video_direction);
    }
#endif

#if NANORTC_FEATURE_VIDEO
    /* Update video RTP payload type to match negotiated PT */
    if (rtc->sdp.has_video && rtc->sdp.video_pt != 0) {
        rtc->video_rtp.payload_type = rtc->sdp.video_pt;
    }
#endif
    (void)rtc;
}

/* Cache DTLS fingerprint with "sha-256 " prefix into SDP state (RFC 8122 §5) */
static void rtc_cache_fingerprint(nanortc_t *rtc)
{
    if (rtc->sdp.local_fingerprint[0] != '\0')
        return;
    const char *fp = dtls_get_fingerprint(&rtc->dtls);
    if (!fp)
        return;
    size_t fplen = nanortc_strnlen(fp, sizeof(rtc->dtls.local_fingerprint));
    if (8 + fplen < sizeof(rtc->sdp.local_fingerprint)) {
        memcpy(rtc->sdp.local_fingerprint, "sha-256 ", 8);
        memcpy(rtc->sdp.local_fingerprint + 8, fp, fplen + 1);
    }
}

int nanortc_accept_offer(nanortc_t *rtc, const char *offer, char *answer_buf, size_t answer_buf_len,
                         size_t *out_len)
{
    if (!rtc || !offer || !answer_buf) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    size_t offer_len = strlen(offer); /* NANORTC_SAFE: API boundary */

    /* Parse remote SDP */
    int rc = sdp_parse(&rtc->sdp, offer, offer_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    rtc_apply_remote_sdp(rtc);
    rtc_generate_ice_credentials(rtc);

    rtc_apply_negotiated_media(rtc);

    /* Determine DTLS role from remote setup (RFC 8842 §5.2) */
    if (rtc->sdp.remote_setup == NANORTC_SDP_SETUP_ACTIVE) {
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    } else if (rtc->sdp.remote_setup == NANORTC_SDP_SETUP_PASSIVE) {
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    } else {
        /* Remote is actpass (offerer default) — answerer chooses passive */
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    }

    /* Early DTLS init: create certificate for SDP fingerprint (RFC 8827 §5) */
    if (rtc->config.crypto && !rtc->dtls.crypto_ctx) {
        int is_dtls_server = (rtc->sdp.local_setup == NANORTC_SDP_SETUP_PASSIVE);
        int drc = dtls_init(&rtc->dtls, rtc->config.crypto, is_dtls_server);
        if (drc != NANORTC_OK) {
            return drc;
        }
        rtc_cache_fingerprint(rtc);
    }

    /* Generate answer SDP */
    size_t answer_len = 0;
    rc = sdp_generate_answer(&rtc->sdp, answer_buf, answer_buf_len, &answer_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    if (out_len) {
        *out_len = answer_len;
    }

    rtc_add_sdp_candidates(rtc);

    NANORTC_LOGI("RTC", "offer accepted, answer generated");
    return NANORTC_OK;
}

int nanortc_create_offer(nanortc_t *rtc, char *offer_buf, size_t offer_buf_len, size_t *out_len)
{
    if (!rtc || !offer_buf) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANORTC_STATE_NEW) {
        return NANORTC_ERR_STATE;
    }

    rtc_generate_ice_credentials(rtc);

    /* Offerer is DTLS active, setup=actpass in SDP (RFC 8842) */
    rtc->sdp.local_setup = NANORTC_SDP_SETUP_ACTPASS;

    /* Early DTLS init — need certificate fingerprint for SDP.
     * Role is tentative (client); accept_answer() finalizes via dtls_set_role. */
    if (rtc->config.crypto && !rtc->dtls.crypto_ctx) {
        int drc = dtls_init(&rtc->dtls, rtc->config.crypto, 0);
        if (drc != NANORTC_OK) {
            return drc;
        }
        rtc_cache_fingerprint(rtc);
    }

    /* Generate offer SDP (reuse answer generator — offer format is the same for WebRTC) */
    size_t offer_len = 0;
    int rc = sdp_generate_answer(&rtc->sdp, offer_buf, offer_buf_len, &offer_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    if (out_len) {
        *out_len = offer_len;
    }

    NANORTC_LOGI("RTC", "offer created");
    return NANORTC_OK;
}

int nanortc_accept_answer(nanortc_t *rtc, const char *answer)
{
    if (!rtc || !answer) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* B1: State guard — must have called create_offer first */
    if (rtc->state != NANORTC_STATE_NEW) {
        return NANORTC_ERR_STATE;
    }

    size_t answer_len = strlen(answer); /* NANORTC_SAFE: API boundary */

    /* Parse remote SDP answer */
    int rc = sdp_parse(&rtc->sdp, answer, answer_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    rtc_apply_remote_sdp(rtc);

    /* Determine DTLS role from answer's setup attribute.
     * Offerer sent actpass; answerer picks active or passive. */
    if (rtc->sdp.remote_setup == NANORTC_SDP_SETUP_PASSIVE) {
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    } else if (rtc->sdp.remote_setup == NANORTC_SDP_SETUP_ACTIVE) {
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    } else {
        /* B2: Answerer returned actpass — invalid per RFC 8842 §5.2.
         * Both sides would become server. Log warning and default to active. */
        NANORTC_LOGW("RTC", "remote answer has setup:actpass, forcing local active");
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    }

    /* Finalize DTLS role based on negotiated SDP setup attribute.
     * create_offer() inits DTLS early (for fingerprint) with a tentative role;
     * the answer determines the actual role. dtls_set_role switches the crypto
     * provider's internal state machine without regenerating the certificate. */
    if (rtc->dtls.crypto_ctx) {
        int is_server = (rtc->sdp.local_setup == NANORTC_SDP_SETUP_PASSIVE);
        if (rtc->config.crypto->dtls_set_role) {
            rtc->config.crypto->dtls_set_role((nanortc_crypto_dtls_ctx_t *)rtc->dtls.crypto_ctx,
                                              is_server);
        }
        rtc->dtls.is_server = is_server;
    }

    rtc_apply_negotiated_media(rtc);
    rtc_add_sdp_candidates(rtc);

    NANORTC_LOGI("RTC", "answer accepted");
    return NANORTC_OK;
}

int nanortc_add_local_candidate(nanortc_t *rtc, const char *ip, uint16_t port)
{
    if (!rtc || !ip) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Store in SDP state for answer generation (a=candidate: line) */
    size_t ip_len = strlen(ip); /* NANORTC_SAFE: API boundary */
    if (ip_len >= sizeof(rtc->sdp.local_candidate_ip)) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(rtc->sdp.local_candidate_ip, ip, ip_len + 1);
    rtc->sdp.local_candidate_port = port;
    rtc->sdp.has_local_candidate = true;

    NANORTC_LOGI("RTC", "local candidate added");
    return NANORTC_OK;
}

int nanortc_add_remote_candidate(nanortc_t *rtc, const char *candidate_str)
{
    if (!rtc || !candidate_str) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /*
     * Parse SDP candidate attribute (RFC 8839 §5.1):
     *   "candidate:<foundation> <component> UDP <priority> <addr> <port> typ <type>"
     * We need fields 5 (addr) and 6 (port), 1-indexed from "candidate:".
     * Also accept plain "<addr> <port>" for simple use cases.
     */
    const char *p = candidate_str;

    /* Skip "candidate:" prefix if present */
    const char *prefix = "candidate:";
    size_t pfx_len = 10;
    bool has_prefix = true;
    for (size_t i = 0; i < pfx_len; i++) {
        if (p[i] == '\0' || p[i] != prefix[i]) {
            has_prefix = false;
            break;
        }
    }

    const char *addr_str = NULL;
    size_t addr_len = 0;
    uint16_t port = 0;

    if (has_prefix) {
        /* SDP format: skip to field 5 (addr) and field 6 (port) */
        p += pfx_len;
        int field = 1;
        while (*p && field < 5) {
            if (*p == ' ') {
                field++;
                while (*p == ' ')
                    p++;
            } else {
                p++;
            }
        }
        /* p now points to addr field */
        addr_str = p;
        while (*p && *p != ' ')
            p++;
        addr_len = (size_t)(p - addr_str);

        /* Skip to port field */
        while (*p == ' ')
            p++;
        /* Parse port */
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (uint16_t)(*p - '0');
            p++;
        }
    } else {
        /* Simple format: "<addr> <port>" */
        addr_str = candidate_str;
        while (*p && *p != ' ')
            p++;
        addr_len = (size_t)(p - addr_str);
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (uint16_t)(*p - '0');
            p++;
        }
    }

    if (addr_len == 0 || addr_len >= NANORTC_IPV6_STR_SIZE || port == 0) {
        return NANORTC_ERR_PARSE;
    }

    /* Parse IPv4 address (simple: a.b.c.d) */
    char addr_buf[NANORTC_IPV6_STR_SIZE];
    memcpy(addr_buf, addr_str, addr_len);
    addr_buf[addr_len] = '\0';

    uint8_t ip[4] = {0};
    int octet = 0;
    uint16_t val = 0;
    for (size_t i = 0; i <= addr_len; i++) {
        if (i == addr_len || addr_buf[i] == '.') {
            if (octet >= 4 || val > 255) {
                return NANORTC_ERR_PARSE;
            }
            ip[octet++] = (uint8_t)val;
            val = 0;
        } else if (addr_buf[i] >= '0' && addr_buf[i] <= '9') {
            val = val * 10 + (uint16_t)(addr_buf[i] - '0');
        } else {
            /* Not a simple IPv4 — could be IPv6, not yet supported */
            return NANORTC_ERR_NOT_IMPLEMENTED;
        }
    }

    if (octet != 4) {
        return NANORTC_ERR_PARSE;
    }

    /* Store in ICE remote candidates array */
    if (rtc->ice.remote_candidate_count >= NANORTC_MAX_ICE_CANDIDATES) {
        NANORTC_LOGW("RTC", "remote candidate table full");
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    uint8_t idx = rtc->ice.remote_candidate_count;
    rtc->ice.remote_candidates[idx].family = 4;
    memset(rtc->ice.remote_candidates[idx].addr, 0, NANORTC_ADDR_SIZE);
    memcpy(rtc->ice.remote_candidates[idx].addr, ip, 4);
    rtc->ice.remote_candidates[idx].port = port;
    rtc->ice.remote_candidate_count++;

    NANORTC_LOGI("RTC", "remote candidate added");
    return NANORTC_OK;
}

int nanortc_poll_output(nanortc_t *rtc, nanortc_output_t *out)
{
    if (!rtc || !out) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->out_head == rtc->out_tail) {
        return NANORTC_ERR_NO_DATA;
    }
    *out = rtc->out_queue[rtc->out_head & (NANORTC_OUT_QUEUE_SIZE - 1)];
    rtc->out_head++;
    return NANORTC_OK;
}

/* Init DTLS (if needed) and begin handshake after ICE connects */
static int rtc_begin_dtls_handshake(nanortc_t *rtc, const nanortc_addr_t *src)
{
    int is_server = (rtc->sdp.local_setup == NANORTC_SDP_SETUP_PASSIVE);

    /* accept_offer() does early init; this guard covers create_offer() path */
    if (!rtc->dtls.crypto_ctx) {
        int rc = dtls_init(&rtc->dtls, rtc->config.crypto, is_server);
        if (rc != NANORTC_OK)
            return rc;
    }

    rtc->state = NANORTC_STATE_DTLS_HANDSHAKING;

    if (!is_server) {
        int rc = dtls_start(&rtc->dtls);
        if (rc != NANORTC_OK)
            return rc;
        rtc_drain_dtls_output(rtc, src);
    }
    return NANORTC_OK;
}

#if NANORTC_FEATURE_DATACHANNEL
/* ----------------------------------------------------------------
 * Internal: drain SCTP output through DTLS encrypt → transmit queue
 * ---------------------------------------------------------------- */

static void rtc_pump_sctp_through_dtls(nanortc_t *rtc, const nanortc_addr_t *dest)
{
    size_t nsctp_out = 0;
    uint8_t nsctp_buf[NANORTC_SCTP_MTU];
    while (nsctp_poll_output(&rtc->sctp, nsctp_buf, sizeof(nsctp_buf), &nsctp_out) == NANORTC_OK &&
           nsctp_out > 0) {
        dtls_encrypt(&rtc->dtls, nsctp_buf, nsctp_out);
        size_t enc_len = 0;
        while (dtls_poll_output(&rtc->dtls, rtc->dtls_scratch, sizeof(rtc->dtls_scratch),
                                &enc_len) == NANORTC_OK &&
               enc_len > 0) {
            nanortc_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANORTC_OUTPUT_TRANSMIT;
            tout.transmit.data = rtc->dtls_scratch;
            tout.transmit.len = enc_len;
            tout.transmit.dest = *dest;
            rtc_enqueue_output(rtc, &tout);
            enc_len = 0;
        }
        nsctp_out = 0;
    }
}
#endif /* NANORTC_FEATURE_DATACHANNEL */

/* ----------------------------------------------------------------
 * nanortc_handle_receive — RFC 7983 demux
 * ---------------------------------------------------------------- */

int nanortc_handle_receive(nanortc_t *rtc, uint32_t now_ms, const uint8_t *data, size_t len,
                           const nanortc_addr_t *src)
{
    if (!rtc || !data || len == 0 || !src) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    rtc->now_ms = now_ms;
    uint8_t first = data[0];

    /* RFC 7983 §3: demultiplexing by first byte */
    if (first <= 3) {
        /* STUN [0x00-0x03] */
        size_t resp_len = 0;
        int rc = ice_handle_stun(&rtc->ice, data, len, src, rtc->config.crypto, rtc->stun_buf,
                                 sizeof(rtc->stun_buf), &resp_len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        /* Enqueue STUN response for transmission */
        if (resp_len > 0) {
            nanortc_output_t out;
            memset(&out, 0, sizeof(out));
            out.type = NANORTC_OUTPUT_TRANSMIT;
            out.transmit.data = rtc->stun_buf;
            out.transmit.len = resp_len;
            out.transmit.dest = *src; /* reply to sender */
            rtc_enqueue_output(rtc, &out);
        }

        /* Check for ICE state transition → init DTLS + emit event */
        if (rtc->ice.state == NANORTC_ICE_STATE_CONNECTED &&
            rtc->state < NANORTC_STATE_ICE_CONNECTED) {
            rtc->state = NANORTC_STATE_ICE_CONNECTED;
            rtc_emit_event(rtc, NANORTC_EVENT_ICE_CONNECTED, 0);
            /* B3: Also emit generic ICE_STATE_CHANGE */
            rtc_emit_event(rtc, NANORTC_EVENT_ICE_STATE_CHANGE,
                           (uint16_t)NANORTC_ICE_STATE_CONNECTED);

            int drc = rtc_begin_dtls_handshake(rtc, src);
            if (drc != NANORTC_OK) {
                return drc;
            }
        }

        return NANORTC_OK;

    } else if (first >= 20 && first <= 63) {
        /* DTLS [0x14-0x3F] — RFC 6347 */
        if (rtc->state < NANORTC_STATE_ICE_CONNECTED) {
            return NANORTC_ERR_STATE; /* ICE must complete first */
        }

        int drc = dtls_handle_data(&rtc->dtls, data, len);
        if (drc < 0) {
            return drc;
        }

        /* Drain DTLS output into transmit queue */
        rtc_drain_dtls_output(rtc, src);

        /* Check for DTLS state transition → emit event */
        if (rtc->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED &&
            rtc->state < NANORTC_STATE_DTLS_CONNECTED) {
            rtc->state = NANORTC_STATE_DTLS_CONNECTED;
            rtc->remote_addr = *src; /* save for timeout-driven output */
            rtc_emit_event(rtc, NANORTC_EVENT_DTLS_CONNECTED, 0);

            rtc_cache_fingerprint(rtc);

#if NANORTC_HAVE_MEDIA_TRANSPORT
            /* Derive SRTP keys from DTLS keying material (RFC 5764 §4.2) */
            if (rtc->dtls.keying_material_ready) {
                int is_client = !rtc->dtls.is_server;
                srtp_init(&rtc->srtp, rtc->config.crypto, is_client);
                srtp_derive_keys(&rtc->srtp, rtc->dtls.keying_material, NANORTC_DTLS_KEYING_SIZE);
                /* Random SSRC + negotiated PT (RFC 3550 §5.1) */
                uint32_t ssrc = 0;
                uint16_t init_seq = 0;
                if (rtc->config.crypto) {
                    uint8_t rnd[6];
                    rtc->config.crypto->random_bytes(rnd, 6);
                    ssrc = nanortc_read_u32be(rnd);
                    init_seq = nanortc_read_u16be(rnd + 4);
                }
                rtp_init(&rtc->rtp, ssrc, rtc->sdp.has_audio ? rtc->sdp.audio_pt : 0);
                rtc->rtp.seq = init_seq; /* RFC 3550: random initial seq */
                NANORTC_LOGI("RTC", "SRTP keys derived, RTP ready");
            }
#endif

#if NANORTC_FEATURE_DATACHANNEL
            /* Initiate SCTP: DTLS client sends INIT (RFC 8831) */
            if (!rtc->dtls.is_server) {
                nsctp_start(&rtc->sctp);
                rtc->state = NANORTC_STATE_SCTP_CONNECTING;

                /* Drain SCTP output (INIT) through DTLS encrypt */
                rtc_pump_sctp_through_dtls(rtc, src);
            }
#else
            /* No DataChannel — DTLS connected is final state */
            rtc->state = NANORTC_STATE_CONNECTED;
#endif
        }

#if NANORTC_FEATURE_DATACHANNEL
        /* If DTLS is established, check for decrypted app data → SCTP */
        if (rtc->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED) {
            const uint8_t *app_data = NULL;
            size_t app_len = 0;
            while (dtls_poll_app_data(&rtc->dtls, &app_data, &app_len) == NANORTC_OK &&
                   app_len > 0) {
                /* Feed decrypted data to SCTP */
                nsctp_handle_data(&rtc->sctp, app_data, app_len);

                /* Check for SCTP state transition */
                if (rtc->sctp.state == NANORTC_SCTP_STATE_ESTABLISHED &&
                    rtc->state < NANORTC_STATE_CONNECTED) {
                    rtc->state = NANORTC_STATE_CONNECTED;
                    rtc_emit_event(rtc, NANORTC_EVENT_SCTP_CONNECTED, 0);
                    NANORTC_LOGI("RTC", "SCTP established");
                }

                /* Deliver SCTP payload via DataChannel */
                if (rtc->sctp.has_delivered) {
                    dc_handle_message(&rtc->datachannel, rtc->sctp.delivered_stream,
                                      rtc->sctp.delivered_ppid, rtc->sctp.delivered_data,
                                      rtc->sctp.delivered_len);
                    rtc->sctp.has_delivered = false;

                    /* Emit DC events for data messages */
                    nanortc_event_type_t etype;
                    if (rtc->sctp.delivered_ppid == DCEP_PPID_STRING ||
                        rtc->sctp.delivered_ppid == DCEP_PPID_STRING_EMPTY) {
                        etype = NANORTC_EVENT_DATACHANNEL_STRING;
                    } else if (rtc->sctp.delivered_ppid == DCEP_PPID_BINARY ||
                               rtc->sctp.delivered_ppid == DCEP_PPID_BINARY_EMPTY) {
                        etype = NANORTC_EVENT_DATACHANNEL_DATA;
                    } else if (rtc->sctp.delivered_ppid == DCEP_PPID_CONTROL) {
                        etype = NANORTC_EVENT_DATACHANNEL_OPEN;
                    } else {
                        etype = NANORTC_EVENT_DATACHANNEL_DATA; /* fallback */
                    }

                    {
                        nanortc_output_t devt2;
                        memset(&devt2, 0, sizeof(devt2));
                        devt2.type = NANORTC_OUTPUT_EVENT;
                        devt2.event.type = etype;
                        devt2.event.stream_id = rtc->sctp.delivered_stream;
                        devt2.event.data = rtc->sctp.delivered_data;
                        devt2.event.len = rtc->sctp.delivered_len;
                        /* Attach label for OPEN events */
                        if (etype == NANORTC_EVENT_DATACHANNEL_OPEN) {
                            for (uint8_t ci = 0; ci < rtc->datachannel.channel_count; ci++) {
                                if (rtc->datachannel.channels[ci].stream_id ==
                                    rtc->sctp.delivered_stream) {
                                    devt2.event.label = rtc->datachannel.channels[ci].label;
                                    break;
                                }
                            }
                        }
                        rtc_enqueue_output(rtc, &devt2);
                    }
                }

                /* Drain SCTP output (SACK, handshake) through DTLS */
                rtc_pump_sctp_through_dtls(rtc, src);

                /* Also drain DC output (DCEP ACK) → SCTP → DTLS */
                uint8_t dc_buf[NANORTC_DC_OUT_BUF_SIZE];
                size_t dc_len = 0;
                uint16_t dc_stream = 0;
                while (dc_poll_output(&rtc->datachannel, dc_buf, sizeof(dc_buf), &dc_len,
                                      &dc_stream) == NANORTC_OK &&
                       dc_len > 0) {
                    nsctp_send(&rtc->sctp, dc_stream, DCEP_PPID_CONTROL, dc_buf, dc_len);
                    rtc_pump_sctp_through_dtls(rtc, src);
                    dc_len = 0;
                }

                app_len = 0;
            }
        }
#endif /* NANORTC_FEATURE_DATACHANNEL */

        return NANORTC_OK;

    } else if (first >= 128 && first <= 191) {
        /* RTP/RTCP [0x80-0xBF] — silently consumed.
         * Full receive-path decoding (PLI, NACK, audio/video) deferred. */
        return NANORTC_OK;
    }

    return NANORTC_ERR_PROTOCOL; /* Unknown packet type */
}

/* ----------------------------------------------------------------
 * nanortc_handle_timeout — timer-driven state transitions
 * ---------------------------------------------------------------- */

int nanortc_handle_timeout(nanortc_t *rtc, uint32_t now_ms)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    rtc->now_ms = now_ms;

    /* ICE: generate connectivity checks (controlling role) */
    if (rtc->ice.is_controlling && rtc->ice.state != NANORTC_ICE_STATE_CONNECTED &&
        rtc->ice.state != NANORTC_ICE_STATE_FAILED) {
        nano_ice_state_t prev_ice = rtc->ice.state;
        uint8_t ci_before = rtc->ice.current_candidate;
        size_t out_len = 0;
        int rc = ice_generate_check(&rtc->ice, now_ms, rtc->config.crypto, rtc->stun_buf,
                                    sizeof(rtc->stun_buf), &out_len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        if (out_len > 0 && ci_before < rtc->ice.remote_candidate_count) {
            uint8_t ci = ci_before;
            nanortc_output_t out;
            memset(&out, 0, sizeof(out));
            out.type = NANORTC_OUTPUT_TRANSMIT;
            out.transmit.data = rtc->stun_buf;
            out.transmit.len = out_len;
            /* Destination: current remote candidate */
            out.transmit.dest.family = rtc->ice.remote_candidates[ci].family;
            memcpy(out.transmit.dest.addr, rtc->ice.remote_candidates[ci].addr, NANORTC_ADDR_SIZE);
            out.transmit.dest.port = rtc->ice.remote_candidates[ci].port;
            rtc_enqueue_output(rtc, &out);
        }

        /* B3: Emit ICE_STATE_CHANGE on transition to CHECKING */
        if (rtc->ice.state != prev_ice && rtc->ice.state == NANORTC_ICE_STATE_CHECKING) {
            rtc_emit_event(rtc, NANORTC_EVENT_ICE_STATE_CHANGE,
                           (uint16_t)NANORTC_ICE_STATE_CHECKING);
        }

        /* Schedule next timeout */
        if (rtc->ice.state == NANORTC_ICE_STATE_CHECKING) {
            nanortc_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANORTC_OUTPUT_TIMEOUT;
            tout.timeout_ms = rtc->ice.check_interval_ms;
            rtc_enqueue_output(rtc, &tout);
        }

        /* Propagate ICE failure */
        if (rtc->ice.state == NANORTC_ICE_STATE_FAILED) {
            rtc_emit_event(rtc, NANORTC_EVENT_ICE_STATE_CHANGE, (uint16_t)NANORTC_ICE_STATE_FAILED);
            rtc->state = NANORTC_STATE_CLOSED;
        }
    }

#if NANORTC_FEATURE_DATACHANNEL
    /* SCTP: retransmission + heartbeat timers */
    if (rtc->sctp.state == NANORTC_SCTP_STATE_ESTABLISHED) {
        nsctp_handle_timeout(&rtc->sctp, now_ms);

        /* Pump any SCTP output (retransmits, heartbeats, pending DATA) through DTLS */
        rtc_pump_sctp_through_dtls(rtc, &rtc->remote_addr);
    }
#endif

    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * DataChannel API
 * ---------------------------------------------------------------- */

#if NANORTC_FEATURE_DATACHANNEL

/* Allocate the next even (local-initiated) stream ID.
 * RFC 8832 §6.4: DTLS client uses even stream IDs (0, 2, 4, ...),
 * DTLS server uses odd (1, 3, 5, ...). */
static uint16_t rtc_alloc_stream_id(nanortc_t *rtc)
{
    uint16_t base = rtc->dtls.is_server ? 1 : 0;
    uint16_t max_id = base;
    for (uint8_t i = 0; i < rtc->datachannel.channel_count; i++) {
        uint16_t sid = rtc->datachannel.channels[i].stream_id;
        if ((sid % 2) == (base % 2) && sid >= max_id) {
            max_id = sid + 2;
        }
    }
    return max_id;
}

int nanortc_create_datachannel(nanortc_t *rtc, const nanortc_datachannel_config_t *cfg,
                               uint16_t *stream_id)
{
    if (!rtc || !cfg || !cfg->label || !stream_id) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANORTC_STATE_CONNECTED) {
        return NANORTC_ERR_STATE;
    }

    uint16_t sid = rtc_alloc_stream_id(rtc);
    int rc = dc_open(&rtc->datachannel, sid, cfg->label);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* Drain DCEP OPEN → SCTP → DTLS → output queue */
    uint8_t dc_buf[NANORTC_DC_OUT_BUF_SIZE];
    size_t dc_len = 0;
    uint16_t dc_stream = 0;
    while (dc_poll_output(&rtc->datachannel, dc_buf, sizeof(dc_buf), &dc_len, &dc_stream) ==
               NANORTC_OK &&
           dc_len > 0) {
        nsctp_send(&rtc->sctp, dc_stream, DCEP_PPID_CONTROL, dc_buf, dc_len);
        rtc_pump_sctp_through_dtls(rtc, &rtc->remote_addr);
        dc_len = 0;
    }

    *stream_id = sid;
    NANORTC_LOGI("RTC", "datachannel created");
    return NANORTC_OK;
}

int nanortc_close_datachannel(nanortc_t *rtc, uint16_t stream_id)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Find channel and mark closed */
    nano_dc_channel_t *ch = NULL;
    for (uint8_t i = 0; i < rtc->datachannel.channel_count; i++) {
        if (rtc->datachannel.channels[i].stream_id == stream_id &&
            rtc->datachannel.channels[i].state != NANORTC_DC_STATE_CLOSED) {
            ch = &rtc->datachannel.channels[i];
            break;
        }
    }
    if (!ch) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    ch->state = NANORTC_DC_STATE_CLOSED;
    rtc_emit_event(rtc, NANORTC_EVENT_DATACHANNEL_CLOSE, stream_id);

    NANORTC_LOGI("RTC", "datachannel closed");
    return NANORTC_OK;
}

int nanortc_send_datachannel(nanortc_t *rtc, uint16_t stream_id, const void *data, size_t len)
{
    if (!rtc || !data) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANORTC_STATE_CONNECTED) {
        return NANORTC_ERR_STATE;
    }

    uint32_t ppid = (len > 0) ? DCEP_PPID_BINARY : DCEP_PPID_BINARY_EMPTY;
    int rc = nsctp_send(&rtc->sctp, stream_id, ppid, (const uint8_t *)data, len);
    if (rc == NANORTC_ERR_BUFFER_TOO_SMALL) {
        return NANORTC_ERR_WOULD_BLOCK; /* send queue full — backpressure */
    }
    return rc;
}

int nanortc_send_datachannel_string(nanortc_t *rtc, uint16_t stream_id, const char *str)
{
    if (!rtc || !str) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANORTC_STATE_CONNECTED) {
        return NANORTC_ERR_STATE;
    }

    size_t len = strlen(str); /* NANORTC_SAFE: API boundary */

    uint32_t ppid = (len > 0) ? DCEP_PPID_STRING : DCEP_PPID_STRING_EMPTY;
    int rc = nsctp_send(&rtc->sctp, stream_id, ppid, (const uint8_t *)str, len);
    if (rc == NANORTC_ERR_BUFFER_TOO_SMALL) {
        return NANORTC_ERR_WOULD_BLOCK; /* send queue full — backpressure */
    }
    return rc;
}

int nanortc_get_datachannel_label(nanortc_t *rtc, uint16_t stream_id, const char **label)
{
    if (!rtc || !label) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    for (uint8_t i = 0; i < rtc->datachannel.channel_count; i++) {
        if (rtc->datachannel.channels[i].stream_id == stream_id &&
            rtc->datachannel.channels[i].state != NANORTC_DC_STATE_CLOSED) {
            *label = rtc->datachannel.channels[i].label;
            return NANORTC_OK;
        }
    }
    return NANORTC_ERR_INVALID_PARAM;
}
#endif /* NANORTC_FEATURE_DATACHANNEL */

/* ----------------------------------------------------------------
 * Connection state API
 * ---------------------------------------------------------------- */

nano_conn_state_t nanortc_get_state(const nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_STATE_CLOSED;
    }
    return rtc->state;
}

int nanortc_close(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state == NANORTC_STATE_CLOSED || rtc->state == NANORTC_STATE_NEW) {
        return NANORTC_ERR_STATE;
    }

#if NANORTC_FEATURE_DATACHANNEL
    /* Send SCTP SHUTDOWN if association is established */
    if (rtc->sctp.state == NANORTC_SCTP_STATE_ESTABLISHED) {
        /* Encode SHUTDOWN chunk — cumulative TSN of received data */
        uint8_t shutdown_pkt[64];
        size_t hdr_len = nsctp_encode_header(shutdown_pkt, rtc->sctp.local_port,
                                             rtc->sctp.remote_port, rtc->sctp.remote_vtag);
        size_t chunk_len = nsctp_encode_shutdown(shutdown_pkt + hdr_len, rtc->sctp.cumulative_tsn);
        nsctp_finalize_checksum(shutdown_pkt, hdr_len + chunk_len);

        /* Encrypt through DTLS and enqueue */
        dtls_encrypt(&rtc->dtls, shutdown_pkt, hdr_len + chunk_len);
        rtc_drain_dtls_output(rtc, &rtc->remote_addr);
        rtc->sctp.state = NANORTC_SCTP_STATE_SHUTDOWN_SENT;
    }
#endif

    /* Send DTLS close_notify */
    dtls_close(&rtc->dtls);
    rtc_drain_dtls_output(rtc, &rtc->remote_addr);

    rtc->state = NANORTC_STATE_CLOSED;
    rtc_emit_event(rtc, NANORTC_EVENT_DISCONNECTED, 0);

    NANORTC_LOGI("RTC", "graceful close initiated");
    return NANORTC_OK;
}

#if NANORTC_FEATURE_AUDIO
int nanortc_send_audio(nanortc_t *rtc, uint32_t timestamp, const void *data, size_t len)
{
    if (!rtc || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED) {
        return NANORTC_ERR_STATE;
    }
    if (!rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    /* RTP pack into media_buf */
    size_t rtp_len = 0;
    int rc = rtp_pack(&rtc->rtp, timestamp, (const uint8_t *)data, len, rtc->media_buf,
                      sizeof(rtc->media_buf), &rtp_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* SRTP protect (in-place, appends 10B auth tag) */
    size_t srtp_len = 0;
    rc = srtp_protect(&rtc->srtp, rtc->media_buf, rtp_len, &srtp_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* Enqueue for transmission */
    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = rtc->media_buf;
    out.transmit.len = srtp_len;
    out.transmit.dest = rtc->remote_addr;
    return rtc_enqueue_output(rtc, &out);
}
#endif

#if NANORTC_FEATURE_VIDEO

/* Context for h264_packetize callback → RTP pack + SRTP protect + enqueue */
typedef struct {
    nanortc_t *rtc;
    uint32_t timestamp;
    int last_rc;
    int is_last_nal; /* true if this is the last NAL in the access unit */
} video_send_ctx_t;

static int video_send_fragment_cb(const uint8_t *payload, size_t len, int marker, void *userdata)
{
    video_send_ctx_t *ctx = (video_send_ctx_t *)userdata;
    nanortc_t *rtc = ctx->rtc;

    /* RFC 6184 §5.1: marker bit set on last packet of access unit.
     * Only set marker on last fragment of the last NAL in the frame. */
    rtc->video_rtp.marker = (uint8_t)((marker && ctx->is_last_nal) ? 1 : 0);

    /* Select a packet buffer from the ring so multiple fragments don't clobber
     * each other before the run loop dispatches them (Sans I/O). */
    uint8_t slot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
    uint8_t *pkt_buf = rtc->pkt_ring[slot];

    /* RTP pack */
    size_t rtp_len = 0;
    int rc = rtp_pack(&rtc->video_rtp, ctx->timestamp, payload, len, pkt_buf,
                      NANORTC_MEDIA_BUF_SIZE, &rtp_len);
    if (rc != NANORTC_OK) {
        ctx->last_rc = rc;
        return rc;
    }

    /* SRTP protect (in-place, appends 10B auth tag) */
    size_t srtp_len = 0;
    rc = srtp_protect(&rtc->srtp, pkt_buf, rtp_len, &srtp_len);
    if (rc != NANORTC_OK) {
        ctx->last_rc = rc;
        return rc;
    }

    /* Update RTCP stats */
    rtc->rtcp.packets_sent++;
    rtc->rtcp.octets_sent += (uint32_t)len;

    /* Enqueue for transmission — each slot has its own buffer */
    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = pkt_buf;
    out.transmit.len = srtp_len;
    out.transmit.dest = rtc->remote_addr;
    ctx->last_rc = rtc_enqueue_output(rtc, &out);
    return ctx->last_rc;
}

int nanortc_send_video(nanortc_t *rtc, uint32_t timestamp, const void *data, size_t len, int flags)
{
    if (!rtc || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED) {
        return NANORTC_ERR_STATE;
    }
    if (!rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    video_send_ctx_t ctx;
    ctx.rtc = rtc;
    ctx.timestamp = timestamp;
    ctx.last_rc = NANORTC_OK;
    ctx.is_last_nal = (flags & NANORTC_VIDEO_FLAG_MARKER) ? 1 : 0;

    int rc =
        h264_packetize((const uint8_t *)data, len, NANORTC_VIDEO_MTU, video_send_fragment_cb, &ctx);
    if (rc != NANORTC_OK) {
        return rc;
    }
    return ctx.last_rc;
}

int nanortc_request_keyframe(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED) {
        return NANORTC_ERR_STATE;
    }
    if (!rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    /* Generate PLI (RFC 4585 §6.3.1) */
    uint8_t pli_buf[RTCP_PLI_SIZE + NANORTC_SRTP_AUTH_TAG_SIZE + 4];
    size_t pli_len = 0;
    int rc = rtcp_generate_pli(rtc->rtcp.ssrc, rtc->rtcp.remote_ssrc, pli_buf, sizeof(pli_buf),
                               &pli_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* TODO: SRTCP protect (requires srtcp_protect, deferred to full RTCP integration) */

    /* Enqueue for transmission */
    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = pli_buf;
    out.transmit.len = pli_len;
    out.transmit.dest = rtc->remote_addr;
    return rtc_enqueue_output(rtc, &out);
}
#endif

const char *nanortc_err_to_name(int err)
{
    switch (err) {
    case NANORTC_OK:
        return "NANORTC_OK";
    case NANORTC_ERR_INVALID_PARAM:
        return "NANORTC_ERR_INVALID_PARAM";
    case NANORTC_ERR_BUFFER_TOO_SMALL:
        return "NANORTC_ERR_BUFFER_TOO_SMALL";
    case NANORTC_ERR_STATE:
        return "NANORTC_ERR_STATE";
    case NANORTC_ERR_CRYPTO:
        return "NANORTC_ERR_CRYPTO";
    case NANORTC_ERR_PROTOCOL:
        return "NANORTC_ERR_PROTOCOL";
    case NANORTC_ERR_NOT_IMPLEMENTED:
        return "NANORTC_ERR_NOT_IMPLEMENTED";
    case NANORTC_ERR_PARSE:
        return "NANORTC_ERR_PARSE";
    case NANORTC_ERR_NO_DATA:
        return "NANORTC_ERR_NO_DATA";
    case NANORTC_ERR_INTERNAL:
        return "NANORTC_ERR_INTERNAL";
    case NANORTC_ERR_WOULD_BLOCK:
        return "NANORTC_ERR_WOULD_BLOCK";
    default:
        return "NANORTC_ERR_UNKNOWN";
    }
}
