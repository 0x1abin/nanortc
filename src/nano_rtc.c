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
#include "nano_addr.h"
#include "nano_log.h"
#include "nanortc_util.h"

#if NANORTC_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#include "nano_datachannel.h"
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_media.h"
#include "nano_rtp.h"
#include "nano_rtcp.h"
#include "nano_srtp.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_h264.h"
#include "nano_bwe.h"
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

/* A4: Emit a simple event (no extra data) */
static int rtc_emit_event(nanortc_t *rtc, nanortc_event_type_t type)
{
    nanortc_output_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = NANORTC_OUTPUT_EVENT;
    evt.event.type = type;
    return rtc_enqueue_output(rtc, &evt);
}

/* Emit a typed event with full event struct */
static int rtc_emit_event_full(nanortc_t *rtc, const nanortc_event_t *event)
{
    nanortc_output_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = NANORTC_OUTPUT_EVENT;
    evt.event = *event;
    return rtc_enqueue_output(rtc, &evt);
}

/* Emit NANORTC_EV_CONNECTED with pre-filled writer handles */
static int rtc_emit_connected(nanortc_t *rtc)
{
    nanortc_event_t event;
    memset(&event, 0, sizeof(event));
    event.type = NANORTC_EV_CONNECTED;

#if NANORTC_HAVE_MEDIA_TRANSPORT
    uint8_t wc = 0;
    for (uint8_t i = 0; i < rtc->media_count && wc < NANORTC_MAX_MEDIA_TRACKS; i++) {
        nanortc_track_t *m = &rtc->media[i];
        if (!m->active) {
            continue;
        }
        if (m->direction == NANORTC_DIR_RECVONLY || m->direction == NANORTC_DIR_INACTIVE) {
            continue;
        }
        event.connected.writers[wc].rtc = rtc;
        event.connected.writers[wc].mid = m->mid;
        event.connected.writers[wc].kind = (uint8_t)m->kind;
        event.connected.writers[wc].rtp_ts = 0;
        event.connected.writers[wc].clock_rate =
            (m->kind == NANORTC_TRACK_VIDEO) ? 90000 : m->sample_rate;
        event.connected.writers[wc].frame_dur_ms = 0;
        wc++;
    }
    event.connected.writer_count = wc;
#endif

    return rtc_emit_event_full(rtc, &event);
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
    /* Media tracks start empty — user adds them via nanortc_add_track() */
    rtc->media_count = 0;
    memset(rtc->ssrc_map, 0, sizeof(rtc->ssrc_map));
    nano_srtp_init(&rtc->srtp, cfg->crypto, 0);
#endif

#if NANORTC_FEATURE_VIDEO
    bwe_init(&rtc->bwe);
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
 * Called from both accept_offer (answerer) and accept_answer (offerer).
 *
 * For each SDP m-line, find or create the matching media[] entry and
 * apply negotiated direction and PT. */
static void rtc_apply_negotiated_media(nanortc_t *rtc)
{
#if NANORTC_HAVE_MEDIA_TRANSPORT
    for (uint8_t i = 0; i < rtc->sdp.mline_count; i++) {
        nano_sdp_mline_t *ml = &rtc->sdp.mlines[i];
        if (!ml->active)
            continue;

        nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, ml->mid);
        if (!m) {
            /* Remote-initiated track (answerer path): auto-create media entry */
            if (rtc->media_count >= NANORTC_MAX_MEDIA_TRACKS)
                continue;
            nanortc_track_kind_t kind =
                (ml->kind == SDP_MLINE_AUDIO) ? NANORTC_TRACK_AUDIO : NANORTC_TRACK_VIDEO;
            uint8_t mid = ml->mid;
            uint8_t tidx = rtc->media_count;
            nanortc_direction_t local_dir = direction_complement(ml->remote_direction);
            uint32_t jitter_ms = 0;
#if NANORTC_FEATURE_AUDIO
            jitter_ms = rtc->config.jitter_depth_ms;
#endif
            track_init(&rtc->media[tidx], mid, kind, local_dir, ml->codec, ml->sample_rate,
                       ml->channels, jitter_ms);
            rtc->media_count = tidx + 1;
            m = &rtc->media[tidx];

            /* Emit MEDIA_ADDED event for remote-initiated tracks */
            nanortc_event_t maevt;
            memset(&maevt, 0, sizeof(maevt));
            maevt.type = NANORTC_EV_MEDIA_ADDED;
            maevt.media_added.mid = mid;
            maevt.media_added.kind = (uint8_t)kind;
            maevt.media_added.direction = local_dir;
            rtc_emit_event_full(rtc, &maevt);
        }

        /* Apply negotiated direction (answerer computes complement) */
        if (ml->remote_direction != NANORTC_DIR_SENDRECV || m->direction == NANORTC_DIR_SENDRECV) {
            m->direction = direction_complement(ml->remote_direction);
        }
        ml->direction = m->direction;

        /* Apply negotiated PT.
         * For video: fmtp parsing already selected the correct H264 PT from
         * the offer's codec list (matching packetization-mode=1).  Don't
         * overwrite it with remote_pt, which is just the *first* PT on the
         * m= line (often VP8, not H264). */
        if (ml->kind == SDP_MLINE_VIDEO && ml->pt != 0) {
            m->rtp.payload_type = ml->pt;
            NANORTC_LOGD("SDP", "video using negotiated H264 PT");
        } else if (ml->remote_pt != 0) {
            m->rtp.payload_type = ml->remote_pt;
            ml->pt = ml->remote_pt;
            NANORTC_LOGD("SDP", "media using remote PT");
        }
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

    /* DataChannel m-line is registered via nanortc_create_datachannel() — no auto-add */

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

    /* Parse IP address (IPv4 or IPv6) */
    uint8_t parsed_addr[NANORTC_ADDR_SIZE] = {0};
    uint8_t family = 0;
    int rc = addr_parse_auto(addr_str, addr_len, parsed_addr, &family);
    if (rc != 0) {
        return rc;
    }

    /* Store in ICE remote candidates array */
    if (rtc->ice.remote_candidate_count >= NANORTC_MAX_ICE_CANDIDATES) {
        NANORTC_LOGW("RTC", "remote candidate table full");
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    uint8_t idx = rtc->ice.remote_candidate_count;
    rtc->ice.remote_candidates[idx].family = family;
    memset(rtc->ice.remote_candidates[idx].addr, 0, NANORTC_ADDR_SIZE);
    memcpy(rtc->ice.remote_candidates[idx].addr, parsed_addr,
           family == 4 ? 4u : (size_t)NANORTC_ADDR_SIZE);
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
 * rtc_process_receive — RFC 7983 demux
 * ---------------------------------------------------------------- */

static int rtc_process_receive(nanortc_t *rtc, const uint8_t *data, size_t len,
                               const nanortc_addr_t *src)
{
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
            /* Emit ICE_STATE_CHANGE (CONNECTED emitted later when fully ready) */
            {
                nanortc_event_t ice_evt;
                memset(&ice_evt, 0, sizeof(ice_evt));
                ice_evt.type = NANORTC_EV_ICE_STATE_CHANGE;
                ice_evt.ice_state = (uint16_t)NANORTC_ICE_STATE_CONNECTED;
                rtc_emit_event_full(rtc, &ice_evt);
            }

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

            rtc_cache_fingerprint(rtc);

#if NANORTC_HAVE_MEDIA_TRANSPORT
            /* Derive SRTP keys from DTLS keying material (RFC 5764 §4.2) */
            if (rtc->dtls.keying_material_ready) {
                int is_client = !rtc->dtls.is_server;
                nano_srtp_init(&rtc->srtp, rtc->config.crypto, is_client);
                nano_srtp_derive_keys(&rtc->srtp, rtc->dtls.keying_material,
                                      NANORTC_DTLS_KEYING_SIZE);

                /* Generate random SSRC + init_seq for each active track,
                 * register in ssrc_map for receive-path demuxing. */
                for (uint8_t ti = 0; ti < rtc->media_count; ti++) {
                    nanortc_track_t *m = &rtc->media[ti];
                    if (!m->active)
                        continue;
                    uint32_t ssrc = 0;
                    uint16_t init_seq = 0;
                    if (rtc->config.crypto) {
                        uint8_t rnd[6];
                        rtc->config.crypto->random_bytes(rnd, 6);
                        ssrc = nanortc_read_u32be(rnd);
                        init_seq = nanortc_read_u16be(rnd + 4);
                    }
                    /* Find negotiated PT from SDP mline */
                    uint8_t pt = m->rtp.payload_type;
                    nano_sdp_mline_t *ml = sdp_find_mline(&rtc->sdp, m->mid);
                    if (ml && ml->pt != 0)
                        pt = ml->pt;
                    rtp_init(&m->rtp, ssrc, pt);
                    m->rtp.seq = init_seq;
                    m->rtcp.ssrc = ssrc;
                    ssrc_map_register(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, ssrc, m->mid);
                    NANORTC_LOGD("RTP", "track RTP initialized");
                }
                NANORTC_LOGI("RTC", "SRTP keys derived, RTP ready");
            }
#endif

#if NANORTC_FEATURE_DATACHANNEL
            /* Initiate SCTP only if m=application was negotiated */
            if (rtc->sdp.has_datachannel) {
                /* DTLS client sends INIT (RFC 8831) */
                if (!rtc->dtls.is_server) {
                    nsctp_start(&rtc->sctp);
                    rtc->state = NANORTC_STATE_SCTP_CONNECTING;

                    /* Drain SCTP output (INIT) through DTLS encrypt */
                    rtc_pump_sctp_through_dtls(rtc, src);
                }
            } else {
                /* Media-only session — DTLS connected is final state */
                rtc->state = NANORTC_STATE_CONNECTED;
                rtc_emit_connected(rtc);
                NANORTC_LOGI("RTC", "connected (media only, no SCTP)");
            }
#else
            /* No DataChannel — DTLS connected is final state */
            rtc->state = NANORTC_STATE_CONNECTED;
            rtc_emit_connected(rtc);
            NANORTC_LOGI("RTC", "connected (no DC)");
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
                    rtc_emit_connected(rtc);
                    NANORTC_LOGI("RTC", "connected (SCTP established)");
                }

                /* Deliver SCTP payload via DataChannel */
                if (rtc->sctp.has_delivered) {
                    dc_handle_message(&rtc->datachannel, rtc->sctp.delivered_stream,
                                      rtc->sctp.delivered_ppid, rtc->sctp.delivered_data,
                                      rtc->sctp.delivered_len);
                    rtc->sctp.has_delivered = false;

                    /* Emit DC events using typed event structs */
                    if (rtc->sctp.delivered_ppid == DCEP_PPID_CONTROL) {
                        if (!rtc->datachannel.last_was_open) {
                            goto skip_dc_event;
                        }
                        /* CHANNEL_OPEN event */
                        nanortc_event_t oevt;
                        memset(&oevt, 0, sizeof(oevt));
                        oevt.type = NANORTC_EV_DATACHANNEL_OPEN;
                        oevt.datachannel_open.id = rtc->sctp.delivered_stream;
                        for (uint8_t ci = 0; ci < rtc->datachannel.channel_count; ci++) {
                            if (rtc->datachannel.channels[ci].stream_id ==
                                rtc->sctp.delivered_stream) {
                                oevt.datachannel_open.label = rtc->datachannel.channels[ci].label;
                                break;
                            }
                        }
                        rtc_emit_event_full(rtc, &oevt);
                    } else {
                        /* CHANNEL_DATA event (binary or string) */
                        bool is_binary = (rtc->sctp.delivered_ppid == DCEP_PPID_BINARY ||
                                          rtc->sctp.delivered_ppid == DCEP_PPID_BINARY_EMPTY);
                        nanortc_event_t devt;
                        memset(&devt, 0, sizeof(devt));
                        devt.type = NANORTC_EV_DATACHANNEL_DATA;
                        devt.datachannel_data.id = rtc->sctp.delivered_stream;
                        devt.datachannel_data.data = rtc->sctp.delivered_data;
                        devt.datachannel_data.len = rtc->sctp.delivered_len;
                        devt.datachannel_data.binary = is_binary;
                        rtc_emit_event_full(rtc, &devt);
                    }
                skip_dc_event:;
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
        /* RTP/RTCP [0x80-0xBF] — RFC 7983 §3 */
#if NANORTC_HAVE_MEDIA_TRANSPORT
        if (!rtc->srtp.ready) {
            return NANORTC_OK; /* SRTP not ready yet, discard */
        }

        /* Distinguish RTP vs RTCP by payload type field (byte 1).
         * RTCP PT range: 200-211 (standard).
         * RFC 5761 §4: RTP PT < 72 or > 76, RTCP PT ∈ {200..211}. */
        if (len < 2) {
            return NANORTC_ERR_PARSE;
        }
        uint8_t second = data[1];

        if (second >= 200 && second <= 211) {
            /* RTCP packet — parse and handle PLI/NACK/SR */
            /* Copy to scratch for unprotect (in-place) */
            if (len > sizeof(rtc->stun_buf)) {
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            /* TODO: SRTCP unprotect when full SRTCP support is added */
            nano_rtcp_info_t info;
            memset(&info, 0, sizeof(info));
            int rrc = rtcp_parse(data, len, &info);
            if (rrc == NANORTC_OK && info.type == RTCP_PSFB) {
                /* PLI — find video track by SSRC and emit keyframe request event */
                int mid = ssrc_map_lookup(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, info.ssrc);
                if (mid >= 0) {
                    nanortc_event_t kfevt;
                    memset(&kfevt, 0, sizeof(kfevt));
                    kfevt.type = NANORTC_EV_KEYFRAME_REQUEST;
                    kfevt.keyframe_request.mid = (uint8_t)mid;
                    rtc_emit_event_full(rtc, &kfevt);
                }
            }
            return NANORTC_OK;
        }

        /* RTP packet — demux by SSRC → MID */
        /* Make a mutable copy for SRTP unprotect (in-place) */
        if (len > NANORTC_MEDIA_BUF_SIZE) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }

        /* Use stun_buf as scratch for RTP unprotect (not used simultaneously) */
        uint8_t *pkt = rtc->stun_buf;
        if (len > sizeof(rtc->stun_buf)) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(pkt, data, len);
        size_t pkt_len = len;

        /* SRTP unprotect */
        size_t plain_len = 0;
        int src_rc = nano_srtp_unprotect(&rtc->srtp, pkt, pkt_len, &plain_len);
        if (src_rc != NANORTC_OK) {
            return NANORTC_OK; /* Silently discard bad SRTP packets */
        }

        /* Parse RTP header */
        uint8_t rtp_pt = 0;
        uint16_t rtp_seq = 0;
        uint32_t rtp_ts = 0;
        uint32_t rtp_ssrc = 0;
        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        int rrc = rtp_unpack(pkt, plain_len, &rtp_pt, &rtp_seq, &rtp_ts, &rtp_ssrc, &payload,
                             &payload_len);
        if (rrc != NANORTC_OK) {
            return NANORTC_OK; /* Malformed RTP, discard */
        }

        /* SSRC → MID lookup */
        int mid = ssrc_map_lookup(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, rtp_ssrc);
        if (mid < 0) {
            /* First-time SSRC discovery: try PT-based matching */
            for (uint8_t ti = 0; ti < rtc->media_count; ti++) {
                nanortc_track_t *mc = &rtc->media[ti];
                if (!mc->active)
                    continue;
                nano_sdp_mline_t *ml = sdp_find_mline(&rtc->sdp, mc->mid);
                if (ml && ml->remote_pt == rtp_pt) {
                    ssrc_map_register(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, rtp_ssrc, mc->mid);
                    mc->rtcp.remote_ssrc = rtp_ssrc;
                    mid = (int)mc->mid;
                    break;
                }
            }
        }
        if (mid < 0) {
            return NANORTC_OK; /* Unknown SSRC/PT, discard */
        }

        nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, (uint8_t)mid);
        if (!m) {
            return NANORTC_OK;
        }

        /* Update RTCP receiver stats */
        m->rtcp.packets_received++;
        if (rtp_seq > m->rtcp.max_seq || m->rtcp.packets_received == 1) {
            m->rtcp.max_seq = rtp_seq;
        }
        if (m->rtcp.remote_ssrc == 0) {
            m->rtcp.remote_ssrc = rtp_ssrc;
        }

        /* Route to audio or video processing */
        if (m->kind == NANORTC_TRACK_AUDIO) {
#if NANORTC_FEATURE_AUDIO
            /* Push into jitter buffer, then try to pop completed frame */
            jitter_push(&m->track.audio.jitter, rtp_seq, rtp_ts, payload, payload_len, rtc->now_ms);
            size_t pop_len = 0;
            uint32_t pop_ts = 0;
            while (jitter_pop(&m->track.audio.jitter, rtc->now_ms, m->media_buf,
                              sizeof(m->media_buf), &pop_len, &pop_ts) == NANORTC_OK) {
                nanortc_event_t aevt;
                memset(&aevt, 0, sizeof(aevt));
                aevt.type = NANORTC_EV_MEDIA_DATA;
                aevt.media_data.mid = m->mid;
                aevt.media_data.pt = m->rtp.payload_type;
                aevt.media_data.data = m->media_buf;
                aevt.media_data.len = pop_len;
                aevt.media_data.timestamp = pop_ts;
                aevt.media_data.contiguous = true; /* jitter buffer ensures order */
                rtc_emit_event_full(rtc, &aevt);
            }
#endif
        } else {
#if NANORTC_FEATURE_VIDEO
            /* H.264 depacketization */
            uint8_t rtp_marker = (pkt[1] >> 7) & 1;
            const uint8_t *nalu_out = NULL;
            size_t nalu_len = 0;
            int drc = h264_depkt_push(&m->track.video.h264_depkt, payload, payload_len, rtp_marker,
                                      &nalu_out, &nalu_len);
            if (drc == NANORTC_OK && nalu_out && nalu_len > 0) {
                nanortc_event_t vevt;
                memset(&vevt, 0, sizeof(vevt));
                vevt.type = NANORTC_EV_MEDIA_DATA;
                vevt.media_data.mid = m->mid;
                vevt.media_data.pt = m->rtp.payload_type;
                vevt.media_data.data = nalu_out;
                vevt.media_data.len = nalu_len;
                vevt.media_data.timestamp = rtp_ts;
                vevt.media_data.is_keyframe = h264_is_keyframe(nalu_out, nalu_len) ? true : false;
                vevt.media_data.contiguous = true;
                rtc_emit_event_full(rtc, &vevt);
            }
#endif
        }
        return NANORTC_OK;
#else
        /* No media transport compiled — silently discard */
        return NANORTC_OK;
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */
    }

    return NANORTC_ERR_PROTOCOL; /* Unknown packet type */
}

/* ----------------------------------------------------------------
 * Timer processing (extracted from former nanortc_handle_timeout)
 * ---------------------------------------------------------------- */

static int rtc_process_timers(nanortc_t *rtc, uint32_t now_ms)
{
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

        /* Emit ICE_STATE_CHANGE on transition to CHECKING */
        if (rtc->ice.state != prev_ice && rtc->ice.state == NANORTC_ICE_STATE_CHECKING) {
            nanortc_event_t isce;
            memset(&isce, 0, sizeof(isce));
            isce.type = NANORTC_EV_ICE_STATE_CHANGE;
            isce.ice_state = (uint16_t)NANORTC_ICE_STATE_CHECKING;
            rtc_emit_event_full(rtc, &isce);
        }

        /* Schedule next timeout (only when a check was actually sent) */
        if (out_len > 0 && rtc->ice.state == NANORTC_ICE_STATE_CHECKING) {
            nanortc_output_t tout;
            memset(&tout, 0, sizeof(tout));
            tout.type = NANORTC_OUTPUT_TIMEOUT;
            tout.timeout_ms = rtc->ice.check_interval_ms;
            rtc_enqueue_output(rtc, &tout);
        }

        /* Propagate ICE failure */
        if (rtc->ice.state == NANORTC_ICE_STATE_FAILED) {
            nanortc_event_t fice;
            memset(&fice, 0, sizeof(fice));
            fice.type = NANORTC_EV_ICE_STATE_CHANGE;
            fice.ice_state = (uint16_t)NANORTC_ICE_STATE_FAILED;
            rtc_emit_event_full(rtc, &fice);
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
 * nanortc_handle_input — unified input entry point
 * ---------------------------------------------------------------- */

int nanortc_handle_input(nanortc_t *rtc, uint32_t now_ms, const uint8_t *data, size_t len,
                         const nanortc_addr_t *src)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    rtc->now_ms = now_ms;

    /* Always process timers (ICE checks, SCTP retransmits) */
    int trc = rtc_process_timers(rtc, now_ms);
    if (trc != NANORTC_OK) {
        return trc;
    }

    /* If packet data provided, process the incoming UDP packet */
    if (data && len > 0 && src) {
        return rtc_process_receive(rtc, data, len, src);
    }

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

int nanortc_create_datachannel(nanortc_t *rtc, const char *label,
                               const nanortc_datachannel_options_t *options)
{
    if (!rtc || !label) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Ensure DC m-line is registered in SDP */
    if (!rtc->sdp.has_datachannel) {
        rtc->sdp.has_datachannel = true;
        rtc->sdp.dc_mid = rtc->sdp.mid_count;
        rtc->sdp.mid_count++;
    }

    bool ordered = options ? !options->unordered : true;
    uint16_t max_rexmit = options ? options->max_retransmits : 0;

    uint16_t sid = rtc_alloc_stream_id(rtc);
    int rc = dc_open(&rtc->datachannel, sid, label, ordered, max_rexmit);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* If already connected, drain DCEP OPEN through SCTP→DTLS */
    if (rtc->state == NANORTC_STATE_CONNECTED) {
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
    }

    NANORTC_LOGI("RTC", "datachannel created");
    return (int)sid;
}

int nanortc_get_datachannel(nanortc_t *rtc, uint16_t id, nanortc_datachannel_t *ch)
{
    if (!rtc || !ch) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    /* Validate channel exists and is open */
    for (uint8_t i = 0; i < rtc->datachannel.channel_count; i++) {
        if (rtc->datachannel.channels[i].stream_id == id &&
            rtc->datachannel.channels[i].state != NANORTC_DC_STATE_CLOSED) {
            ch->rtc = rtc;
            ch->id = id;
            return NANORTC_OK;
        }
    }
    return NANORTC_ERR_INVALID_PARAM;
}

int nanortc_datachannel_send(nanortc_datachannel_t *ch, const void *data, size_t len)
{
    if (!ch || !ch->rtc || !data) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (ch->rtc->state != NANORTC_STATE_CONNECTED) {
        NANORTC_LOGW("DC", "send failed: not connected");
        return NANORTC_ERR_STATE;
    }

    uint32_t ppid = (len > 0) ? DCEP_PPID_BINARY : DCEP_PPID_BINARY_EMPTY;
    int rc = nsctp_send(&ch->rtc->sctp, ch->id, ppid, (const uint8_t *)data, len);
    if (rc == NANORTC_ERR_BUFFER_TOO_SMALL) {
        NANORTC_LOGD("DC", "send would block (SCTP buffer full)");
        return NANORTC_ERR_WOULD_BLOCK;
    }
    return rc;
}

int nanortc_datachannel_send_string(nanortc_datachannel_t *ch, const char *str)
{
    if (!ch || !ch->rtc || !str) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (ch->rtc->state != NANORTC_STATE_CONNECTED) {
        return NANORTC_ERR_STATE;
    }

    size_t len = strlen(str); /* NANORTC_SAFE: API boundary */

    uint32_t ppid = (len > 0) ? DCEP_PPID_STRING : DCEP_PPID_STRING_EMPTY;
    int rc = nsctp_send(&ch->rtc->sctp, ch->id, ppid, (const uint8_t *)str, len);
    if (rc == NANORTC_ERR_BUFFER_TOO_SMALL) {
        return NANORTC_ERR_WOULD_BLOCK;
    }
    return rc;
}

int nanortc_datachannel_close(nanortc_datachannel_t *ch)
{
    if (!ch || !ch->rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nano_dc_channel_t *dc = NULL;
    for (uint8_t i = 0; i < ch->rtc->datachannel.channel_count; i++) {
        if (ch->rtc->datachannel.channels[i].stream_id == ch->id &&
            ch->rtc->datachannel.channels[i].state != NANORTC_DC_STATE_CLOSED) {
            dc = &ch->rtc->datachannel.channels[i];
            break;
        }
    }
    if (!dc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    dc->state = NANORTC_DC_STATE_CLOSED;
    nanortc_event_t cevt;
    memset(&cevt, 0, sizeof(cevt));
    cevt.type = NANORTC_EV_DATACHANNEL_CLOSE;
    cevt.datachannel_id.id = ch->id;
    rtc_emit_event_full(ch->rtc, &cevt);

    NANORTC_LOGI("RTC", "channel closed");
    return NANORTC_OK;
}

const char *nanortc_datachannel_get_label(nanortc_datachannel_t *ch)
{
    if (!ch || !ch->rtc) {
        return NULL;
    }
    for (uint8_t i = 0; i < ch->rtc->datachannel.channel_count; i++) {
        if (ch->rtc->datachannel.channels[i].stream_id == ch->id &&
            ch->rtc->datachannel.channels[i].state != NANORTC_DC_STATE_CLOSED) {
            return ch->rtc->datachannel.channels[i].label;
        }
    }
    return NULL;
}
#endif /* NANORTC_FEATURE_DATACHANNEL */

/* ----------------------------------------------------------------
 * Connection state API
 * ---------------------------------------------------------------- */

bool nanortc_is_alive(const nanortc_t *rtc)
{
    if (!rtc) {
        return false;
    }
    return rtc->state != NANORTC_STATE_CLOSED;
}

bool nanortc_is_connected(const nanortc_t *rtc)
{
    if (!rtc) {
        return false;
    }
    return rtc->state >= NANORTC_STATE_CONNECTED && rtc->state != NANORTC_STATE_CLOSED;
}

void nanortc_disconnect(nanortc_t *rtc)
{
    if (!rtc) {
        return;
    }
    if (rtc->state == NANORTC_STATE_CLOSED || rtc->state == NANORTC_STATE_NEW) {
        return;
    }

#if NANORTC_FEATURE_DATACHANNEL
    /* Send SCTP SHUTDOWN if association is established */
    if (rtc->sctp.state == NANORTC_SCTP_STATE_ESTABLISHED) {
        uint8_t shutdown_pkt[64];
        size_t hdr_len = nsctp_encode_header(shutdown_pkt, rtc->sctp.local_port,
                                             rtc->sctp.remote_port, rtc->sctp.remote_vtag);
        size_t chunk_len = nsctp_encode_shutdown(shutdown_pkt + hdr_len, rtc->sctp.cumulative_tsn);
        nsctp_finalize_checksum(shutdown_pkt, hdr_len + chunk_len);

        dtls_encrypt(&rtc->dtls, shutdown_pkt, hdr_len + chunk_len);
        rtc_drain_dtls_output(rtc, &rtc->remote_addr);
        rtc->sctp.state = NANORTC_SCTP_STATE_SHUTDOWN_SENT;
    }
#endif

    /* Send DTLS close_notify */
    dtls_close(&rtc->dtls);
    rtc_drain_dtls_output(rtc, &rtc->remote_addr);

    rtc->state = NANORTC_STATE_CLOSED;
    rtc_emit_event(rtc, NANORTC_EV_DISCONNECTED);

    NANORTC_LOGI("RTC", "disconnected");
}

#if NANORTC_HAVE_MEDIA_TRANSPORT

int nanortc_add_track(nanortc_t *rtc, nanortc_track_kind_t kind, nanortc_direction_t direction,
                      nanortc_codec_t codec, uint32_t sample_rate, uint8_t channels)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->media_count >= NANORTC_MAX_MEDIA_TRACKS) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Determine PT for SDP */
    uint8_t pt = 0;
    uint8_t sdp_kind = SDP_MLINE_AUDIO;
    if (kind == NANORTC_TRACK_AUDIO) {
        if (codec == NANORTC_CODEC_PCMU)
            pt = 0;
        else if (codec == NANORTC_CODEC_PCMA)
            pt = 8;
        else
            pt = 111; /* Opus dynamic PT */
        sdp_kind = SDP_MLINE_AUDIO;
    } else {
        pt = NANORTC_VIDEO_DEFAULT_PT;
        sdp_kind = SDP_MLINE_VIDEO;
    }

    /* Add SDP m-line (returns MID) */
    int mid =
        sdp_add_mline(&rtc->sdp, sdp_kind, (uint8_t)codec, pt, sample_rate, channels, direction);
    if (mid < 0) {
        return mid;
    }

    /* Initialize media track at the next available slot (not by MID index —
     * DC can occupy SDP MIDs without consuming media track slots). */
    uint8_t tidx = rtc->media_count;

    uint32_t jitter_ms = 0;
#if NANORTC_FEATURE_AUDIO
    jitter_ms = rtc->config.jitter_depth_ms;
#endif
    int rc = track_init(&rtc->media[tidx], (uint8_t)mid, kind, direction, (uint8_t)codec,
                        sample_rate, channels, jitter_ms);
    if (rc != NANORTC_OK) {
        return rc;
    }
    rtc->media_count = tidx + 1;

    NANORTC_LOGI("RTC", "media track added");
    return mid;
}

int nanortc_add_audio_track(nanortc_t *rtc, nanortc_direction_t direction, nanortc_codec_t codec,
                            uint32_t sample_rate, uint8_t channels)
{
    return nanortc_add_track(rtc, NANORTC_TRACK_AUDIO, direction, codec, sample_rate, channels);
}

int nanortc_add_video_track(nanortc_t *rtc, nanortc_direction_t direction, nanortc_codec_t codec)
{
    return nanortc_add_track(rtc, NANORTC_TRACK_VIDEO, direction, codec, 90000, 0);
}

/* Send audio: RTP pack → SRTP protect → enqueue */
static int rtc_send_audio(nanortc_t *rtc, nanortc_track_t *m, uint32_t timestamp,
                          const uint8_t *data, size_t len)
{
    size_t rtp_len = 0;
    int rc = rtp_pack(&m->rtp, timestamp, data, len, m->media_buf, sizeof(m->media_buf), &rtp_len);
    if (rc != NANORTC_OK)
        return rc;

    size_t srtp_len = 0;
    rc = nano_srtp_protect(&rtc->srtp, m->media_buf, rtp_len, &srtp_len);
    if (rc != NANORTC_OK)
        return rc;

    m->rtcp.packets_sent++;
    m->rtcp.octets_sent += (uint32_t)len;

    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = m->media_buf;
    out.transmit.len = srtp_len;
    out.transmit.dest = rtc->remote_addr;
    return rtc_enqueue_output(rtc, &out);
}

#if NANORTC_FEATURE_VIDEO
/* Context for h264_packetize callback → RTP pack + SRTP protect + enqueue */
typedef struct {
    nanortc_t *rtc;
    nanortc_track_t *media;
    uint32_t timestamp;
    int last_rc;
    int is_last_nal;
} video_send_ctx_t;

static int video_send_fragment_cb(const uint8_t *payload, size_t len, int marker, void *userdata)
{
    video_send_ctx_t *ctx = (video_send_ctx_t *)userdata;
    nanortc_t *rtc = ctx->rtc;
    nanortc_track_t *m = ctx->media;

    /* RFC 6184 §5.1: marker bit on last packet of access unit */
    m->rtp.marker = (uint8_t)((marker && ctx->is_last_nal) ? 1 : 0);

    /* Select a packet buffer from the ring so multiple fragments don't clobber
     * each other before dispatch (Sans I/O). */
    uint8_t slot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
    uint8_t *pkt_buf = rtc->pkt_ring[slot];

    size_t rtp_len = 0;
    int rc =
        rtp_pack(&m->rtp, ctx->timestamp, payload, len, pkt_buf, NANORTC_MEDIA_BUF_SIZE, &rtp_len);
    if (rc != NANORTC_OK) {
        NANORTC_LOGW("RTP", "video rtp_pack failed");
        ctx->last_rc = rc;
        return rc;
    }

    size_t srtp_len = 0;
    rc = nano_srtp_protect(&rtc->srtp, pkt_buf, rtp_len, &srtp_len);
    if (rc != NANORTC_OK) {
        NANORTC_LOGW("SRTP", "video srtp_protect failed");
        ctx->last_rc = rc;
        return rc;
    }

    m->rtcp.packets_sent++;
    m->rtcp.octets_sent += (uint32_t)len;

    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = pkt_buf;
    out.transmit.len = srtp_len;
    out.transmit.dest = rtc->remote_addr;
    ctx->last_rc = rtc_enqueue_output(rtc, &out);
    return ctx->last_rc;
}

static int rtc_send_video(nanortc_t *rtc, nanortc_track_t *m, uint32_t timestamp,
                          const uint8_t *data, size_t len, int flags)
{
    video_send_ctx_t ctx;
    ctx.rtc = rtc;
    ctx.media = m;
    ctx.timestamp = timestamp;
    ctx.last_rc = NANORTC_OK;
    ctx.is_last_nal = (flags & NANORTC_VIDEO_FLAG_MARKER) ? 1 : 0;

    int rc = h264_packetize(data, len, NANORTC_VIDEO_MTU, video_send_fragment_cb, &ctx);
    if (rc != NANORTC_OK)
        return rc;
    return ctx.last_rc;
}
#endif /* NANORTC_FEATURE_VIDEO */

void nanortc_set_direction(nanortc_t *rtc, uint8_t mid, nanortc_direction_t dir)
{
    if (!rtc) {
        return;
    }
    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m) {
        return;
    }
    nanortc_direction_t old_dir = m->direction;
    m->direction = dir;

    /* Emit MEDIA_CHANGED event if direction actually changed */
    if (old_dir != dir) {
        nanortc_event_t mce;
        memset(&mce, 0, sizeof(mce));
        mce.type = NANORTC_EV_MEDIA_CHANGED;
        mce.media_changed.mid = mid;
        mce.media_changed.old_direction = old_dir;
        mce.media_changed.new_direction = dir;
        rtc_emit_event_full(rtc, &mce);
    }
}

const nanortc_track_t *nanortc_get_track(const nanortc_t *rtc, uint8_t mid)
{
    if (!rtc) {
        return NULL;
    }
    return track_find_by_mid((nanortc_track_t *)rtc->media, rtc->media_count, mid);
}

int nanortc_get_writer(nanortc_t *rtc, uint8_t mid, nanortc_writer_t *w)
{
    if (!rtc || !w) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (m->direction == NANORTC_DIR_RECVONLY || m->direction == NANORTC_DIR_INACTIVE) {
        return NANORTC_ERR_STATE;
    }

    w->rtc = rtc;
    w->mid = mid;
    w->kind = (uint8_t)m->kind;
    w->rtp_ts = 0;
    w->clock_rate = (m->kind == NANORTC_TRACK_VIDEO) ? 90000 : m->sample_rate;
    w->frame_dur_ms = 0;
    return NANORTC_OK;
}

int nanortc_writer_write(nanortc_writer_t *w, uint32_t timestamp, const void *data, size_t len,
                         int flags)
{
    if (!w || !w->rtc || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nanortc_t *rtc = w->rtc;
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, w->mid);
    if (!m) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (m->kind == NANORTC_TRACK_AUDIO) {
        return rtc_send_audio(rtc, m, timestamp, (const uint8_t *)data, len);
    }
#if NANORTC_FEATURE_VIDEO
    if (m->kind == NANORTC_TRACK_VIDEO) {
        return rtc_send_video(rtc, m, timestamp, (const uint8_t *)data, len, flags);
    }
#endif
    return NANORTC_ERR_INVALID_PARAM;
}

int nanortc_writer_request_keyframe(nanortc_writer_t *w)
{
    if (!w || !w->rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nanortc_t *rtc = w->rtc;
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, w->mid);
    if (!m || m->kind != NANORTC_TRACK_VIDEO) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Generate PLI (RFC 4585 §6.3.1) */
    uint8_t pli_buf[RTCP_PLI_SIZE + NANORTC_SRTP_AUTH_TAG_SIZE + 4];
    size_t pli_len = 0;
    int rc =
        rtcp_generate_pli(m->rtcp.ssrc, m->rtcp.remote_ssrc, pli_buf, sizeof(pli_buf), &pli_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* TODO: SRTCP protect (deferred to full RTCP integration) */

    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = pli_buf;
    out.transmit.len = pli_len;
    out.transmit.dest = rtc->remote_addr;
    return rtc_enqueue_output(rtc, &out);
}

/* ----------------------------------------------------------------
 * Flat convenience API (no writer handle)
 *
 * These are the canonical implementations. The writer convenience
 * functions (nanortc_writer_send_audio / nanortc_writer_send_video)
 * delegate here to avoid logic duplication.
 * ---------------------------------------------------------------- */

void nanortc_set_frame_duration(nanortc_t *rtc, uint8_t mid, uint32_t frame_ms)
{
    if (!rtc) {
        return;
    }
    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (m) {
        m->send_frame_dur_ms = frame_ms;
    }
}

/** Advance the per-track RTP timestamp after sending one frame. */
static void track_advance_rtp_ts(nanortc_track_t *m)
{
    uint32_t clock = (m->kind == NANORTC_TRACK_VIDEO) ? 90000 : m->sample_rate;
    if (clock == 0) {
        return;
    }
    if (m->send_frame_dur_ms > 0) {
        m->send_rtp_ts += clock * m->send_frame_dur_ms / 1000;
    } else if (m->kind == NANORTC_TRACK_AUDIO) {
        m->send_rtp_ts += clock / 50; /* default 20ms */
    }
    /* Video with frame_dur_ms==0: no auto-advance (user must call
     * nanortc_set_frame_duration first, since fps is unknown). */
}

int nanortc_send_audio(nanortc_t *rtc, uint8_t mid, const void *data, size_t len)
{
    if (!rtc || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m || m->kind != NANORTC_TRACK_AUDIO) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    int rc = rtc_send_audio(rtc, m, m->send_rtp_ts, (const uint8_t *)data, len);
    track_advance_rtp_ts(m);
    return rc;
}

#if NANORTC_FEATURE_VIDEO
int nanortc_send_video(nanortc_t *rtc, uint8_t mid, const void *data, size_t len)
{
    if (!rtc || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        NANORTC_LOGW("RTP", "video send blocked by state");
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m || m->kind != NANORTC_TRACK_VIDEO) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    uint32_t ts = m->send_rtp_ts;
    const uint8_t *buf = (const uint8_t *)data;
    size_t offset = 0;
    size_t nal_len = 0;
    int last_rc = NANORTC_OK;

    while (offset < len) {
        const uint8_t *nal = h264_annex_b_find_nal(buf, len, &offset, &nal_len);
        if (!nal || nal_len == 0) {
            break;
        }

        int flags = 0;
        if ((nal[0] & 0x1F) == 5) {
            flags |= NANORTC_VIDEO_FLAG_KEYFRAME;
        }

        size_t peek_off = offset;
        size_t peek_len = 0;
        if (!h264_annex_b_find_nal(buf, len, &peek_off, &peek_len)) {
            flags |= NANORTC_VIDEO_FLAG_MARKER;
        }

        last_rc = rtc_send_video(rtc, m, ts, nal, nal_len, flags);
        if (last_rc != NANORTC_OK) {
            return last_rc;
        }
    }

    track_advance_rtp_ts(m);
    return last_rc;
}
#endif /* NANORTC_FEATURE_VIDEO */

/* Writer convenience: thin wrappers around flat API */

int nanortc_writer_send_audio(nanortc_writer_t *w, const void *data, size_t len)
{
    if (!w || !w->rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    return nanortc_send_audio(w->rtc, w->mid, data, len);
}

#if NANORTC_FEATURE_VIDEO
int nanortc_writer_send_video(nanortc_writer_t *w, const void *data, size_t len)
{
    if (!w || !w->rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    return nanortc_send_video(w->rtc, w->mid, data, len);
}
#endif /* NANORTC_FEATURE_VIDEO */

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

const char *nanortc_err_name(int err)
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
