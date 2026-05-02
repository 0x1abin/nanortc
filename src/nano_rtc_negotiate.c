/*
 * nanortc — Offer / answer + ICE candidate / iceServers entry surface.
 *
 * Split off from nano_rtc.c (Phase 10 PR-4) to keep the receive/timer
 * backbone of the orchestration layer separately reviewable. The split
 * is deliberately shallow: only the SDP-driven negotiation entry points
 * and the trickle-ICE candidate/string helpers move here. The hot path
 * (`rtc_process_receive`, `rtc_process_timers`, output queue helpers,
 * media send paths) remains in nano_rtc.c because it shares state — the
 * out_queue, TURN wrap meta, SRTP session — that splitting would either
 * fragment or force into widely-exposed internal helpers. See the four
 * function declarations in nano_rtc_internal.h for the actual
 * cross-file contract.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_rtc_internal.h"
#include "nano_addr.h"
#include "nano_dtls.h"
#include "nano_ice.h"
#include "nano_log.h"
#include "nano_sdp.h"
#include "nano_stun.h"
#include "nanortc_util.h"

#if NANORTC_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_media.h"
#include "nano_rtp.h"
#endif

#if NANORTC_FEATURE_TURN
#include "nano_turn.h"
#endif

#include <string.h>

/* Shared hex alphabet for ICE credential generation. Local copy keeps the
 * symbol scoped to this translation unit; nanortc_ice_restart() in
 * nano_rtc.c carries its own hex[] for the same reason. */
static const char hex_chars[] = "0123456789abcdef";

/* ----------------------------------------------------------------
 * Candidate string builder utilities
 *
 * These produce ASCII SDP candidate lines (RFC 8839 §5.1) without any
 * heap allocation or unbounded format calls — the safe-C policy bans
 * the entire `*printf`/`*scanf` family in `src/`, and the caller-
 * provided buffer carries its own size guarantee through the
 * NANORTC_HOST_CAND_STR_SIZE-style macros at the storage site.
 * ---------------------------------------------------------------- */

/** Append a base-10 unsigned integer to buf at *pos. Returns new pos. */
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

size_t nano_rtc_build_candidate_str(char *buf, uint16_t foundation, uint32_t priority,
                                    const char *ip, size_t ip_len, uint16_t port, const char *type,
                                    size_t type_len)
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

void nano_rtc_emit_ice_candidate(nanortc_t *rtc, const char *candidate_str)
{
    nanortc_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = NANORTC_EV_ICE_CANDIDATE;
    evt.ice_candidate.candidate_str = candidate_str;
    evt.ice_candidate.end_of_candidates = false;
    nano_rtc_emit_event_full(rtc, &evt);
}

/* ----------------------------------------------------------------
 * SDP / ICE credential helpers
 * ---------------------------------------------------------------- */

/* Generate random ufrag+pwd into SDP and ICE state. Hex-encoded so the
 * RFC 8839 candidate / RFC 8866 SDP serialisation can use the values
 * verbatim without further escaping. */
static int rtc_generate_ice_credentials(nanortc_t *rtc)
{
    if (!rtc->config.crypto) {
        return NANORTC_OK;
    }

    uint8_t rnd[NANORTC_ICE_UFRAG_LEN / 2];
    rtc->config.crypto->random_bytes(rnd, sizeof(rnd));
    for (int i = 0; i < (int)sizeof(rnd); i++) {
        rtc->sdp.local_ufrag[i * 2] = hex_chars[(rnd[i] >> 4) & 0xF];
        rtc->sdp.local_ufrag[i * 2 + 1] = hex_chars[rnd[i] & 0xF];
    }
    rtc->sdp.local_ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';

    uint8_t rnd2[NANORTC_ICE_PWD_LEN / 2];
    rtc->config.crypto->random_bytes(rnd2, sizeof(rnd2));
    for (int i = 0; i < (int)sizeof(rnd2); i++) {
        rtc->sdp.local_pwd[i * 2] = hex_chars[(rnd2[i] >> 4) & 0xF];
        rtc->sdp.local_pwd[i * 2 + 1] = hex_chars[rnd2[i] & 0xF];
    }
    rtc->sdp.local_pwd[NANORTC_ICE_PWD_LEN] = '\0';

    memcpy(rtc->ice.local_ufrag, rtc->sdp.local_ufrag, sizeof(rtc->ice.local_ufrag));
    memcpy(rtc->ice.local_pwd, rtc->sdp.local_pwd, sizeof(rtc->ice.local_pwd));
    rtc->ice.local_ufrag_len = NANORTC_ICE_UFRAG_LEN;
    rtc->ice.local_pwd_len = NANORTC_ICE_PWD_LEN;

    return NANORTC_OK;
}

/* A5: Apply remote ICE/SCTP credentials from parsed SDP to subsystem state */
static void rtc_apply_remote_sdp(nanortc_t *rtc)
{
    memcpy(rtc->ice.remote_ufrag, rtc->sdp.remote_ufrag, sizeof(rtc->ice.remote_ufrag));
    memcpy(rtc->ice.remote_pwd, rtc->sdp.remote_pwd, sizeof(rtc->ice.remote_pwd));
    rtc->ice.remote_ufrag_len =
        nanortc_strnlen(rtc->sdp.remote_ufrag, sizeof(rtc->sdp.remote_ufrag));
    rtc->ice.remote_pwd_len = nanortc_strnlen(rtc->sdp.remote_pwd, sizeof(rtc->sdp.remote_pwd));

#if NANORTC_FEATURE_DATACHANNEL
    if (rtc->sdp.remote_sctp_port > 0) {
        rtc->sctp.remote_port = rtc->sdp.remote_sctp_port;
    }
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
        if (addr_len + 8 < sizeof(cand_str)) {
            memcpy(cand_str, c->addr, addr_len);
            cand_str[addr_len] = ' ';
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
    if (rtc->sdp.end_of_candidates) {
        rtc->ice.end_of_candidates = true;
    }
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

            nanortc_event_t maevt;
            memset(&maevt, 0, sizeof(maevt));
            maevt.type = NANORTC_EV_MEDIA_ADDED;
            maevt.media_added.mid = mid;
            maevt.media_added.kind = (uint8_t)kind;
            maevt.media_added.direction = local_dir;
            nano_rtc_emit_event_full(rtc, &maevt);
        }

        if (ml->remote_direction != NANORTC_DIR_SENDRECV || m->direction == NANORTC_DIR_SENDRECV) {
            m->direction = direction_complement(ml->remote_direction);
        }
        ml->direction = m->direction;

        if (ml->kind == SDP_MLINE_VIDEO && ml->pt != 0) {
            m->rtp.payload_type = ml->pt;
            NANORTC_LOGD("SDP", "video using negotiated PT");
        } else if (ml->remote_pt != 0) {
            m->rtp.payload_type = ml->remote_pt;
            ml->pt = ml->remote_pt;
            NANORTC_LOGD("SDP", "media using remote PT");
        }

#if NANORTC_FEATURE_VIDEO
        /* Apply negotiated TWCC header extension ID. Video only for now;
         * audio TWCC can be enabled later once the BWE consumer learns
         * to distinguish audio vs video streams. */
        if (ml->kind == SDP_MLINE_VIDEO) {
            m->rtp.twcc_ext_id = ml->twcc_ext_id;
        }
#endif
    }
#endif
    (void)rtc;
}

void nano_rtc_cache_fingerprint(nanortc_t *rtc)
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

/* ----------------------------------------------------------------
 * Public API: offer / answer
 * ---------------------------------------------------------------- */

int nanortc_accept_offer(nanortc_t *rtc, const char *offer, char *answer_buf, size_t answer_buf_len,
                         size_t *out_len)
{
    if (!rtc || !offer || !answer_buf) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    size_t offer_len = strlen(offer); /* NANORTC_SAFE: API boundary */

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
        nano_rtc_cache_fingerprint(rtc);
    }

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
        nano_rtc_cache_fingerprint(rtc);
    }

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

    if (rtc->state != NANORTC_STATE_NEW) {
        return NANORTC_ERR_STATE;
    }

    size_t answer_len = strlen(answer); /* NANORTC_SAFE: API boundary */

    int rc = sdp_parse(&rtc->sdp, answer, answer_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    rtc_apply_remote_sdp(rtc);

    /* Determine DTLS role from answer's setup attribute. */
    if (rtc->sdp.remote_setup == NANORTC_SDP_SETUP_PASSIVE) {
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    } else if (rtc->sdp.remote_setup == NANORTC_SDP_SETUP_ACTIVE) {
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_PASSIVE;
    } else {
        /* Answerer returned actpass — invalid per RFC 8842 §5.2.
         * Both sides would become server. Default to active. */
        NANORTC_LOGW("RTC", "remote answer has setup:actpass, forcing local active");
        rtc->sdp.local_setup = NANORTC_SDP_SETUP_ACTIVE;
    }

    /* Finalize DTLS role based on negotiated SDP setup attribute. */
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

/* ----------------------------------------------------------------
 * Public API: ICE candidates (trickle ICE)
 * ---------------------------------------------------------------- */

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

    memcpy(rtc->sdp.local_candidates[idx].addr, ip, ip_len + 1);
    rtc->sdp.local_candidates[idx].port = port;

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

    nano_rtc_build_candidate_str(rtc->host_cand_str, (uint16_t)(idx + 1), ICE_HOST_PRIORITY(idx),
                                 ip, ip_len, port, "host", 4);
    nano_rtc_emit_ice_candidate(rtc, rtc->host_cand_str);

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
        addr_str = p;
        while (*p && *p != ' ')
            p++;
        addr_len = (size_t)(p - addr_str);

        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (uint16_t)(*p - '0');
            p++;
        }

        /* Optional "typ <type>" attribute (RFC 8839 §5.1). The string may
         * carry "raddr/rport" and other extensions after the type — we
         * only need the type token itself. F5: previously unparsed, every
         * remote candidate stayed at type=HOST. */
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

    uint8_t parsed_addr[NANORTC_ADDR_SIZE] = {0};
    uint8_t family = 0;
    int rc = addr_parse_auto(addr_str, addr_len, parsed_addr, &family);
    if (rc != 0) {
        return rc;
    }

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

int nanortc_end_of_candidates(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    rtc->ice.end_of_candidates = true;
    rtc->sdp.end_of_candidates = true;
    NANORTC_LOGD("RTC", "end-of-candidates signaled");
    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * Public API: iceServers configuration (WebRTC-style)
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
        uint32_t pv = 0;
        const char *pp = colon + 1;
        while (*pp >= '0' && *pp <= '9') {
            pv = pv * 10 + (uint32_t)(*pp - '0');
            pp++;
        }
        *port = (uint16_t)pv;
    } else {
        size_t hlen = (size_t)(end - p);
        if (hlen >= host_size)
            return -1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = 3478;
    }
    return type;
}

int nano_rtc_apply_ice_servers(nanortc_t *rtc, const nanortc_ice_server_t *servers, size_t count)
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
    return nano_rtc_apply_ice_servers(rtc, servers, count);
}
