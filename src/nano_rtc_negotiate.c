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
 * ICE_CANDIDATE_STR_SIZE macro at the storage site.
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
    /* RFC 8839 §5.1 / §9.1: retain mandatory related fields, but hide the
     * base/mapped address using the candidate's family and discard port. */
    if (type_len == 5 && (memcmp(type, "srflx", 5) == 0 || memcmp(type, "relay", 5) == 0)) {
        if (memchr(ip, ':', ip_len))
            pos = cand_append(buf, pos, " raddr :: rport 9", sizeof(" raddr :: rport 9") - 1);
        else
            pos = cand_append(buf, pos, " raddr 0.0.0.0 rport 9",
                              sizeof(" raddr 0.0.0.0 rport 9") - 1);
    }
    buf[pos] = '\0';
    return pos;
}

int nano_rtc_candidate_produce(nanortc_t *rtc, nanortc_output_t *out)
{
    const char *ip = NULL, *type = "host";
    char host_ip[NANORTC_IPV6_STR_SIZE];
    uint16_t port = 0, foundation = 0;
    uint32_t priority = 0;
    size_t type_len = 4;
    for (uint8_t i = 0; i < rtc->sdp.local_candidate_count; i++) {
        if (!rtc->host_candidate_pending[i])
            continue;
        const nano_sdp_candidate_t *candidate = &rtc->sdp.local_candidates[i];
        size_t ip_len;
        int rc = addr_format(candidate->addr, candidate->family, host_ip, sizeof(host_ip), &ip_len);
        if (rc != NANORTC_OK)
            return rc;
        rtc->host_candidate_pending[i] = false;
        ip = host_ip;
        port = rtc->sdp.local_candidates[i].port;
        foundation = (uint16_t)(i + 1);
        priority = candidate->priority;
        break;
    }
    if (!ip && rtc->srflx_candidate_pending) {
        rtc->srflx_candidate_pending = false;
        ip = rtc->sdp.srflx_candidate_ip;
        port = rtc->sdp.srflx_candidate_port;
        type = "srflx";
        type_len = 5;
        foundation = 3;
        priority = 1090519295;
    }
#if NANORTC_FEATURE_TURN
    if (!ip && rtc->relay_candidate_pending) {
        rtc->relay_candidate_pending = false;
        ip = rtc->sdp.relay_candidate_ip;
        port = rtc->sdp.relay_candidate_port;
        type = "relay";
        type_len = 5;
        foundation = 2;
        priority = 16777215;
    }
#endif
    if (!ip)
        return NANORTC_ERR_NO_DATA;
    nano_rtc_build_candidate_str(rtc->candidate_str, foundation, priority, ip,
                                 nanortc_strnlen(ip, NANORTC_IPV6_STR_SIZE), port, type, type_len);
    memset(out, 0, sizeof(*out));
    out->type = NANORTC_OUTPUT_EVENT;
    out->event.type = NANORTC_EV_ICE_CANDIDATE;
    out->event.ice_candidate.candidate_str = rtc->candidate_str;
    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * SDP / ICE credential helpers
 * ---------------------------------------------------------------- */

/* Generate random ufrag+pwd into SDP and ICE state. Hex-encoded so the
 * RFC 8839 candidate / RFC 8866 SDP serialisation can use the values
 * verbatim without further escaping. */
static int rtc_generate_ice_credentials(const nanortc_t *rtc, char ufrag[NANORTC_ICE_UFRAG_SIZE],
                                        char pwd[NANORTC_ICE_PWD_SIZE])
{
    if (!rtc->config.crypto || !rtc->config.crypto->random_bytes) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    uint8_t rnd[NANORTC_ICE_UFRAG_LEN / 2];
    uint8_t rnd2[NANORTC_ICE_PWD_LEN / 2];
    if (rtc->config.crypto->random_bytes(rnd, sizeof(rnd)) != 0 ||
        rtc->config.crypto->random_bytes(rnd2, sizeof(rnd2)) != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    for (int i = 0; i < (int)sizeof(rnd); i++) {
        ufrag[i * 2] = hex_chars[(rnd[i] >> 4) & 0xF];
        ufrag[i * 2 + 1] = hex_chars[rnd[i] & 0xF];
    }
    ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';

    for (int i = 0; i < (int)sizeof(rnd2); i++) {
        pwd[i * 2] = hex_chars[(rnd2[i] >> 4) & 0xF];
        pwd[i * 2 + 1] = hex_chars[rnd2[i] & 0xF];
    }
    pwd[NANORTC_ICE_PWD_LEN] = '\0';

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
    rtc->sctp.peer_max_message_size = rtc->sdp.remote_max_message_size;
#endif
}

/* A2: Auto-add ICE candidates embedded in SDP (RFC 8839) */
static int rtc_add_candidate(nano_ice_candidate_t *candidates, uint32_t *priorities,
                             nano_ice_related_t *related, uint8_t *count,
                             const nano_sdp_candidate_t *candidate)
{
    nano_ice_related_t detail = {0};
    memcpy(detail.addr, candidate->related_addr, sizeof(detail.addr));
    detail.port = candidate->related_port;
    detail.family = candidate->related_family;
    detail.present = candidate->has_related;
    for (uint8_t i = 0; i < *count; i++) {
        nano_ice_candidate_t *c = &candidates[i];
        if (c->family == candidate->family && c->port == candidate->port &&
            memcmp(c->addr, candidate->addr, NANORTC_ADDR_SIZE) == 0) {
            c->type = candidate->type;
            priorities[i] = candidate->priority;
            related[i] = detail;
            return NANORTC_OK;
        }
    }
    if (*count == NANORTC_MAX_ICE_CANDIDATES)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    priorities[*count] = candidate->priority;
    related[*count] = detail;
    nano_ice_candidate_t *c = &candidates[(*count)++];
    memset(c, 0, sizeof(*c));
    memcpy(c->addr, candidate->addr, sizeof(c->addr));
    c->family = candidate->family;
    c->port = candidate->port;
    c->type = candidate->type;
    return NANORTC_OK;
}

static int rtc_prepare_candidates(const nanortc_t *rtc, const nano_sdp_remote_t *remote,
                                  nano_ice_candidate_t candidates[NANORTC_MAX_ICE_CANDIDATES],
                                  uint32_t priorities[NANORTC_MAX_ICE_CANDIDATES],
                                  nano_ice_related_t related[NANORTC_MAX_ICE_CANDIDATES],
                                  uint8_t *count, bool restarting)
{
    *count = restarting ? 0 : rtc->ice.remote_candidate_count;
    memset(candidates, 0, sizeof(*candidates) * NANORTC_MAX_ICE_CANDIDATES);
    memcpy(candidates, rtc->ice.remote_candidates, sizeof(*candidates) * *count);
    memset(priorities, 0, sizeof(*priorities) * NANORTC_MAX_ICE_CANDIDATES);
    memcpy(priorities, rtc->ice.remote_priorities, sizeof(*priorities) * *count);
    memset(related, 0, sizeof(*related) * NANORTC_MAX_ICE_CANDIDATES);
    memcpy(related, rtc->remote_candidate_related, sizeof(*related) * *count);
    for (uint8_t i = 0; i < remote->candidate_count; i++) {
        int rc = rtc_add_candidate(candidates, priorities, related, count,
                                   &remote->remote_candidates[i]);
        if (rc != NANORTC_OK)
            return rc;
    }
    return NANORTC_OK;
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
            nanortc_direction_t local_dir = ml->direction;
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

        m->direction = ml->direction;

        if (ml->kind == SDP_MLINE_VIDEO && ml->pt != 0) {
            m->rtp.payload_type = ml->pt;
            NANORTC_LOGD(&rtc->config.log, "SDP", "video using negotiated PT");
        } else if (ml->remote_pt != 0) {
            m->rtp.payload_type = ml->remote_pt;
            ml->pt = ml->remote_pt;
            NANORTC_LOGD(&rtc->config.log, "SDP", "media using remote PT");
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

typedef struct {
    char ufrag[NANORTC_ICE_UFRAG_SIZE];
    char pwd[NANORTC_ICE_PWD_SIZE];
    char fingerprint[NANORTC_SDP_FINGERPRINT_SIZE];
    nano_sdp_setup_t setup;
} rtc_local_description_t;

static void rtc_local_description_get(const nano_sdp_t *sdp, rtc_local_description_t *local)
{
    memcpy(local->ufrag, sdp->local_ufrag, sizeof(local->ufrag));
    memcpy(local->pwd, sdp->local_pwd, sizeof(local->pwd));
    memcpy(local->fingerprint, sdp->local_fingerprint, sizeof(local->fingerprint));
    local->setup = sdp->local_setup;
}

static void rtc_local_description_set(nano_sdp_t *sdp, const rtc_local_description_t *local)
{
    memcpy(sdp->local_ufrag, local->ufrag, sizeof(local->ufrag));
    memcpy(sdp->local_pwd, local->pwd, sizeof(local->pwd));
    memcpy(sdp->local_fingerprint, local->fingerprint, sizeof(local->fingerprint));
    sdp->local_setup = local->setup;
}

/* Pure rendering borrows local storage for a compact remote view; no callbacks run. */
static int rtc_render_description(nano_sdp_t *sdp, const nano_sdp_remote_t *remote,
                                  const rtc_local_description_t *local, char *buf, size_t cap,
                                  size_t *len)
{
    if (cap < NANORTC_SDP_MIN_BUF_SIZE)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    nano_sdp_remote_t previous;
    rtc_local_description_t previous_local;
    sdp_snapshot_remote(sdp, &previous);
    rtc_local_description_get(sdp, &previous_local);
    if (remote)
        sdp_commit_remote(sdp, remote);
    rtc_local_description_set(sdp, local);
    int rc = sdp_generate_answer(sdp, buf, cap, len);
    sdp_commit_remote(sdp, &previous);
    rtc_local_description_set(sdp, &previous_local);
    return rc;
}

static int rtc_prepare_local_description(nanortc_t *rtc, rtc_local_description_t *local,
                                         nano_sdp_setup_t setup)
{
    rtc_local_description_get(&rtc->sdp, local);
    local->setup = setup;
    if (local->ufrag[0] == '\0') {
        int rc = rtc_generate_ice_credentials(rtc, local->ufrag, local->pwd);
        if (rc != NANORTC_OK)
            return rc;
    }
    if (local->fingerprint[0] == '\0') {
        /* Fixed-size placeholder preflights the answer before allocating DTLS. */
        memcpy(local->fingerprint, "sha-256 ", 8);
        for (size_t i = 0; i < NANORTC_DTLS_FINGERPRINT_SIZE; i++) {
            local->fingerprint[8u + i * 3u] = '0';
            local->fingerprint[9u + i * 3u] = '0';
            local->fingerprint[10u + i * 3u] = ':';
        }
        local->fingerprint[8u + NANORTC_DTLS_FINGERPRINT_SIZE * 3u - 1u] = '\0';
    }
    return NANORTC_OK;
}

static void rtc_commit_local_description(nanortc_t *rtc, const rtc_local_description_t *local)
{
    rtc_local_description_set(&rtc->sdp, local);
    memcpy(rtc->ice.local_ufrag, local->ufrag, sizeof(local->ufrag));
    memcpy(rtc->ice.local_pwd, local->pwd, sizeof(local->pwd));
    rtc->ice.local_ufrag_len = nanortc_strnlen(local->ufrag, sizeof(local->ufrag));
    rtc->ice.local_pwd_len = nanortc_strnlen(local->pwd, sizeof(local->pwd));
}

static int rtc_validate_remote(const nanortc_t *rtc, nano_sdp_remote_t *remote, bool existing,
                               bool answer)
{
    if (!remote->has_remote_fingerprint)
        return NANORTC_ERR_PARSE;
    if (answer && remote->remote_setup == NANORTC_SDP_SETUP_ACTPASS)
        return NANORTC_ERR_PARSE;
    /* RFC 8842 §6: preserve the DTLS association through a pure ICE restart. */
    if (existing && memcmp(remote->remote_fingerprint, rtc->sdp.remote_fingerprint,
                           NANORTC_DTLS_FINGERPRINT_SIZE) != 0)
        return NANORTC_ERR_CRYPTO;
    if ((existing || answer) &&
        (remote->has_datachannel != rtc->sdp.has_datachannel ||
         remote->mid_count != rtc->sdp.mid_count || remote->dc_mid != rtc->sdp.dc_mid))
        return NANORTC_ERR_NOT_IMPLEMENTED;
    if (existing && (remote->remote_sctp_port != rtc->sdp.remote_sctp_port ||
                     remote->remote_max_message_size != rtc->sdp.remote_max_message_size))
        return NANORTC_ERR_NOT_IMPLEMENTED;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    if ((existing || answer) && remote->mline_count != rtc->sdp.mline_count)
        return NANORTC_ERR_NOT_IMPLEMENTED;
    for (uint8_t i = 0; i < remote->mline_count; i++) {
        nano_sdp_remote_mline_t *r = &remote->mlines[i];
        if (r->kind == SDP_MLINE_AUDIO)
            r->pt = r->remote_pt;
        if (r->kind == SDP_MLINE_AUDIO &&
            (r->codec == NANORTC_CODEC_NONE ||
             (r->codec == NANORTC_CODEC_OPUS && (r->sample_rate != 48000 || r->channels != 2)) ||
             (r->codec != NANORTC_CODEC_OPUS && (r->sample_rate != 8000 || r->channels != 1))))
            return NANORTC_ERR_NOT_IMPLEMENTED;
        if (r->kind == SDP_MLINE_VIDEO &&
            ((r->codec == NANORTC_CODEC_H264 && !r->video_h264_rtpmap_pt)
#if NANORTC_FEATURE_H265
             || (r->codec == NANORTC_CODEC_H265 && !r->video_h265_rtpmap_pt)
#endif
                 ))
            return NANORTC_ERR_NOT_IMPLEMENTED;
        if (i < rtc->sdp.mline_count && r->codec != rtc->sdp.mlines[i].codec)
            return NANORTC_ERR_NOT_IMPLEMENTED;
        if (existing || answer) {
            const nano_sdp_mline_t *old = &rtc->sdp.mlines[i];
            if (r->mid != old->mid || r->kind != old->kind || r->codec != old->codec ||
                (existing &&
                 (r->remote_direction != old->remote_direction || r->remote_pt != old->remote_pt ||
                  r->pt != old->pt || r->sample_rate != old->sample_rate ||
                  r->channels != old->channels || r->twcc_ext_id != old->twcc_ext_id)))
                return NANORTC_ERR_NOT_IMPLEMENTED;
        }
        nanortc_direction_t desired = r->direction;
        nanortc_direction_t offered = direction_complement(r->remote_direction);
        bool send = (desired == NANORTC_DIR_SENDONLY || desired == NANORTC_DIR_SENDRECV) &&
                    (offered == NANORTC_DIR_SENDONLY || offered == NANORTC_DIR_SENDRECV);
        bool recv = (desired == NANORTC_DIR_RECVONLY || desired == NANORTC_DIR_SENDRECV) &&
                    (offered == NANORTC_DIR_RECVONLY || offered == NANORTC_DIR_SENDRECV);
        r->direction = send ? (recv ? NANORTC_DIR_SENDRECV : NANORTC_DIR_SENDONLY)
                            : (recv ? NANORTC_DIR_RECVONLY : NANORTC_DIR_INACTIVE);
    }
#endif
    return NANORTC_OK;
}

static int rtc_prepare_dtls(nanortc_t *rtc, rtc_local_description_t *local)
{
    if (!rtc->dtls.crypto_ctx) {
        int rc =
            dtls_init(&rtc->dtls, rtc->config.crypto, local->setup == NANORTC_SDP_SETUP_PASSIVE);
        if (rc != NANORTC_OK)
            return rc;
    }
    memcpy(local->fingerprint, "sha-256 ", 8);
    memcpy(local->fingerprint + 8, rtc->dtls.local_fingerprint, NANORTC_DTLS_FINGERPRINT_MIN_BUF);
    return NANORTC_OK;
}

static void rtc_commit_remote(nanortc_t *rtc, const nano_sdp_remote_t *remote,
                              const nano_ice_candidate_t candidates[NANORTC_MAX_ICE_CANDIDATES],
                              const uint32_t priorities[NANORTC_MAX_ICE_CANDIDATES],
                              const nano_ice_related_t related[NANORTC_MAX_ICE_CANDIDATES],
                              uint8_t count)
{
    sdp_commit_remote(&rtc->sdp, remote);
    rtc_apply_remote_sdp(rtc);
    memcpy(rtc->ice.remote_candidates, candidates, sizeof(rtc->ice.remote_candidates));
    memcpy(rtc->ice.remote_priorities, priorities, sizeof(rtc->ice.remote_priorities));
    memcpy(rtc->remote_candidate_related, related, sizeof(rtc->remote_candidate_related));
    rtc->ice.remote_candidate_count = count;
    rtc->ice.end_of_candidates = remote->end_of_candidates;
    if (rtc->dtls.state == NANORTC_DTLS_STATE_INIT)
        (void)dtls_set_peer_fingerprint(&rtc->dtls, remote->remote_fingerprint);
    if (!rtc->ice_restarting)
        rtc_apply_negotiated_media(rtc);
    rtc->signaling_state = NANORTC_SIGNALING_NEGOTIATED;
}

int nanortc_accept_offer(nanortc_t *rtc, const char *offer, size_t offer_len, char *answer_buf,
                         size_t answer_buf_len, size_t *out_len)
{
    if (!rtc || !offer || !offer_len || !answer_buf)
        return NANORTC_ERR_INVALID_PARAM;
    if (rtc->state == NANORTC_STATE_CLOSED ||
        rtc->signaling_state == NANORTC_SIGNALING_HAVE_LOCAL_OFFER)
        return NANORTC_ERR_STATE;
    bool existing = rtc->signaling_state == NANORTC_SIGNALING_NEGOTIATED || rtc->ice_restarting;
    nano_sdp_remote_t remote;
    int rc = sdp_parse_remote(&rtc->sdp, offer, offer_len, &remote);
    if (rc != NANORTC_OK)
        return rc;
    rc = rtc_validate_remote(rtc, &remote, existing, false);
    if (rc != NANORTC_OK)
        return rc;
    if (existing && !rtc->ice_restarting &&
        memcmp(remote.remote_ufrag, rtc->sdp.remote_ufrag, sizeof(remote.remote_ufrag)) == 0 &&
        memcmp(remote.remote_pwd, rtc->sdp.remote_pwd, sizeof(remote.remote_pwd)) == 0)
        return NANORTC_ERR_STATE;
    nano_ice_candidate_t candidates[NANORTC_MAX_ICE_CANDIDATES];
    uint32_t priorities[NANORTC_MAX_ICE_CANDIDATES];
    nano_ice_related_t related[NANORTC_MAX_ICE_CANDIDATES];
    uint8_t count;
    rc = rtc_prepare_candidates(rtc, &remote, candidates, priorities, related, &count, existing);
    if (rc != NANORTC_OK)
        return rc;
    rtc_local_description_t local;
    rc = rtc_prepare_local_description(rtc, &local,
                                       remote.remote_setup == NANORTC_SDP_SETUP_ACTIVE
                                           ? NANORTC_SDP_SETUP_PASSIVE
                                           : NANORTC_SDP_SETUP_ACTIVE);
    /* An actpass offer retains NanoRTC's conventional passive answer. */
    if (remote.remote_setup == NANORTC_SDP_SETUP_ACTPASS)
        local.setup = NANORTC_SDP_SETUP_PASSIVE;
    if (rc != NANORTC_OK)
        return rc;
    if (existing) {
        nano_sdp_setup_t established =
            rtc->dtls.is_server ? NANORTC_SDP_SETUP_PASSIVE : NANORTC_SDP_SETUP_ACTIVE;
        if (remote.remote_setup != NANORTC_SDP_SETUP_ACTPASS && local.setup != established)
            return NANORTC_ERR_NOT_IMPLEMENTED;
        local.setup = established;
    }
    size_t produced = 0;
    rc = rtc_render_description(&rtc->sdp, &remote, &local, answer_buf, answer_buf_len, &produced);
    if (rc != NANORTC_OK)
        return rc;
    if (existing && !rtc->ice_restarting) {
        rc = nanortc_ice_restart(rtc);
        if (rc != NANORTC_OK)
            return rc;
        memcpy(local.ufrag, rtc->sdp.local_ufrag, sizeof(local.ufrag));
        memcpy(local.pwd, rtc->sdp.local_pwd, sizeof(local.pwd));
    }
    rc = rtc_prepare_dtls(rtc, &local);
    if (rc != NANORTC_OK)
        return rc;
    rc = rtc_render_description(&rtc->sdp, &remote, &local, answer_buf, answer_buf_len, &produced);
    if (rc != NANORTC_OK)
        return rc;
    rtc_commit_local_description(rtc, &local);
    rtc_commit_remote(rtc, &remote, candidates, priorities, related, count);
    if (out_len)
        *out_len = produced;
    return NANORTC_OK;
}

int nanortc_create_offer(nanortc_t *rtc, char *offer_buf, size_t offer_buf_len, size_t *out_len)
{
    if (!rtc || !offer_buf)
        return NANORTC_ERR_INVALID_PARAM;
    if (rtc->state == NANORTC_STATE_CLOSED || rtc->signaling_state != NANORTC_SIGNALING_READY)
        return NANORTC_ERR_STATE;
    rtc_local_description_t local;
    int rc = rtc_prepare_local_description(rtc, &local, NANORTC_SDP_SETUP_ACTPASS);
    if (rc != NANORTC_OK)
        return rc;
    size_t produced = 0;
    rc = rtc_render_description(&rtc->sdp, NULL, &local, offer_buf, offer_buf_len, &produced);
    if (rc != NANORTC_OK)
        return rc;
    rc = rtc_prepare_dtls(rtc, &local);
    if (rc != NANORTC_OK)
        return rc;
    rc = rtc_render_description(&rtc->sdp, NULL, &local, offer_buf, offer_buf_len, &produced);
    if (rc != NANORTC_OK)
        return rc;
    rtc_commit_local_description(rtc, &local);
    rtc->signaling_state = NANORTC_SIGNALING_HAVE_LOCAL_OFFER;
    if (out_len)
        *out_len = produced;
    return NANORTC_OK;
}

int nanortc_accept_answer(nanortc_t *rtc, const char *answer, size_t answer_len)
{
    if (!rtc || !answer || !answer_len)
        return NANORTC_ERR_INVALID_PARAM;
    if (rtc->state == NANORTC_STATE_CLOSED ||
        rtc->signaling_state != NANORTC_SIGNALING_HAVE_LOCAL_OFFER)
        return NANORTC_ERR_STATE;
    nano_sdp_remote_t remote;
    int rc = sdp_parse_remote(&rtc->sdp, answer, answer_len, &remote);
    if (rc != NANORTC_OK)
        return rc;
    rc = rtc_validate_remote(rtc, &remote, rtc->ice_restarting, true);
    if (rc != NANORTC_OK)
        return rc;
    nano_ice_candidate_t candidates[NANORTC_MAX_ICE_CANDIDATES];
    uint32_t priorities[NANORTC_MAX_ICE_CANDIDATES];
    nano_ice_related_t related[NANORTC_MAX_ICE_CANDIDATES];
    uint8_t count;
    rc = rtc_prepare_candidates(rtc, &remote, candidates, priorities, related, &count,
                                rtc->ice_restarting);
    if (rc != NANORTC_OK)
        return rc;
    int is_server = remote.remote_setup == NANORTC_SDP_SETUP_ACTIVE;
    if (rtc->dtls.is_server != is_server) {
        if (rtc->ice_restarting || !rtc->config.crypto->dtls_set_role)
            return NANORTC_ERR_NOT_IMPLEMENTED;
        if (rtc->config.crypto->dtls_set_role(rtc->dtls.crypto_ctx, is_server) != 0)
            return NANORTC_ERR_CRYPTO;
        rtc->dtls.is_server = is_server;
    }
    rtc->sdp.local_setup = is_server ? NANORTC_SDP_SETUP_PASSIVE : NANORTC_SDP_SETUP_ACTIVE;
    rtc_commit_remote(rtc, &remote, candidates, priorities, related, count);
    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * Public API: ICE candidates (trickle ICE)
 * ---------------------------------------------------------------- */

int nanortc_add_local_candidate(nanortc_t *rtc, const char *ip, uint16_t port)
{
    if (!rtc || !ip || !port) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (rtc->sdp.local_candidate_count >= NANORTC_MAX_LOCAL_CANDIDATES) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    uint8_t idx = rtc->sdp.local_candidate_count;
    size_t ip_len = strlen(ip); /* NANORTC_SAFE: API boundary */
    if (ip_len >= NANORTC_IPV6_STR_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    uint8_t parsed_addr[NANORTC_ADDR_SIZE];
    memset(parsed_addr, 0, sizeof(parsed_addr));
    uint8_t family = 0;
    int rc = addr_parse_auto(ip, ip_len, parsed_addr, &family);
    if (rc != NANORTC_OK) {
        return rc;
    }
    memcpy(rtc->sdp.local_candidates[idx].addr, parsed_addr, NANORTC_ADDR_SIZE);
    rtc->sdp.local_candidates[idx].family = family;
    rtc->sdp.local_candidates[idx].type = NANORTC_ICE_CAND_HOST;
    rtc->sdp.local_candidates[idx].priority = ICE_HOST_PRIORITY(idx);
    rtc->sdp.local_candidates[idx].port = port;
    rtc->ice.local_candidates[idx].family = family;
    memcpy(rtc->ice.local_candidates[idx].addr, parsed_addr, NANORTC_ADDR_SIZE);
    rtc->ice.local_candidates[idx].port = port;
    rtc->ice.local_candidates[idx].type = NANORTC_ICE_CAND_HOST;

    rtc->sdp.local_candidate_count = idx + 1;
    rtc->ice.local_candidate_count = idx + 1;

    rtc->host_candidate_pending[idx] = true;

    NANORTC_LOGI(&rtc->config.log, "RTC", "local candidate added");
    return NANORTC_OK;
}

int nanortc_add_remote_candidate(nanortc_t *rtc, const char *text, size_t len)
{
    if (!rtc || !text)
        return NANORTC_ERR_INVALID_PARAM;
    if (rtc->state == NANORTC_STATE_CLOSED || rtc->ice.end_of_candidates)
        return NANORTC_ERR_STATE;
    nano_sdp_candidate_t candidate;
    int rc = sdp_parse_candidate(text, len, &candidate);
    if (rc != NANORTC_OK)
        return rc;
    return rtc_add_candidate(rtc->ice.remote_candidates, rtc->ice.remote_priorities,
                             rtc->remote_candidate_related, &rtc->ice.remote_candidate_count,
                             &candidate);
}

int nanortc_end_of_candidates(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    rtc->ice.end_of_candidates = true;
    rtc->sdp.end_of_candidates = true;
    NANORTC_LOGD(&rtc->config.log, "RTC", "end-of-candidates signaled");
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
                    NANORTC_LOGI(&rtc->config.log, "RTC", "TURN server configured");
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
                    NANORTC_LOGI(&rtc->config.log, "RTC", "STUN server configured");
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
