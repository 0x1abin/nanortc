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
#if NANORTC_FEATURE_H265
#include "nano_h265.h"
#include "nano_base64.h"
#include "nano_annex_b.h"
#endif
/* Internal video flags for RTP packetization */
#define NANORTC_VIDEO_FLAG_KEYFRAME 0x01 /* NAL is part of a keyframe (IDR) */
#define NANORTC_VIDEO_FLAG_MARKER   0x02 /* Last NAL in access unit (RTP marker bit) */
#endif

#include <string.h>

/* Shared hex alphabet for ICE credential generation */
static const char hex_chars[] = "0123456789abcdef";

/* Forward declarations */
static int rtc_apply_ice_servers(nanortc_t *rtc, const nanortc_ice_server_t *servers, size_t count);
static int rtc_emit_event_full(nanortc_t *rtc, const nanortc_event_t *event);

/* ----------------------------------------------------------------
 * Candidate string builder utilities
 *
 * Used by all three candidate emission paths (host, srflx, relay)
 * to avoid duplicating uint-to-decimal + memcpy logic.
 * ---------------------------------------------------------------- */

/** Append a decimal uint32 to buf at *pos. Returns new pos. */
static size_t cand_append_u32(char *buf, size_t pos, uint32_t val)
{
    char tmp[12];
    int len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        char rev[12];
        int ri = 0;
        while (val > 0) {
            rev[ri++] = (char)('0' + (val % 10));
            val /= 10;
        }
        while (ri > 0)
            tmp[len++] = rev[--ri];
    }
    memcpy(buf + pos, tmp, (size_t)len);
    return pos + (size_t)len;
}

/** Append a literal string to buf at *pos. Returns new pos. */
static size_t cand_append(char *buf, size_t pos, const char *str, size_t len)
{
    memcpy(buf + pos, str, len);
    return pos + len;
}

/**
 * Build an ICE candidate string into buf.
 *
 * Format: "candidate:<foundation> 1 UDP <priority> <ip> <port> typ <type>"
 * Returns the string length (buf is NUL-terminated).
 */
static size_t rtc_build_candidate_str(char *buf, uint16_t foundation, uint32_t priority,
                                      const char *ip, size_t ip_len, uint16_t port,
                                      const char *type, size_t type_len)
{
    size_t pos = 0;
    pos = cand_append(buf, pos, "candidate:", 10);
    pos = cand_append_u32(buf, pos, foundation);
    pos = cand_append(buf, pos, " 1 UDP ", 7);
    pos = cand_append_u32(buf, pos, priority);
    buf[pos++] = ' ';
    pos = cand_append(buf, pos, ip, ip_len);
    buf[pos++] = ' ';
    pos = cand_append_u32(buf, pos, port);
    pos = cand_append(buf, pos, " typ ", 5);
    pos = cand_append(buf, pos, type, type_len);
    buf[pos] = '\0';
    return pos;
}

/** Emit an NANORTC_EV_ICE_CANDIDATE event. */
static void rtc_emit_ice_candidate(nanortc_t *rtc, const char *candidate_str)
{
    nanortc_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = NANORTC_EV_ICE_CANDIDATE;
    evt.ice_candidate.candidate_str = candidate_str;
    evt.ice_candidate.end_of_candidates = false;
    rtc_emit_event_full(rtc, &evt);
}

/* Enqueue an output. Returns NANORTC_OK or NANORTC_ERR_BUFFER_TOO_SMALL. */
static inline int rtc_enqueue_output(nanortc_t *rtc, const nanortc_output_t *out)
{
    uint16_t used = (uint16_t)(rtc->out_tail - rtc->out_head);
    if (used >= NANORTC_OUT_QUEUE_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    uint16_t slot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
    rtc->out_queue[slot] = *out;
#if NANORTC_FEATURE_TURN
    /* Direct enqueues never need lazy TURN wrap; clear any stale flag from
     * the previous occupant of this ring slot. */
    rtc->out_wrap_meta[slot].via_turn = false;
#endif
    rtc->out_tail++;
    return NANORTC_OK;
}

/**
 * Enqueue a TRANSMIT output, deferring TURN wrap to nanortc_poll_output().
 *
 * Lazy wrap is required because the output queue stores only a pointer per
 * slot. Eagerly wrapping a burst of N media packets in one tick into the
 * shared turn_buf would leave all N enqueued outputs pointing at whatever
 * the LAST wrap wrote — silently dropping N-1 packets. Deferring serializes
 * wrap operations: each call to poll_output overwrites turn_buf with the
 * next packet, and the user transmits it before calling poll_output again.
 *
 * The wrap decision (selected_type == RELAY) is made here and stamped into
 * out_wrap_meta; the actual ChannelData / Send-indication encoding happens
 * in nanortc_poll_output().
 */
static int rtc_enqueue_transmit(nanortc_t *rtc, const uint8_t *data, size_t len,
                                const nanortc_addr_t *peer_dest)
{
    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = data;
    out.transmit.len = len;
    out.transmit.dest = *peer_dest;
    if (rtc->ice.selected_local_family != 0) {
        out.transmit.src.family = rtc->ice.selected_local_family;
        memcpy(out.transmit.src.addr, rtc->ice.selected_local_addr, NANORTC_ADDR_SIZE);
        out.transmit.src.port = rtc->ice.selected_local_port;
    }

    /* Capture slot before rtc_enqueue_output advances out_tail; rtc_enqueue_output
     * also clears out_wrap_meta[slot].via_turn, so we conditionally re-stamp it
     * below when the destination needs the lazy TURN wrap. */
    uint16_t slot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
    int rc = rtc_enqueue_output(rtc, &out);
    if (rc != NANORTC_OK) {
#if NANORTC_FEATURE_TURN
        rtc->stats_tx_queue_full++;
#endif
        NANORTC_LOGW("RTC", "tx queue full, dropping output");
        return rc;
    }

#if NANORTC_FEATURE_TURN
    /* selected_type → RELAY is set in ice_handle_stun() when a USE-CANDIDATE
     * check arrives via a TURN Data Indication / ChannelData unwrap (see the
     * via_turn arg plumbed through rtc_process_receive). Permission existence
     * alone is NOT a sufficient signal — we create permissions for every
     * remote candidate including LAN ones, and routing same-LAN responses
     * through a remote TURN server makes them appear from the wrong source
     * IP and the controlling browser drops them as ICE pair mismatches. */
    if (rtc->turn.configured && rtc->turn.state == NANORTC_TURN_ALLOCATED &&
        rtc->ice.selected_type == NANORTC_ICE_CAND_RELAY) {
        rtc->out_wrap_meta[slot].via_turn = true;
        rtc->out_wrap_meta[slot].peer_dest = *peer_dest;
        rtc->stats_enqueue_via_turn++;
    } else {
        rtc->stats_enqueue_direct++;
    }
#endif

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
    /* Propagate end-of-candidates from SDP to ICE (RFC 8838) */
    if (rtc->sdp.end_of_candidates) {
        rtc->ice.end_of_candidates = true;
    }
}

/* A3: Drain DTLS output into the transmit queue (uses relay wrapping if needed) */
static void rtc_drain_dtls_output(nanortc_t *rtc, const nanortc_addr_t *dest)
{
    size_t dout_len = 0;
    while (dtls_poll_output(&rtc->dtls, rtc->dtls_scratch, sizeof(rtc->dtls_scratch), &dout_len) ==
               NANORTC_OK &&
           dout_len > 0) {
        rtc_enqueue_transmit(rtc, rtc->dtls_scratch, dout_len, dest);
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
    uint8_t mc = 0;
    for (uint8_t i = 0; i < rtc->media_count && mc < NANORTC_MAX_MEDIA_TRACKS; i++) {
        nanortc_track_t *m = &rtc->media[i];
        if (!m->active) {
            continue;
        }
        if (m->direction == NANORTC_DIR_RECVONLY || m->direction == NANORTC_DIR_INACTIVE) {
            continue;
        }
        event.connected.mids[mc] = m->mid;
        mc++;
    }
    event.connected.mid_count = mc;
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

    /* Process ICE servers (WebRTC RTCConfiguration.iceServers) */
#if NANORTC_FEATURE_TURN
    turn_init(&rtc->turn);
#endif
    if (cfg->ice_servers && cfg->ice_server_count > 0) {
        int irc = rtc_apply_ice_servers(rtc, cfg->ice_servers, cfg->ice_server_count);
        if (irc != NANORTC_OK) {
            return irc;
        }
    }

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

    if (rtc->sdp.local_candidate_count >= NANORTC_MAX_LOCAL_CANDIDATES) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    uint8_t idx = rtc->sdp.local_candidate_count;
    size_t ip_len = strlen(ip); /* NANORTC_SAFE: API boundary */
    if (ip_len >= sizeof(rtc->sdp.local_candidates[idx].addr)) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Store string form in SDP state for answer/offer generation */
    memcpy(rtc->sdp.local_candidates[idx].addr, ip, ip_len + 1);
    rtc->sdp.local_candidates[idx].port = port;

    /* Store binary form in ICE state for connectivity checks */
    uint8_t parsed_addr[NANORTC_ADDR_SIZE];
    memset(parsed_addr, 0, sizeof(parsed_addr));
    uint8_t family = 0;
    int rc = addr_parse_auto(ip, ip_len, parsed_addr, &family);
    if (rc != NANORTC_OK) {
        return rc;
    }
    rtc->ice.local_candidates[idx].family = family;
    memcpy(rtc->ice.local_candidates[idx].addr, parsed_addr, NANORTC_ADDR_SIZE);
    rtc->ice.local_candidates[idx].port = port;
    rtc->ice.local_candidates[idx].type = NANORTC_ICE_CAND_HOST;

    rtc->sdp.local_candidate_count = idx + 1;
    rtc->ice.local_candidate_count = idx + 1;

    /* Emit trickle ICE candidate event (host) */
    rtc_build_candidate_str(rtc->host_cand_str, (uint16_t)(idx + 1), ICE_HOST_PRIORITY(idx), ip,
                            ip_len, port, "host", 4);
    rtc_emit_ice_candidate(rtc, rtc->host_cand_str);

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
    uint8_t cand_type = NANORTC_ICE_CAND_HOST; /* default if "typ" attr missing */

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

        /* Optional "typ <type>" attribute (RFC 8839 §5.1). The candidate
         * string may also carry "raddr/rport" and other extensions after
         * the type — we only need the type token itself. F5: previously
         * unparsed, leaving every remote candidate stuck at type=HOST. */
        while (*p == ' ')
            p++;
        while (*p) {
            if (p[0] == 't' && p[1] == 'y' && p[2] == 'p' && (p[3] == ' ' || p[3] == '\t')) {
                p += 4;
                while (*p == ' ' || *p == '\t')
                    p++;
                if (p[0] == 'h' && p[1] == 'o' && p[2] == 's' && p[3] == 't') {
                    cand_type = NANORTC_ICE_CAND_HOST;
                } else if (p[0] == 's' && p[1] == 'r' && p[2] == 'f' && p[3] == 'l' &&
                           p[4] == 'x') {
                    cand_type = NANORTC_ICE_CAND_SRFLX;
                } else if (p[0] == 'p' && p[1] == 'r' && p[2] == 'f' && p[3] == 'l' &&
                           p[4] == 'x') {
                    cand_type = NANORTC_ICE_CAND_SRFLX; /* peer-reflexive: treat as srflx */
                } else if (p[0] == 'r' && p[1] == 'e' && p[2] == 'l' && p[3] == 'a' &&
                           p[4] == 'y') {
                    cand_type = NANORTC_ICE_CAND_RELAY;
                }
                break;
            }
            /* Skip current token */
            while (*p && *p != ' ' && *p != '\t')
                p++;
            while (*p == ' ' || *p == '\t')
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
    rtc->ice.remote_candidates[idx].type = cand_type;
    rtc->ice.remote_candidate_count++;

    NANORTC_LOGI("RTC", "remote candidate added");
    return NANORTC_OK;
}

int nanortc_poll_output(nanortc_t *rtc, nanortc_output_t *out)
{
    if (!rtc || !out) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    while (rtc->out_head != rtc->out_tail) {
        uint16_t slot = rtc->out_head & (NANORTC_OUT_QUEUE_SIZE - 1);
        *out = rtc->out_queue[slot];

#if NANORTC_FEATURE_TURN
        if (out->type == NANORTC_OUTPUT_TRANSMIT && rtc->out_wrap_meta[slot].via_turn) {
            const nanortc_addr_t *peer = &rtc->out_wrap_meta[slot].peer_dest;
            uint16_t channel = 0;
            size_t wrap_len = 0;
            int wrc;

            if (turn_find_channel_for_peer(&rtc->turn, peer->addr, peer->family, &channel)) {
                /* ChannelData (RFC 5766 §11.4) — 4 bytes overhead, preferred
                 * once a channel is bound for this peer. */
                wrc = nano_turn_wrap_channel_data(channel, out->transmit.data, out->transmit.len,
                                                  rtc->turn_buf, sizeof(rtc->turn_buf), &wrap_len);
            } else {
                /* Send indication (RFC 5766 §10) — used pre-ChannelBind or
                 * for peers without a channel. ~36 B (IPv4) / ~48 B (IPv6)
                 * overhead. turn_buf is sized via NANORTC_TURN_BUF_SIZE to
                 * accommodate the largest payload + Send-indication header. */
                wrc = turn_wrap_send(peer->addr, peer->family, peer->port, out->transmit.data,
                                     out->transmit.len, rtc->turn_buf, sizeof(rtc->turn_buf),
                                     &wrap_len);
            }

            if (wrc != NANORTC_OK || wrap_len == 0) {
                /* Wrap failed (e.g. payload bigger than turn_buf). Drop the
                 * output silently and try the next one — the caller never
                 * needs to know about TURN-internal errors. */
                rtc->stats_wrap_dropped++;
                NANORTC_LOGW("TURN", "lazy wrap failed, dropping output");
                rtc->out_head++;
                continue;
            }

            out->transmit.data = rtc->turn_buf;
            out->transmit.len = wrap_len;
            out->transmit.dest.family = (rtc->turn.server_family == STUN_FAMILY_IPV4) ? 4 : 6;
            memcpy(out->transmit.dest.addr, rtc->turn.server_addr, NANORTC_ADDR_SIZE);
            out->transmit.dest.port = rtc->turn.server_port;
        }
#endif /* NANORTC_FEATURE_TURN */

        rtc->out_head++;
        return NANORTC_OK;
    }
    return NANORTC_ERR_NO_DATA;
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
 * Internal: deliver one SCTP message to DataChannel layer + emit events
 * ---------------------------------------------------------------- */

static void rtc_deliver_sctp_to_dc(nanortc_t *rtc)
{
    dc_handle_message(&rtc->datachannel, rtc->sctp.delivered_stream, rtc->sctp.delivered_ppid,
                      rtc->sctp.delivered_data, rtc->sctp.delivered_len);
    rtc->sctp.has_delivered = false;

    /* Emit DC events using typed event structs */
    if (rtc->sctp.delivered_ppid == DCEP_PPID_CONTROL) {
        if (!rtc->datachannel.last_was_open) {
            return;
        }
        /* CHANNEL_OPEN event */
        nanortc_event_t oevt;
        memset(&oevt, 0, sizeof(oevt));
        oevt.type = NANORTC_EV_DATACHANNEL_OPEN;
        oevt.datachannel_open.id = rtc->sctp.delivered_stream;
        for (uint8_t ci = 0; ci < rtc->datachannel.channel_count; ci++) {
            if (rtc->datachannel.channels[ci].stream_id == rtc->sctp.delivered_stream) {
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
}

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
            rtc_enqueue_transmit(rtc, rtc->dtls_scratch, enc_len, dest);
            enc_len = 0;
        }
        nsctp_out = 0;
    }
}
#endif /* NANORTC_FEATURE_DATACHANNEL */

/* ----------------------------------------------------------------
 * rtc_process_receive — RFC 7983 demux
 *
 * @p via_turn is set to true on the recursive calls that follow a TURN
 * Data Indication / ChannelData unwrap. The flag is needed by ICE so it can
 * mark the selected pair as RELAY (and thereby gate the TURN-wrap on the
 * outbound side); without it the controlled side cannot tell a directly-
 * arriving Binding Request from one that just came through our relay,
 * because both expose the same `src` peer address.
 * ---------------------------------------------------------------- */

static int rtc_process_receive(nanortc_t *rtc, const uint8_t *data, size_t len,
                               const nanortc_addr_t *src, bool via_turn)
{
    uint8_t first = data[0];

#if NANORTC_FEATURE_TURN
    /* RFC 7983 §3: ChannelData [0x40-0x7F] from TURN server */
    if (turn_is_channel_data(data, len) && rtc->turn.configured &&
        turn_is_from_server(&rtc->turn, src->addr, src->family, src->port)) {
        uint16_t channel = 0;
        const uint8_t *cd_payload = NULL;
        size_t cd_len = 0;
        if (turn_unwrap_channel_data(data, len, &channel, &cd_payload, &cd_len) == NANORTC_OK) {
            uint8_t peer_addr[NANORTC_ADDR_SIZE];
            uint8_t peer_family = 0;
            uint16_t peer_port = 0;
            if (turn_find_peer_for_channel(&rtc->turn, channel, peer_addr, &peer_family,
                                           &peer_port)) {
                nanortc_addr_t peer_src;
                memset(&peer_src, 0, sizeof(peer_src));
                peer_src.family = (peer_family == STUN_FAMILY_IPV4) ? 4 : 6;
                memcpy(peer_src.addr, peer_addr, NANORTC_ADDR_SIZE);
                peer_src.port = peer_port;
                return rtc_process_receive(rtc, cd_payload, cd_len, &peer_src, true);
            }
        }
        return NANORTC_OK;
    }
#endif /* NANORTC_FEATURE_TURN */

    /* RFC 7983 §3: demultiplexing by first byte */
    if (first <= 3) {
        /* STUN [0x00-0x03] */

#if NANORTC_FEATURE_TURN
        /* TURN: intercept packets from the TURN server */
        if (rtc->turn.configured &&
            turn_is_from_server(&rtc->turn, src->addr, src->family, src->port)) {
            /* Try Data indication first (relayed peer data) */
            uint8_t peer_addr[NANORTC_ADDR_SIZE];
            uint8_t peer_family = 0;
            uint16_t peer_port = 0;
            const uint8_t *payload = NULL;
            size_t payload_len = 0;
            int urc = turn_unwrap_data(data, len, peer_addr, &peer_family, &peer_port, &payload,
                                       &payload_len);
            if (urc == NANORTC_OK && payload_len > 0) {
                /* Re-dispatch unwrapped inner packet with peer's address */
                nanortc_addr_t peer_src;
                memset(&peer_src, 0, sizeof(peer_src));
                peer_src.family = (peer_family == STUN_FAMILY_IPV4) ? 4 : 6;
                memcpy(peer_src.addr, peer_addr, NANORTC_ADDR_SIZE);
                peer_src.port = peer_port;
                return rtc_process_receive(rtc, payload, payload_len, &peer_src, true);
            }

            /* Not a Data indication — try as TURN response */
            nano_turn_state_t prev_state = rtc->turn.state;
            int trc = turn_handle_response(&rtc->turn, data, len, rtc->config.crypto);
            (void)trc;

            /* On fresh allocation: emit relay candidate + create permission */
            if (prev_state != NANORTC_TURN_ALLOCATED && rtc->turn.state == NANORTC_TURN_ALLOCATED) {
                /* Store relay address in SDP state for offer/answer generation */
                size_t ip_len = 0;
                uint8_t relay_fam = (rtc->turn.relay_family == STUN_FAMILY_IPV4) ? 4 : 6;
                addr_format(rtc->turn.relay_addr, relay_fam, rtc->sdp.relay_candidate_ip,
                            sizeof(rtc->sdp.relay_candidate_ip), &ip_len);
                rtc->sdp.relay_candidate_port = rtc->turn.relay_port;
                rtc->sdp.has_relay_candidate = true;

                /* Emit trickle ICE candidate event (relay) */
                rtc_build_candidate_str(rtc->relay_cand_str, 2, 16777215,
                                        rtc->sdp.relay_candidate_ip, ip_len, rtc->turn.relay_port,
                                        "relay", 5);
                rtc_emit_ice_candidate(rtc, rtc->relay_cand_str);

                /* Permission fan-out is driven from rtc_process_timers so a
                 * single shared turn_buf can serialize one CreatePermission
                 * per tick across all remote candidates (and across trickle
                 * additions after allocation). */
            }
            return NANORTC_OK;
        }
#endif /* NANORTC_FEATURE_TURN */

        /* STUN server response for srflx discovery (RFC 8445 §5.1.1.1) */
        if (rtc->stun_server_configured && !rtc->srflx_discovered) {
            size_t addr_cmp_len = (src->family == 4) ? 4 : 16;
            bool from_stun_server =
                (src->family == rtc->stun_server_family && src->port == rtc->stun_server_port &&
                 memcmp(src->addr, rtc->stun_server_addr, addr_cmp_len) == 0);
            if (from_stun_server) {
                stun_msg_t smsg;
                int src2 = stun_parse(data, len, &smsg);
                if (src2 == NANORTC_OK && smsg.type == STUN_BINDING_RESPONSE &&
                    memcmp(smsg.transaction_id, rtc->stun_txid, STUN_TXID_SIZE) == 0 &&
                    smsg.mapped_family != 0) {
                    /* Extract XOR-MAPPED-ADDRESS as srflx candidate */
                    uint8_t mapped_fam = (smsg.mapped_family == STUN_FAMILY_IPV4) ? 4 : 6;
                    size_t ip_len = 0;
                    addr_format(smsg.mapped_addr, mapped_fam, rtc->sdp.srflx_candidate_ip,
                                sizeof(rtc->sdp.srflx_candidate_ip), &ip_len);
                    rtc->sdp.srflx_candidate_port = smsg.mapped_port;
                    rtc->sdp.has_srflx_candidate = true;
                    rtc->srflx_discovered = true;

                    /* Emit trickle ICE candidate event (srflx) */
                    rtc_build_candidate_str(rtc->srflx_cand_str, 3, 1090519295,
                                            rtc->sdp.srflx_candidate_ip, ip_len, smsg.mapped_port,
                                            "srflx", 5);
                    rtc_emit_ice_candidate(rtc, rtc->srflx_cand_str);

                    NANORTC_LOGI("RTC", "srflx candidate discovered");
                }
                return NANORTC_OK;
            }
        }

        bool was_consent_pending = rtc->ice.consent_pending;
        size_t resp_len = 0;
        int rc = ice_handle_stun(&rtc->ice, data, len, src, via_turn, rtc->config.crypto,
                                 rtc->stun_buf, sizeof(rtc->stun_buf), &resp_len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        /* Refresh consent expiry on any successful STUN from the selected pair.
         * RFC 7675 §5.1: receiving a valid STUN message on the selected pair
         * proves the remote endpoint is alive and consents to receive traffic. */
        if (rtc->ice.state == NANORTC_ICE_STATE_CONNECTED && rtc->ice.consent_expiry_ms > 0 &&
            src->family == rtc->ice.selected_family && src->port == rtc->ice.selected_port &&
            memcmp(src->addr, rtc->ice.selected_addr, NANORTC_ADDR_SIZE) == 0) {
            rtc->ice.consent_expiry_ms = rtc->now_ms + NANORTC_ICE_CONSENT_TIMEOUT_MS;
        }
        /* Also clear pending if our consent check got a response */
        if (was_consent_pending && !rtc->ice.consent_pending) {
            rtc->ice.consent_expiry_ms = rtc->now_ms + NANORTC_ICE_CONSENT_TIMEOUT_MS;
        }

        /* Enqueue STUN response for transmission. Use rtc_enqueue_transmit so
         * that if `src` is a peer reached via the TURN relay (the recursive
         * call from turn_unwrap_data / turn_unwrap_channel_data hands us the
         * inner peer address, not the TURN server), the response is wrapped
         * in a Send indication / ChannelData and routed back through the
         * relay. Direct sendto() on a NAT'd cellular peer address has no
         * route from the device. */
        if (resp_len > 0) {
            rtc_enqueue_transmit(rtc, rtc->stun_buf, resp_len, src);
        }

        /* Check for ICE state transition → init DTLS + emit event */
        if (rtc->ice.state == NANORTC_ICE_STATE_CONNECTED &&
            rtc->state < NANORTC_STATE_ICE_CONNECTED) {
            rtc->state = NANORTC_STATE_ICE_CONNECTED;
            /* Arm consent freshness timers (RFC 7675) */
            rtc->ice.consent_next_ms = rtc->now_ms + NANORTC_ICE_CONSENT_INTERVAL_MS;
            rtc->ice.consent_expiry_ms = rtc->now_ms + NANORTC_ICE_CONSENT_TIMEOUT_MS;
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
                    rtc_deliver_sctp_to_dc(rtc);
                }

                /* Drain gap-fill delivery queue (out-of-order reordering) */
                while (nsctp_poll_delivery(&rtc->sctp) == NANORTC_OK) {
                    rtc_deliver_sctp_to_dc(rtc);
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
            /* RTCP packet — SRTCP unprotect then parse */
            if (len > sizeof(rtc->stun_buf)) {
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            /* Copy to scratch for in-place SRTCP unprotect */
            memcpy(rtc->stun_buf, data, len);
            size_t rtcp_len = 0;
            int urc = nano_srtp_unprotect_rtcp(&rtc->srtp, rtc->stun_buf, len, &rtcp_len);
            if (urc != NANORTC_OK) {
                return NANORTC_OK; /* Silently discard bad SRTCP packets */
            }
            nano_rtcp_info_t info;
            memset(&info, 0, sizeof(info));
            int rrc = rtcp_parse(rtc->stun_buf, rtcp_len, &info);
            if (rrc == NANORTC_OK) {
                if (info.type == RTCP_SR) {
                    /* Sender Report — update receiver stats for DLSR (RFC 3550 §6.4.1).
                     * Compact NTP = middle 32 bits of NTP timestamp. */
                    int mid = ssrc_map_lookup(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, info.ssrc);
                    if (mid >= 0) {
                        nanortc_track_t *m =
                            track_find_by_mid(rtc->media, rtc->media_count, (uint8_t)mid);
                        if (m) {
                            m->rtcp.last_sr_ntp =
                                ((info.ntp_sec & 0xFFFFu) << 16) | (info.ntp_frac >> 16);
                            m->rtcp.last_sr_recv_ms = rtc->now_ms;
                        }
                    }
                } else if (info.type == RTCP_PSFB) {
                    /* PSFB — check FMT to distinguish PLI (FMT=1) from REMB (FMT=15) */
                    uint8_t psfb_fmt = rtc->stun_buf[0] & 0x1F;
#if NANORTC_FEATURE_VIDEO
                    if (psfb_fmt == BWE_REMB_FMT) {
                        /* REMB — feed to bandwidth estimator */
                        uint32_t prev_bps = rtc->bwe.estimated_bitrate;
                        bwe_on_rtcp_feedback(&rtc->bwe, rtc->stun_buf, rtcp_len, rtc->now_ms);
                        /* Emit event if estimate changed significantly */
                        if (bwe_should_emit_event(&rtc->bwe)) {
                            nanortc_event_t bwe_evt;
                            memset(&bwe_evt, 0, sizeof(bwe_evt));
                            bwe_evt.type = NANORTC_EV_BITRATE_ESTIMATE;
                            bwe_evt.bitrate_estimate.bitrate_bps = rtc->bwe.estimated_bitrate;
                            bwe_evt.bitrate_estimate.prev_bitrate_bps = prev_bps;
                            rtc_emit_event_full(rtc, &bwe_evt);
                        }
                    } else
#endif
                        if (psfb_fmt == 1) {
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
#if NANORTC_FEATURE_VIDEO
                } else if (info.type == RTCP_RTPFB) {
                    /* Generic NACK (RFC 4585 §6.2.1) — retransmit lost packets
                     * from pkt_ring if they are still available. */
                    uint8_t rtpfb_fmt = rtc->stun_buf[0] & 0x1F;
                    if (rtpfb_fmt == 1) {
                        /* Expand PID + BLP into up to 17 lost seq numbers and
                         * retransmit each one found in the pkt_ring. */
                        uint16_t lost[17];
                        int lost_count = 0;
                        lost[lost_count++] = info.nack_pid;
                        for (int bit = 0; bit < 16; bit++) {
                            if (info.nack_blp & (1u << bit)) {
                                lost[lost_count++] = (uint16_t)(info.nack_pid + 1 + bit);
                            }
                        }
                        int retx = 0;
                        for (int i = 0; i < lost_count; i++) {
                            /* Linear scan over pkt_ring_meta for a matching seq.
                             * OUT_QUEUE_SIZE is small (32-256) so this is fast. */
                            for (uint16_t s = 0; s < NANORTC_OUT_QUEUE_SIZE; s++) {
                                if (rtc->pkt_ring_meta[s].len > 0 &&
                                    rtc->pkt_ring_meta[s].seq == lost[i]) {
                                    rtc_enqueue_transmit(rtc, rtc->pkt_ring[s],
                                                         rtc->pkt_ring_meta[s].len,
                                                         &rtc->remote_addr);
                                    retx++;
                                    break;
                                }
                            }
                        }
                        if (retx > 0) {
                            NANORTC_LOGD("NACK", "retransmitted packet(s)");
                        }
                    }
#endif /* NANORTC_FEATURE_VIDEO */
                }
            }
            return NANORTC_OK;
        }

        /* RTP packet — demux by SSRC → MID.
         * Use stun_buf as scratch for in-place SRTP unprotect: under Sans I/O
         * single-threaded invocation, STUN/RTCP/RTP use of stun_buf is
         * time-disjoint. In media builds stun_buf is sized to
         * NANORTC_MEDIA_BUF_SIZE (see nanortc_config.h), so a full RTP packet
         * fits; in DC-only builds this path is unreachable. */
        if (len > sizeof(rtc->stun_buf)) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        uint8_t *pkt = rtc->stun_buf;
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
        uint8_t local_before = rtc->ice.current_local;
        uint8_t remote_before = rtc->ice.current_remote;
        size_t out_len = 0;
        int rc = ice_generate_check(&rtc->ice, now_ms, rtc->config.crypto, rtc->stun_buf,
                                    sizeof(rtc->stun_buf), &out_len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        if (out_len > 0 && remote_before < rtc->ice.remote_candidate_count &&
            local_before < rtc->ice.local_candidate_count) {
            nanortc_output_t out;
            memset(&out, 0, sizeof(out));
            out.type = NANORTC_OUTPUT_TRANSMIT;
            out.transmit.data = rtc->stun_buf;
            out.transmit.len = out_len;
            /* Destination: remote candidate */
            out.transmit.dest.family = rtc->ice.remote_candidates[remote_before].family;
            memcpy(out.transmit.dest.addr, rtc->ice.remote_candidates[remote_before].addr,
                   NANORTC_ADDR_SIZE);
            out.transmit.dest.port = rtc->ice.remote_candidates[remote_before].port;
            /* Source: local candidate */
            out.transmit.src.family = rtc->ice.local_candidates[local_before].family;
            memcpy(out.transmit.src.addr, rtc->ice.local_candidates[local_before].addr,
                   NANORTC_ADDR_SIZE);
            out.transmit.src.port = rtc->ice.local_candidates[local_before].port;
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
            /* TD-018: emit DISCONNECTED symmetric to the consent-expiry path
             * so applications see a consistent signal across both failure
             * modes (ICE check exhaustion and consent loss). */
            nanortc_event_t fdev;
            memset(&fdev, 0, sizeof(fdev));
            fdev.type = NANORTC_EV_DISCONNECTED;
            rtc_emit_event_full(rtc, &fdev);
            rtc->state = NANORTC_STATE_CLOSED;
        }
    }

    /* Consent freshness (RFC 7675): periodic STUN keepalive on connected path */
    if (rtc->ice.state == NANORTC_ICE_STATE_CONNECTED) {
        /* Check for consent timeout */
        if (ice_consent_expired(&rtc->ice, now_ms)) {
            rtc->ice.state = NANORTC_ICE_STATE_DISCONNECTED;
            nanortc_event_t dice;
            memset(&dice, 0, sizeof(dice));
            dice.type = NANORTC_EV_ICE_STATE_CHANGE;
            dice.ice_state = (uint16_t)NANORTC_ICE_STATE_DISCONNECTED;
            rtc_emit_event_full(rtc, &dice);
            /* Emit DISCONNECTED application event */
            nanortc_event_t dev;
            memset(&dev, 0, sizeof(dev));
            dev.type = NANORTC_EV_DISCONNECTED;
            rtc_emit_event_full(rtc, &dev);
            rtc->state = NANORTC_STATE_CLOSED;
        } else {
            /* Generate consent check if due */
            size_t consent_len = 0;
            int crc = ice_generate_consent(&rtc->ice, now_ms, rtc->config.crypto, rtc->stun_buf,
                                           sizeof(rtc->stun_buf), &consent_len);
            if (crc == NANORTC_OK && consent_len > 0) {
                /* Use rtc_enqueue_transmit so the consent check is wrapped in
                 * a TURN Send Indication / ChannelData when the selected pair
                 * is RELAY. Direct sendto() to a NAT'd cellular peer address
                 * never lands; the consent freshness timer would expire after
                 * NANORTC_ICE_CONSENT_TIMEOUT_MS and the call would silently
                 * drop. RFC 7675 §5.1. */
                nanortc_addr_t consent_dest;
                memset(&consent_dest, 0, sizeof(consent_dest));
                consent_dest.family = rtc->ice.selected_family;
                memcpy(consent_dest.addr, rtc->ice.selected_addr, NANORTC_ADDR_SIZE);
                consent_dest.port = rtc->ice.selected_port;
                rtc_enqueue_transmit(rtc, rtc->stun_buf, consent_len, &consent_dest);
            }
        }
    }

#if NANORTC_FEATURE_TURN
    /* TURN: Allocate / Refresh / CreatePermission lifecycle */
    if (rtc->turn.configured) {
        nanortc_addr_t turn_dest;
        memset(&turn_dest, 0, sizeof(turn_dest));
        turn_dest.family = rtc->turn.server_family;
        memcpy(turn_dest.addr, rtc->turn.server_addr, NANORTC_ADDR_SIZE);
        turn_dest.port = rtc->turn.server_port;

        /* All TURN-generated outputs use turn_buf, NOT stun_buf, so the
         * subsequent STUN srflx block doesn't overwrite the pending TURN
         * packet between enqueue and dispatch (both share only a pointer
         * in nanortc_output_t, so whoever writes last wins). Fixed as part
         * of investigating a "TURN Allocate corrupted into STUN Binding
         * Request" packet-level bug on uipcat-camera. */
        if (rtc->turn.state == NANORTC_TURN_IDLE) {
            /* Start Allocate when we begin ICE checking */
            size_t alloc_len = 0;
            int trc = turn_start_allocate(&rtc->turn, rtc->config.crypto, rtc->turn_buf,
                                          sizeof(rtc->turn_buf), &alloc_len);
            if (trc == NANORTC_OK && alloc_len > 0) {
                nanortc_output_t out;
                memset(&out, 0, sizeof(out));
                out.type = NANORTC_OUTPUT_TRANSMIT;
                out.transmit.data = rtc->turn_buf;
                out.transmit.len = alloc_len;
                out.transmit.dest = turn_dest;
                rtc_enqueue_output(rtc, &out);
            }
        } else if (rtc->turn.state == NANORTC_TURN_CHALLENGED) {
            /* Retry Allocate with credentials after 401 */
            size_t alloc_len = 0;
            int trc = turn_start_allocate(&rtc->turn, rtc->config.crypto, rtc->turn_buf,
                                          sizeof(rtc->turn_buf), &alloc_len);
            if (trc == NANORTC_OK && alloc_len > 0) {
                nanortc_output_t out;
                memset(&out, 0, sizeof(out));
                out.type = NANORTC_OUTPUT_TRANSMIT;
                out.transmit.data = rtc->turn_buf;
                out.transmit.len = alloc_len;
                out.transmit.dest = turn_dest;
                rtc_enqueue_output(rtc, &out);
            }
        } else if (rtc->turn.state == NANORTC_TURN_ALLOCATED) {
            /* Initial / trickle CreatePermission fan-out.
             * Walk remote_candidates[] and fire one CreatePermission per tick
             * for the first peer that doesn't yet have a permission table
             * entry. One per tick is required because turn_buf is shared
             * scratch and the output queue holds only pointers, so issuing N
             * back-to-back CreatePermissions in one tick would corrupt all
             * but the last. The user's poll loop drains the output queue
             * between ticks, so N permissions are sent in roughly N tick
             * intervals (<100ms total for typical browser candidate counts). */
            for (uint8_t i = 0; i < rtc->ice.remote_candidate_count; i++) {
                nano_ice_candidate_t *c = &rtc->ice.remote_candidates[i];
                size_t addr_len = (c->family == 4) ? 4 : 16;
                bool has_perm = false;
                for (uint8_t j = 0; j < rtc->turn.permission_count; j++) {
                    if (rtc->turn.permissions[j].family == c->family &&
                        memcmp(rtc->turn.permissions[j].addr, c->addr, addr_len) == 0) {
                        has_perm = true;
                        break;
                    }
                }
                if (has_perm) {
                    continue;
                }
                size_t perm_len = 0;
                int prc = turn_create_permission(&rtc->turn, c->addr, c->family, c->port,
                                                 rtc->config.crypto, rtc->turn_buf,
                                                 sizeof(rtc->turn_buf), &perm_len);
                if (prc == NANORTC_OK && perm_len > 0) {
                    nanortc_output_t out;
                    memset(&out, 0, sizeof(out));
                    out.type = NANORTC_OUTPUT_TRANSMIT;
                    out.transmit.data = rtc->turn_buf;
                    out.transmit.len = perm_len;
                    out.transmit.dest = turn_dest;
                    rtc_enqueue_output(rtc, &out);
                    /* Defer the periodic refresh past this tick so the
                     * refresh block below doesn't immediately overwrite
                     * turn_buf with a refresh of permissions[0]. */
                    rtc->turn.permission_at_ms = now_ms + 240000;
                }
                break; /* one CreatePermission per tick */
            }

            /* Periodic Refresh (RFC 5766 §7) */
            size_t ref_len = 0;
            int trc = turn_generate_refresh(&rtc->turn, now_ms, rtc->config.crypto, rtc->turn_buf,
                                            sizeof(rtc->turn_buf), &ref_len);
            if (trc == NANORTC_OK && ref_len > 0) {
                nanortc_output_t out;
                memset(&out, 0, sizeof(out));
                out.type = NANORTC_OUTPUT_TRANSMIT;
                out.transmit.data = rtc->turn_buf;
                out.transmit.len = ref_len;
                out.transmit.dest = turn_dest;
                rtc_enqueue_output(rtc, &out);
            }

            /* Permission refresh (RFC 5766 §8: expires at 5 min, refresh at 4 min) */
            {
                size_t perm_len = 0;
                int prc = turn_generate_permission_refresh(&rtc->turn, now_ms, rtc->config.crypto,
                                                           rtc->turn_buf, sizeof(rtc->turn_buf),
                                                           &perm_len);
                if (prc == NANORTC_OK && perm_len > 0) {
                    nanortc_output_t out;
                    memset(&out, 0, sizeof(out));
                    out.type = NANORTC_OUTPUT_TRANSMIT;
                    out.transmit.data = rtc->turn_buf;
                    out.transmit.len = perm_len;
                    out.transmit.dest = turn_dest;
                    rtc_enqueue_output(rtc, &out);
                }
            }

            /* ChannelBind refresh (RFC 5766 §11: expires at 10 min, refresh at 9 min) */
            {
                size_t chan_len = 0;
                int crc =
                    turn_generate_channel_refresh(&rtc->turn, now_ms, rtc->config.crypto,
                                                  rtc->turn_buf, sizeof(rtc->turn_buf), &chan_len);
                if (crc == NANORTC_OK && chan_len > 0) {
                    nanortc_output_t out;
                    memset(&out, 0, sizeof(out));
                    out.type = NANORTC_OUTPUT_TRANSMIT;
                    out.transmit.data = rtc->turn_buf;
                    out.transmit.len = chan_len;
                    out.transmit.dest = turn_dest;
                    rtc_enqueue_output(rtc, &out);
                }
            }
        }
    }
#endif /* NANORTC_FEATURE_TURN */

    /* STUN: server-reflexive candidate discovery (RFC 8445 §5.1.1.1) */
    if (rtc->stun_server_configured && !rtc->srflx_discovered) {
        if (rtc->stun_retry_at_ms == 0 || now_ms >= rtc->stun_retry_at_ms) {
            if (rtc->stun_retries < 3) {
                /* Generate random txid on first attempt */
                if (rtc->stun_retries == 0) {
                    rtc->config.crypto->random_bytes(rtc->stun_txid, STUN_TXID_SIZE);
                }

                size_t req_len = 0;
                int src = stun_encode_simple_binding_request(rtc->stun_txid, rtc->stun_buf,
                                                             sizeof(rtc->stun_buf), &req_len);
                if (src == NANORTC_OK && req_len > 0) {
                    nanortc_output_t out;
                    memset(&out, 0, sizeof(out));
                    out.type = NANORTC_OUTPUT_TRANSMIT;
                    out.transmit.data = rtc->stun_buf;
                    out.transmit.len = req_len;
                    out.transmit.dest.family = rtc->stun_server_family;
                    memcpy(out.transmit.dest.addr, rtc->stun_server_addr, NANORTC_ADDR_SIZE);
                    out.transmit.dest.port = rtc->stun_server_port;
                    /* Source: first local candidate (srflx base) */
                    if (rtc->ice.local_candidate_count > 0) {
                        out.transmit.src.family = rtc->ice.local_candidates[0].family;
                        memcpy(out.transmit.src.addr, rtc->ice.local_candidates[0].addr,
                               NANORTC_ADDR_SIZE);
                        out.transmit.src.port = rtc->ice.local_candidates[0].port;
                    }
                    rtc_enqueue_output(rtc, &out);
                }

                rtc->stun_retries++;
                rtc->stun_retry_at_ms = now_ms + 500; /* retry in 500ms */
            }
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

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* Periodic RTCP Sender Report (RFC 3550 §6.2) */
    if (rtc->srtp.ready && (now_ms - rtc->last_rtcp_send_ms) >= NANORTC_RTCP_INTERVAL_MS) {
        rtc->last_rtcp_send_ms = now_ms;

        /* NTP timestamp from monotonic now_ms (RFC 3550 §4):
         * No wall-clock available in Sans I/O; relative time is sufficient
         * for DLSR calculation at the receiver. */
        uint32_t ntp_sec = now_ms / 1000;
        uint32_t ntp_frac = (uint32_t)((uint64_t)(now_ms % 1000) * 4294967u);

        for (uint8_t ti = 0; ti < rtc->media_count; ti++) {
            nanortc_track_t *m = &rtc->media[ti];
            if (!m->active)
                continue;
            /* Only send SR for tracks that are sending */
            if (m->direction == NANORTC_DIR_RECVONLY || m->direction == NANORTC_DIR_INACTIVE)
                continue;
            if (m->rtcp.packets_sent == 0)
                continue;

            /* RTP timestamp corresponding to NTP time */
            uint32_t clock_rate = (m->kind == NANORTC_TRACK_VIDEO) ? 90000 : m->sample_rate;
            uint32_t rtp_ts = (uint32_t)((uint64_t)now_ms * clock_rate / 1000);

            /* Generate SR + SRTCP protect into stun_buf (safe: ICE checks
             * only run when NOT connected, see guard above) */
            size_t sr_len = 0;
            int sr_rc = rtcp_generate_sr(&m->rtcp, ntp_sec, ntp_frac, rtp_ts, rtc->stun_buf,
                                         sizeof(rtc->stun_buf), &sr_len);
            if (sr_rc != NANORTC_OK)
                continue;

            size_t srtcp_len = 0;
            sr_rc = nano_srtp_protect_rtcp(&rtc->srtp, rtc->stun_buf, sr_len, &srtcp_len);
            if (sr_rc != NANORTC_OK)
                continue;

            rtc_enqueue_transmit(rtc, rtc->stun_buf, srtcp_len, &rtc->remote_addr);
        }
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
        return rtc_process_receive(rtc, data, len, src, false);
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

int nanortc_datachannel_send(nanortc_t *rtc, uint16_t id, const void *data, size_t len)
{
    if (!rtc || !data) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANORTC_STATE_CONNECTED) {
        NANORTC_LOGW("DC", "send failed: not connected");
        return NANORTC_ERR_STATE;
    }

    uint32_t ppid = (len > 0) ? DCEP_PPID_BINARY : DCEP_PPID_BINARY_EMPTY;
    int rc = nsctp_send(&rtc->sctp, id, ppid, (const uint8_t *)data, len);
    if (rc == NANORTC_ERR_BUFFER_TOO_SMALL) {
        NANORTC_LOGD("DC", "send would block (SCTP buffer full)");
        return NANORTC_ERR_WOULD_BLOCK;
    }
    return rc;
}

int nanortc_datachannel_send_string(nanortc_t *rtc, uint16_t id, const char *str)
{
    if (!rtc || !str) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state != NANORTC_STATE_CONNECTED) {
        return NANORTC_ERR_STATE;
    }

    size_t len = strlen(str); /* NANORTC_SAFE: API boundary */

    uint32_t ppid = (len > 0) ? DCEP_PPID_STRING : DCEP_PPID_STRING_EMPTY;
    int rc = nsctp_send(&rtc->sctp, id, ppid, (const uint8_t *)str, len);
    if (rc == NANORTC_ERR_BUFFER_TOO_SMALL) {
        return NANORTC_ERR_WOULD_BLOCK;
    }
    return rc;
}

int nanortc_datachannel_close(nanortc_t *rtc, uint16_t id)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nano_dc_channel_t *dc = NULL;
    for (uint8_t i = 0; i < rtc->datachannel.channel_count; i++) {
        if (rtc->datachannel.channels[i].stream_id == id &&
            rtc->datachannel.channels[i].state != NANORTC_DC_STATE_CLOSED) {
            dc = &rtc->datachannel.channels[i];
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
    cevt.datachannel_id.id = id;
    rtc_emit_event_full(rtc, &cevt);

    NANORTC_LOGI("RTC", "channel closed");
    return NANORTC_OK;
}

const char *nanortc_datachannel_get_label(nanortc_t *rtc, uint16_t id)
{
    if (!rtc) {
        return NULL;
    }
    for (uint8_t i = 0; i < rtc->datachannel.channel_count; i++) {
        if (rtc->datachannel.channels[i].stream_id == id &&
            rtc->datachannel.channels[i].state != NANORTC_DC_STATE_CLOSED) {
            return rtc->datachannel.channels[i].label;
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

/* ----------------------------------------------------------------
 * Trickle ICE + ICE Restart API
 * ---------------------------------------------------------------- */

int nanortc_end_of_candidates(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    rtc->ice.end_of_candidates = true;
    /* Also propagate from SDP state */
    rtc->sdp.end_of_candidates = true;
    NANORTC_LOGD("RTC", "end-of-candidates signaled");
    return NANORTC_OK;
}

int nanortc_ice_restart(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    NANORTC_LOGI("RTC", "ICE restart");

    /* Reset ICE state (preserves role + tie_breaker, bumps generation) */
    int rc = ice_restart(&rtc->ice);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* Generate new local ICE credentials */
    uint8_t ufrag_bytes[4];
    uint8_t pwd_bytes[11];
    if (rtc->config.crypto->random_bytes(ufrag_bytes, sizeof(ufrag_bytes)) != 0 ||
        rtc->config.crypto->random_bytes(pwd_bytes, sizeof(pwd_bytes)) != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    /* Hex-encode credentials */
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        rtc->ice.local_ufrag[i * 2] = hex[ufrag_bytes[i] >> 4];
        rtc->ice.local_ufrag[i * 2 + 1] = hex[ufrag_bytes[i] & 0x0F];
    }
    rtc->ice.local_ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';
    rtc->ice.local_ufrag_len = NANORTC_ICE_UFRAG_LEN;

    for (int i = 0; i < 11; i++) {
        rtc->ice.local_pwd[i * 2] = hex[pwd_bytes[i] >> 4];
        rtc->ice.local_pwd[i * 2 + 1] = hex[pwd_bytes[i] & 0x0F];
    }
    rtc->ice.local_pwd[NANORTC_ICE_PWD_LEN] = '\0';
    rtc->ice.local_pwd_len = NANORTC_ICE_PWD_LEN;

    /* Copy new credentials into SDP state */
    memcpy(rtc->sdp.local_ufrag, rtc->ice.local_ufrag, NANORTC_ICE_UFRAG_SIZE);
    memcpy(rtc->sdp.local_pwd, rtc->ice.local_pwd, NANORTC_ICE_PWD_SIZE);

    /* Reset SDP candidate state */
    rtc->sdp.candidate_count = 0;
    rtc->sdp.end_of_candidates = false;

    /* Reset connection state to allow re-negotiation */
    rtc->state = NANORTC_STATE_NEW;

    /* Emit ICE state change */
    nanortc_event_t ice_evt;
    memset(&ice_evt, 0, sizeof(ice_evt));
    ice_evt.type = NANORTC_EV_ICE_STATE_CHANGE;
    ice_evt.ice_state = (uint16_t)NANORTC_ICE_STATE_NEW;
    rtc_emit_event_full(rtc, &ice_evt);

    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * ICE Server Configuration (WebRTC-style)
 * ---------------------------------------------------------------- */

/* Parse "stun:host:port" or "turn:host:port" URL.
 * Returns 0=stun, 1=turn, -1=error. Writes host/port. */
static int parse_ice_url(const char *url, char *host, size_t host_size, uint16_t *port)
{
    int type = -1;
    const char *p = url;

    if (p[0] == 's' && p[1] == 't' && p[2] == 'u' && p[3] == 'n' && p[4] == ':') {
        type = 0;
        p += 5;
    } else if (p[0] == 't' && p[1] == 'u' && p[2] == 'r' && p[3] == 'n' && p[4] == ':') {
        type = 1;
        p += 5;
    } else {
        return -1;
    }

    /* Find last ':' for port separator */
    const char *colon = NULL;
    const char *end = p;
    while (*end) { /* NANORTC_SAFE: API boundary */
        if (*end == ':')
            colon = end;
        end++;
    }

    if (colon && colon > p) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_size)
            return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        /* Parse port */
        uint32_t pv = 0;
        const char *pp = colon + 1;
        while (*pp >= '0' && *pp <= '9') {
            pv = pv * 10 + (uint32_t)(*pp - '0');
            pp++;
        }
        *port = (uint16_t)pv;
    } else {
        /* No port — use default */
        size_t hlen = (size_t)(end - p);
        if (hlen >= host_size)
            return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = 3478;
    }
    return type;
}

static int rtc_apply_ice_servers(nanortc_t *rtc, const nanortc_ice_server_t *servers, size_t count)
{

    for (size_t i = 0; i < count; i++) {
        const nanortc_ice_server_t *s = &servers[i];
        if (!s->urls || s->url_count == 0) {
            continue;
        }

        for (size_t u = 0; u < s->url_count; u++) {
            const char *url = s->urls[u];
            if (!url) {
                continue;
            }

            char host[NANORTC_IPV6_STR_SIZE];
            uint16_t port = 3478;
            int type = parse_ice_url(url, host, sizeof(host), &port);

#if NANORTC_FEATURE_TURN
            if (type == 1 && !rtc->turn.configured) {
                /* First TURN URL wins (only one TURN server supported) */
                if (!s->username || !s->credential) {
                    return NANORTC_ERR_INVALID_PARAM;
                }

                size_t host_len = 0;
                while (host[host_len])
                    host_len++;

                uint8_t server_addr[NANORTC_ADDR_SIZE] = {0};
                uint8_t family = 0;
                int rc = addr_parse_auto(host, host_len, server_addr, &family);
                if (rc != NANORTC_OK) {
                    continue; /* skip unresolvable, try next URL */
                }

                size_t ulen = 0;
                while (s->username[ulen]) /* NANORTC_SAFE: API boundary */
                    ulen++;
                size_t plen = 0;
                while (s->credential[plen]) /* NANORTC_SAFE: API boundary */
                    plen++;

                rc = turn_configure(&rtc->turn, server_addr, family, port, s->username, ulen,
                                    s->credential, plen);
                if (rc == NANORTC_OK) {
                    NANORTC_LOGI("RTC", "TURN server configured");
                }
            }
#endif /* NANORTC_FEATURE_TURN */
            if (type == 0 && !rtc->stun_server_configured) {
                /* First STUN URL — configure for srflx discovery */
                size_t host_len = 0;
                while (host[host_len])
                    host_len++;

                uint8_t server_addr[NANORTC_ADDR_SIZE] = {0};
                uint8_t family = 0;
                int rc = addr_parse_auto(host, host_len, server_addr, &family);
                if (rc == NANORTC_OK) {
                    memcpy(rtc->stun_server_addr, server_addr, NANORTC_ADDR_SIZE);
                    rtc->stun_server_family = family;
                    rtc->stun_server_port = port;
                    rtc->stun_server_configured = true;
                    NANORTC_LOGI("RTC", "STUN server configured");
                }
            }
        }
    }

    return NANORTC_OK;
}

int nanortc_set_ice_servers(nanortc_t *rtc, const nanortc_ice_server_t *servers, size_t count)
{
    if (!rtc || (!servers && count > 0)) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    return rtc_apply_ice_servers(rtc, servers, count);
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

static int nanortc_add_track(nanortc_t *rtc, nanortc_track_kind_t kind,
                             nanortc_direction_t direction, nanortc_codec_t codec,
                             uint32_t sample_rate, uint8_t channels)
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
#if NANORTC_FEATURE_H265
        if (codec == NANORTC_CODEC_H265) {
            pt = NANORTC_VIDEO_H265_DEFAULT_PT; /* 98 */
        } else
#endif
        {
            pt = NANORTC_VIDEO_DEFAULT_PT; /* 96 */
        }
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

#if NANORTC_FEATURE_H265
/* Emit "<tag><base64(nal)>" into dst[*pos], advancing *pos. */
static int h265_sprop_emit(char *dst, size_t cap, size_t *pos, const char *tag, size_t tag_len,
                           const uint8_t *nal, size_t nal_len)
{
    if (*pos + tag_len > cap) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(dst + *pos, tag, tag_len);
    *pos += tag_len;

    size_t enc_len = 0;
    int rc = nano_base64_encode(nal, nal_len, dst + *pos, cap - *pos, &enc_len);
    if (rc != NANORTC_OK) {
        return rc;
    }
    *pos += enc_len;
    return NANORTC_OK;
}

int nanortc_video_set_h265_parameter_sets(nanortc_t *rtc, uint8_t mid, const uint8_t *vps,
                                          size_t vps_len, const uint8_t *sps, size_t sps_len,
                                          const uint8_t *pps, size_t pps_len)
{
    if (!rtc || !vps || vps_len < H265_NAL_HEADER_SIZE || !sps || sps_len < H265_NAL_HEADER_SIZE ||
        !pps || pps_len < H265_NAL_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m || m->kind != NANORTC_TRACK_VIDEO || m->codec != NANORTC_CODEC_H265) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nano_sdp_mline_t *ml = sdp_find_mline(&rtc->sdp, mid);
    if (!ml) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    char *dst = ml->h265_sprop_fmtp;
    const size_t cap = NANORTC_H265_SPROP_FMTP_SIZE;
    size_t pos = 0;

    int rc;
    if ((rc = h265_sprop_emit(dst, cap, &pos, "sprop-vps=", 10, vps, vps_len)) != NANORTC_OK ||
        (rc = h265_sprop_emit(dst, cap, &pos, ";sprop-sps=", 11, sps, sps_len)) != NANORTC_OK ||
        (rc = h265_sprop_emit(dst, cap, &pos, ";sprop-pps=", 11, pps, pps_len)) != NANORTC_OK) {
        return rc;
    }

    ml->h265_sprop_fmtp_len = (uint16_t)pos;
    NANORTC_LOGI("SDP", "H265 sprop-vps/sps/pps stored");
    return NANORTC_OK;
}
#endif /* NANORTC_FEATURE_H265 */

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

    return rtc_enqueue_transmit(rtc, m->media_buf, srtp_len, &rtc->remote_addr);
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
    uint16_t slot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
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

    /* Record NACK retransmission metadata for this slot.
     * rtp_pack() increments seq after writing, so the seq in the packet
     * is (m->rtp.seq - 1). */
    rtc->pkt_ring_meta[slot].seq = (uint16_t)(m->rtp.seq - 1);
    rtc->pkt_ring_meta[slot].len = (uint16_t)srtp_len;

    ctx->last_rc = rtc_enqueue_transmit(rtc, pkt_buf, srtp_len, &rtc->remote_addr);
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

/* ----------------------------------------------------------------
 * Media send API
 * ---------------------------------------------------------------- */

/** Convert millisecond PTS to RTP clock timestamp. */
static inline uint32_t pts_ms_to_rtp(uint32_t pts_ms, uint32_t clock_rate)
{
    return (uint32_t)((uint64_t)pts_ms * clock_rate / 1000);
}

int nanortc_send_audio(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms, const void *data, size_t len)
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

    uint32_t rtp_ts = pts_ms_to_rtp(pts_ms, m->sample_rate);
    return rtc_send_audio(rtc, m, rtp_ts, (const uint8_t *)data, len);
}

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_H265
/* RFC 7798 §4.4: h265_packetize_au greedy-packs Single NAL / AP / FU and
 * sets the RTP marker bit on the final callback, so is_last_nal stays 1. */
static int rtc_send_video_h265(nanortc_t *rtc, nanortc_track_t *m, uint32_t timestamp,
                               const uint8_t *buf, size_t len)
{
    h265_nal_ref_t nals[NANORTC_MAX_NALS_PER_AU];
    size_t n_nals = 0;
    size_t offset = 0;

    while (offset < len && n_nals < NANORTC_MAX_NALS_PER_AU) {
        size_t nal_len = 0;
        const uint8_t *nal = nano_annex_b_find_nal(buf, len, &offset, &nal_len);
        if (!nal || nal_len == 0) {
            break;
        }
        nals[n_nals].data = nal;
        nals[n_nals].len = nal_len;
        n_nals++;
    }

    if (n_nals == 0) {
        return NANORTC_OK;
    }

    video_send_ctx_t ctx;
    ctx.rtc = rtc;
    ctx.media = m;
    ctx.timestamp = timestamp;
    ctx.last_rc = NANORTC_OK;
    ctx.is_last_nal = 1; /* packetize_au drives the marker bit internally */

    int rc = h265_packetize_au(nals, n_nals, NANORTC_VIDEO_MTU, video_send_fragment_cb, &ctx);
    if (rc != NANORTC_OK) {
        return rc;
    }
    return ctx.last_rc;
}
#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_H265 */

#if NANORTC_FEATURE_VIDEO
int nanortc_send_video(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms, const void *data, size_t len)
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

    uint32_t ts = pts_ms_to_rtp(pts_ms, 90000);
    const uint8_t *buf = (const uint8_t *)data;

#if NANORTC_FEATURE_H265
    if (m->codec == NANORTC_CODEC_H265) {
        return rtc_send_video_h265(rtc, m, ts, buf, len);
    }
#endif

    /* H.264: scan per NAL, dispatch to h264_packetize. */
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

    return last_rc;
}
#endif /* NANORTC_FEATURE_VIDEO */

int nanortc_request_keyframe(nanortc_t *rtc, uint8_t mid)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
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

    /* SRTCP protect (RFC 3711 §3.4) */
    size_t srtcp_len = 0;
    int prc = nano_srtp_protect_rtcp(&rtc->srtp, pli_buf, pli_len, &srtcp_len);
    if (prc != NANORTC_OK) {
        return prc;
    }

    return rtc_enqueue_transmit(rtc, pli_buf, srtcp_len, &rtc->remote_addr);
}

/* ================================================================
 * Track statistics
 * ================================================================ */

int nanortc_get_track_stats(const nanortc_t *rtc, uint8_t mid, nanortc_track_stats_t *stats)
{
    if (!rtc || !stats) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    const nanortc_track_t *m = NULL;
    for (uint8_t i = 0; i < rtc->media_count; i++) {
        if (rtc->media[i].active && rtc->media[i].mid == mid) {
            m = &rtc->media[i];
            break;
        }
    }
    if (!m) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    memset(stats, 0, sizeof(*stats));
    stats->mid = mid;
    stats->packets_sent = m->rtcp.packets_sent;
    stats->octets_sent = m->rtcp.octets_sent;
    stats->packets_received = m->rtcp.packets_received;
    stats->packets_lost = m->rtcp.packets_lost;
    stats->jitter = m->rtcp.jitter;

    /* RTT from DLSR: if we have a last_sr_recv_ms and the peer has
     * sent us at least one SR, compute round-trip from DLSR.
     * For now, expose raw DLSR data — actual RTT requires knowing
     * the current time, which is only available during handle_input. */
    stats->rtt_ms = 0;
    if (m->rtcp.last_sr_recv_ms > 0 && rtc->now_ms > m->rtcp.last_sr_recv_ms) {
        stats->rtt_ms = rtc->now_ms - m->rtcp.last_sr_recv_ms;
    }

#if NANORTC_FEATURE_VIDEO
    stats->bitrate_bps = rtc->bwe.estimated_bitrate;
#endif

    return NANORTC_OK;
}

#if NANORTC_FEATURE_VIDEO
uint32_t nanortc_get_estimated_bitrate(const nanortc_t *rtc)
{
    if (!rtc) {
        return 0;
    }
    return bwe_get_bitrate(&rtc->bwe);
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
