/*
 * nanortc — Main state machine
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_rtc_internal.h"
#include "nano_ice.h"
#include "nano_stun.h"
#include "nano_sdp.h"
#include "nano_addr.h"
#include "nano_log.h"
#include "nano_time.h"
#include "nanortc_util.h"

#if NANORTC_FEATURE_DATACHANNEL
#include "nano_sctp.h"
#include "nano_datachannel.h"
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_media.h"
#include "nano_rtp.h"
#include "nano_srtp.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_bwe.h"
#endif

#include <string.h>

bool nano_rtc_peer_can_send(const nanortc_t *rtc)
{
    return !rtc->ice_restarting || (rtc->ice.consent_expiry_ms != 0u &&
                                    !nano_time_is_due(rtc->now_ms, rtc->ice.consent_expiry_ms));
}

static bool rtc_is_peer_payload(const uint8_t *data, size_t len)
{
    return data && len &&
           ((data[0] >= 20u && data[0] <= 63u) || (data[0] >= 128u && data[0] <= 191u));
}

static void rtc_close(nanortc_t *rtc, nanortc_disconnect_reason_t reason)
{
    if (rtc->state == NANORTC_STATE_CLOSED)
        return;
    rtc->state = NANORTC_STATE_CLOSED;
    rtc->transmit_pending = false;
    rtc->out_head = rtc->out_tail;
    rtc->tx_slots_in_use = 0;
    rtc->pending_event_count = 0;
    memset(rtc->host_candidate_pending, 0, sizeof(rtc->host_candidate_pending));
    rtc->srflx_candidate_pending = false;
#if NANORTC_FEATURE_TURN
    rtc->relay_candidate_pending = false;
#endif
#if NANORTC_FEATURE_VIDEO
    rtc->nack_retx_in_use = 0;
#if NANORTC_FEATURE_VIDEO_PACING
    rtc->pacer.head = rtc->pacer.tail;
#endif
#if NANORTC_FEATURE_VIDEO_FEC
    rtc->fec_tx_in_use = 0;
#endif
#endif
    nanortc_event_t event;
    memset(&event, 0, sizeof(event));
    if (reason == NANORTC_DISCONNECT_ICE_FAILED || reason == NANORTC_DISCONNECT_CONSENT_EXPIRED) {
        event.type = NANORTC_EV_ICE_STATE_CHANGE;
        event.ice_state = (uint16_t)rtc->ice.state;
        (void)nano_rtc_emit_event_full(rtc, &event);
    }
    memset(&event, 0, sizeof(event));
    event.type = NANORTC_EV_DISCONNECTED;
    event.disconnect_reason = reason;
    (void)nano_rtc_emit_event_full(rtc, &event);
}

#if NANORTC_HAVE_MEDIA_TRANSPORT
/* Only the authenticated-but-unencrypted RTP header is inspected here. */
static void rtc_transmit_metadata(nanortc_t *rtc, nanortc_output_t *out)
{
    const uint8_t *data = out->transmit.data;
    size_t len = out->transmit.len;
    if (!data || len < RTP_HEADER_SIZE + NANORTC_SRTP_AUTH_TAG_SIZE || (data[0] >> 6) != 2 ||
        (data[1] >= 192 && data[1] <= 223))
        return;
    uint32_t ssrc = nanortc_read_u32be(data + 8);
    for (uint8_t i = 0; i < rtc->media_count; i++) {
        const nanortc_track_t *m = &rtc->media[i];
        if (!m->active)
            continue;
        uint8_t flags = NANO_TX_RTP;
        if (ssrc != m->rtp.ssrc) {
#if NANORTC_FEATURE_VIDEO_FEC
            if (m->kind != NANORTC_TRACK_VIDEO || ssrc != m->rtp.ssrc + 1u ||
                (data[1] & 0x7fu) != NANORTC_VIDEO_FEC_PT)
                continue;
            flags = NANO_TX_FEC;
#else
            continue;
#endif
        }
        size_t header = RTP_HEADER_SIZE + (size_t)(data[0] & 15u) * 4u;
        if (header > len - NANORTC_SRTP_AUTH_TAG_SIZE)
            return;
        if ((data[0] & 0x10u) != 0) {
            if (len - NANORTC_SRTP_AUTH_TAG_SIZE - header < 4u)
                return;
            size_t ext = (size_t)nanortc_read_u16be(data + header + 2u) * 4u;
            if (ext > len - NANORTC_SRTP_AUTH_TAG_SIZE - header - 4u)
                return;
#if NANORTC_FEATURE_VIDEO
            if (ext == 4u && nanortc_read_u16be(data + header) == RTP_EXT_PROFILE_ONE_BYTE &&
                (data[header + 4u] >> 4) == m->rtp.twcc_ext_id && m->rtp.twcc_ext_id != 0) {
                uint16_t wire = nanortc_read_u16be(data + header + 5u);
                out->transmit._internal.transport_seq =
                    rtc->twcc_sequence - (uint16_t)((uint16_t)rtc->twcc_sequence - wire);
                flags |= NANO_TX_TWCC;
            }
#endif
            header += 4u + ext;
        }
#if NANORTC_FEATURE_VIDEO
        for (uint8_t n = 0; n < NANORTC_NACK_RETX_RING; n++)
            if (data == rtc->nack_retx_buf[n])
                flags |= NANO_TX_RETRANSMIT;
#endif
        out->transmit._internal.payload_bytes =
            (uint16_t)(len - header - NANORTC_SRTP_AUTH_TAG_SIZE);
        out->transmit._internal.mid = m->mid;
        out->transmit._internal.flags = flags;
        return;
    }
}
#endif

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

static inline bool rtc_output_has_space(const nanortc_t *rtc)
{
    return (uint16_t)(rtc->out_tail - rtc->out_head) < NANORTC_OUT_QUEUE_SIZE;
}

#if NANORTC_FEATURE_TURN
static const nano_ice_candidate_t *rtc_turn_next_permission_candidate(const nanortc_t *rtc,
                                                                      uint32_t now_ms)
{
    /* RFC 8656 §9: permission fan-out follows signaling order, by peer IP. */
    for (uint8_t i = 0; i < rtc->ice.remote_candidate_count; i++) {
        const nano_ice_candidate_t *candidate = &rtc->ice.remote_candidates[i];
        if (turn_can_create_permission(&rtc->turn, candidate->addr, candidate->family, now_ms))
            return candidate;
    }
    return NULL;
}

static bool rtc_turn_can_bind_channel(const nanortc_t *rtc)
{
    return __atomic_load_n(&rtc->ice.selected_local_type, __ATOMIC_RELAXED) ==
               NANORTC_ICE_CAND_RELAY &&
           turn_can_bind_channel(&rtc->turn, rtc->ice.selected_addr, rtc->ice.selected_family,
                                 rtc->ice.selected_port);
}

#endif

/* TURN supplies permission readiness; ICE owns pair selection and deadlines. */
static uint32_t rtc_ice_get_check(const nanortc_t *rtc, uint32_t now_ms, nano_ice_check_t *check)
{
#if NANORTC_FEATURE_TURN
    bool ready[NANORTC_MAX_ICE_CANDIDATES];
    for (uint8_t i = 0; i < rtc->ice.remote_candidate_count; i++) {
        const nano_ice_candidate_t *r = &rtc->ice.remote_candidates[i];
        ready[i] = turn_peer_is_ready(&rtc->turn, r->addr, r->family);
    }
    return ice_get_check(&rtc->ice, now_ms, ready, check);
#else
    return ice_get_check(&rtc->ice, now_ms, NULL, check);
#endif
}

/* Release every managed backing buffer whose queue entry has just been
 * dequeued. Exact equality plus an explicit busy bit remains correct across
 * uint16 cursor wrap and after arbitrarily long idle periods. */
static void rtc_release_output_backing(nanortc_t *rtc)
{
    for (uint8_t i = 0; i < NANORTC_TX_SLOT_COUNT; i++) {
        uint32_t bit = UINT32_C(1) << i;
        if ((rtc->tx_slots_in_use & bit) != 0 && rtc->tx_slot_free_at[i] == rtc->out_head) {
            rtc->tx_slots_in_use &= ~bit;
        }
    }
#if NANORTC_FEATURE_VIDEO
    for (uint8_t i = 0; i < NANORTC_NACK_RETX_RING; i++) {
        uint64_t bit = UINT64_C(1) << i;
        if ((rtc->nack_retx_in_use & bit) != 0 && rtc->nack_retx_free_at[i] == rtc->out_head) {
            rtc->nack_retx_in_use &= ~bit;
        }
    }
#if NANORTC_FEATURE_VIDEO_FEC
    for (uint8_t i = 0; i < NANORTC_FEC_TX_RING; i++) {
        uint64_t bit = UINT64_C(1) << i;
        if ((rtc->fec_tx_in_use & bit) != 0 && rtc->fec_tx_free_at[i] == rtc->out_head) {
            rtc->fec_tx_in_use &= ~bit;
        }
    }
#endif
#endif
}

int nano_rtc_tx_slot_acquire(nanortc_t *rtc, uint8_t **buf, uint8_t *slot)
{
    if (!rtc || !buf || !slot) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (!rtc_output_has_space(rtc)) {
        __atomic_fetch_add(&rtc->stats_tx_queue_full, 1, __ATOMIC_RELAXED);
        return NANORTC_ERR_WOULD_BLOCK;
    }

    for (uint8_t n = 0; n < NANORTC_TX_SLOT_COUNT; n++) {
        uint8_t i = (uint8_t)((rtc->tx_slot_cursor + n) & (NANORTC_TX_SLOT_COUNT - 1));
        uint32_t bit = UINT32_C(1) << i;
        if ((rtc->tx_slots_in_use & bit) == 0) {
            rtc->tx_slot_cursor = (uint8_t)((i + 1u) & (NANORTC_TX_SLOT_COUNT - 1));
            *slot = i;
            *buf = rtc->tx_slots[i];
            return NANORTC_OK;
        }
    }

    NANORTC_LOGW(&rtc->config.log, "RTC", "transient tx slots full");
    return NANORTC_ERR_WOULD_BLOCK;
}

typedef enum {
    RTC_TX_ROUTE_DIRECT,
    RTC_TX_ROUTE_SELECTED,
    RTC_TX_ROUTE_FORCE_TURN,
} rtc_tx_route_t;

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
 * The wrap decision (selected_local_type == RELAY) is made here and stamped into
 * out_wrap_meta; the actual ChannelData / Send-indication encoding happens
 * in nanortc_poll_output().
 *
 * @p force_via_turn is the RFC 8445 §7.2.2 symmetric-path override. When a
 * Binding Request was received via a TURN Data Indication / ChannelData
 * unwrap, its response MUST be returned through the same relay even before
 * USE-CANDIDATE has flipped selected_type to RELAY. Without this override,
 * the first few pre-nomination responses leak direct to the peer's host
 * address; on cellular NAT blocks them silently so F6 appears to work, but
 * on loopback / LAN it opens a peer-reflexive direct path and ICE selects
 * HOST instead of RELAY. rtc_process_receive passes the inbound via_turn
 * here. All other callers pass false and rely on selected_type.
 */
static int rtc_enqueue_transmit_ref(nanortc_t *rtc, const uint8_t *data, size_t len,
                                    const nanortc_addr_t *peer_dest,
                                    const nanortc_addr_t *local_src, rtc_tx_route_t route)
{
    if (rtc->state == NANORTC_STATE_CLOSED)
        return NANORTC_ERR_STATE;
    if (len > UINT16_MAX)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    if (!nano_rtc_peer_can_send(rtc) && rtc_is_peer_payload(data, len))
        return NANORTC_ERR_WOULD_BLOCK;
    /* Lifetime contract (see nanortc.h `nanortc_output_t`): @p data must stay
     * owned until this queue entry is polled. Transient protocol producers use
     * tx_slots; video/FEC/NACK use their dedicated managed rings. */
    nanortc_output_t out;
    memset(&out, 0, sizeof(out));
    out.type = NANORTC_OUTPUT_TRANSMIT;
    out.transmit.data = data;
    out.transmit.len = len;
    out.transmit.dest = *peer_dest;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    rtc_transmit_metadata(rtc, &out);
#endif
    if (local_src) {
        out.transmit.src = *local_src;
    } else if (rtc->ice.selected_local_family != 0) {
        out.transmit.src.family = rtc->ice.selected_local_family;
        memcpy(out.transmit.src.addr, rtc->ice.selected_local_addr, NANORTC_ADDR_SIZE);
        out.transmit.src.port = rtc->ice.selected_local_port;
    }

#if NANORTC_FEATURE_TURN
    /* Capture slot before rtc_enqueue_output advances out_tail; rtc_enqueue_output
     * also clears out_wrap_meta[slot].via_turn, so we conditionally re-stamp it
     * below when the destination needs the lazy TURN wrap. */
    uint16_t slot = rtc->out_tail & (NANORTC_OUT_QUEUE_SIZE - 1);
#endif
    int rc = rtc_enqueue_output(rtc, &out);
    if (rc != NANORTC_OK) {
        /* PR-2 lifetime contract audit signal — must fire on all builds (not
         * just TURN), since CORE_ONLY/DATA/AUDIO can also exhaust out_queue
         * when callers fail to drain between handle_input ticks. */
        __atomic_fetch_add(&rtc->stats_tx_queue_full, 1, __ATOMIC_RELAXED);
        NANORTC_LOGW(&rtc->config.log, "RTC", "tx queue full, dropping output");
        return rc;
    }

#if NANORTC_FEATURE_TURN
    /* Wrap decision:
     *   - selected_local_type == RELAY: steady-state once USE-CANDIDATE nominated
     *     a relay pair (F6 from Phase 5.2).
     *   - force_via_turn: RFC 8445 §7.2.2 symmetric-path override used by
     *     rtc_process_receive when responding to a Binding Request that
     *     arrived via a TURN unwrap, before USE-CANDIDATE has had a chance
     *     to flip selected_type. Without this, the first pre-nomination
     *     responses leak direct and, on loopback / LAN, the peer builds a
     *     prflx direct candidate and ICE selects HOST instead of RELAY.
     * Permission existence alone is NOT a sufficient signal — we create
     * permissions for every remote candidate including LAN ones, and
     * routing same-LAN responses through a remote TURN server makes them
     * appear from the wrong source IP and the controlling browser drops
     * them as ICE pair mismatches. */
    bool needs_turn = route != RTC_TX_ROUTE_DIRECT && rtc->turn.configured &&
                      rtc->turn.state == NANORTC_TURN_ALLOCATED &&
                      (route == RTC_TX_ROUTE_FORCE_TURN ||
                       __atomic_load_n(&rtc->ice.selected_local_type, __ATOMIC_RELAXED) ==
                           NANORTC_ICE_CAND_RELAY);
    if (needs_turn) {
        rtc->out_wrap_meta[slot].via_turn = true;
        rtc->out_wrap_meta[slot].peer_dest = *peer_dest;
        __atomic_fetch_add(&rtc->stats_enqueue_via_turn, 1, __ATOMIC_RELAXED);
    } else if (route != RTC_TX_ROUTE_DIRECT) {
        __atomic_fetch_add(&rtc->stats_enqueue_direct, 1, __ATOMIC_RELAXED);
    }
#else
    (void)route;
#endif

    return NANORTC_OK;
}

int nano_rtc_enqueue_transmit(nanortc_t *rtc, const uint8_t *data, size_t len,
                              const nanortc_addr_t *peer_dest, bool force_via_turn)
{
    rtc_tx_route_t route = force_via_turn ? RTC_TX_ROUTE_FORCE_TURN : RTC_TX_ROUTE_SELECTED;
    return rtc_enqueue_transmit_ref(rtc, data, len, peer_dest, NULL, route);
}

static int rtc_tx_slot_commit_route(nanortc_t *rtc, uint8_t slot, size_t len,
                                    const nanortc_addr_t *peer_dest,
                                    const nanortc_addr_t *local_src, rtc_tx_route_t route)
{
    if (!rtc || !peer_dest || slot >= NANORTC_TX_SLOT_COUNT || len > NANORTC_TX_SLOT_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    uint32_t bit = UINT32_C(1) << slot;
    if ((rtc->tx_slots_in_use & bit) != 0) {
        return NANORTC_ERR_STATE;
    }

    int rc = rtc_enqueue_transmit_ref(rtc, rtc->tx_slots[slot], len, peer_dest, local_src, route);
    if (rc != NANORTC_OK) {
        return rc == NANORTC_ERR_BUFFER_TOO_SMALL ? NANORTC_ERR_WOULD_BLOCK : rc;
    }
    rtc->tx_slot_free_at[slot] = rtc->out_tail;
    rtc->tx_slots_in_use |= bit;
    return NANORTC_OK;
}

int nano_rtc_tx_slot_commit(nanortc_t *rtc, uint8_t slot, size_t len,
                            const nanortc_addr_t *peer_dest, bool force_via_turn)
{
    rtc_tx_route_t route = force_via_turn ? RTC_TX_ROUTE_FORCE_TURN : RTC_TX_ROUTE_SELECTED;
    return rtc_tx_slot_commit_route(rtc, slot, len, peer_dest, NULL, route);
}

static int rtc_tx_slot_commit_direct(nanortc_t *rtc, uint8_t slot, size_t len,
                                     const nanortc_addr_t *dest, const nanortc_addr_t *local_src)
{
    return rtc_tx_slot_commit_route(rtc, slot, len, dest, local_src, RTC_TX_ROUTE_DIRECT);
}

/* A3: Drain DTLS output into the transmit queue (uses relay wrapping if needed) */
static int rtc_drain_dtls_output(nanortc_t *rtc, const nanortc_addr_t *dest)
{
    while (rtc->dtls.out_len > 0) {
        uint8_t *buf = NULL;
        uint8_t slot = 0;
        int rc = nano_rtc_tx_slot_acquire(rtc, &buf, &slot);
        if (rc != NANORTC_OK) {
            return rc;
        }
        size_t dout_len = 0;
        rc = dtls_poll_output(&rtc->dtls, buf, NANORTC_TX_SLOT_SIZE, &dout_len);
        if (rc == NANORTC_ERR_NO_DATA) {
            return NANORTC_OK;
        }
        if (rc != NANORTC_OK) {
            return rc;
        }
        rc = nano_rtc_tx_slot_commit(rtc, slot, dout_len, dest, false);
        if (rc != NANORTC_OK) {
            return rc;
        }
    }
    return NANORTC_OK;
}

/* Emit a typed event with full event struct.
 * Non-static so nano_rtc_negotiate.c can call it via nano_rtc_internal.h. */
static uint16_t rtc_event_key(const nanortc_event_t *event)
{
    switch (event->type) {
    case NANORTC_EV_MEDIA_ADDED:
        return event->media_added.mid;
    case NANORTC_EV_MEDIA_CHANGED:
        return event->media_changed.mid;
    case NANORTC_EV_KEYFRAME_REQUEST:
        return event->keyframe_request.mid;
    case NANORTC_EV_DATACHANNEL_CLOSE:
        return event->datachannel_id.id;
    default:
        return 0;
    }
}

int nano_rtc_emit_event_full(nanortc_t *rtc, const nanortc_event_t *event)
{
    bool payload =
        event->type == NANORTC_EV_MEDIA_DATA || event->type == NANORTC_EV_DATACHANNEL_DATA ||
        event->type == NANORTC_EV_DATACHANNEL_OPEN || event->type == NANORTC_EV_ICE_CANDIDATE;
    if (!payload && (rtc->pending_event_count || !rtc_output_has_space(rtc))) {
        for (uint8_t i = 0; i < rtc->pending_event_count; i++) {
            nanortc_event_t *old = &rtc->pending_events[i];
            if (old->type == event->type && rtc_event_key(old) == rtc_event_key(event)) {
                memmove(old, old + 1, (rtc->pending_event_count - i - 1u) * sizeof(*old));
                rtc->pending_events[rtc->pending_event_count - 1u] = *event;
                return NANORTC_OK;
            }
        }
        if (rtc->pending_event_count == NANORTC_PENDING_EVENT_SLOTS)
            return NANORTC_ERR_WOULD_BLOCK;
        rtc->pending_events[rtc->pending_event_count++] = *event;
        return NANORTC_OK;
    }
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

    return nano_rtc_emit_event_full(rtc, &event);
}

int nanortc_init(nanortc_t *rtc, const nanortc_config_t *cfg)
{
    if (!rtc || !cfg) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    /* Validate the crypto provider's *required* surface up front (the members
     * crypto/nanortc_crypto.h marks "required"), so a partially-populated
     * provider fails fast at init instead of crashing on a NULL function
     * pointer at first DTLS handshake / STUN MESSAGE-INTEGRITY / SRTP use (or
     * on the nanortc_ice_restart() random_bytes() path). Optional members
     * (dtls_close_notify, dtls_set_role, and md5 — only for TURN long-term
     * credentials, already NULL-checked in turn_derive_key) are not required
     * here. SRTP members are required only when media transport is compiled. */
    if (dtls_validate_provider(cfg->crypto) != NANORTC_OK || !cfg->crypto->random_bytes ||
        !cfg->crypto->hmac_sha1) {
        return NANORTC_ERR_INVALID_PARAM;
    }
#if NANORTC_HAVE_MEDIA_TRANSPORT
    if (!cfg->crypto->aes_128_cm || !cfg->crypto->hmac_sha1_80) {
        return NANORTC_ERR_INVALID_PARAM;
    }
#endif
#if NANORTC_FEATURE_AUDIO
    if (cfg->jitter_depth_ms > NANO_TIME_MAX_INTERVAL_MS) {
        return NANORTC_ERR_INVALID_PARAM;
    }
#endif

    memset(rtc, 0, sizeof(*rtc));
    rtc->config = *cfg;
    rtc->state = NANORTC_STATE_NEW;

    /* RFC 8445 §5.2: tie-breaker MUST be a cryptographically random 64-bit
     * value. It feeds ICE-CONTROLLING / ICE-CONTROLLED on every outgoing
     * check and resolves 487 Role Conflict; a fixed zero is both non-RFC
     * and predictable. Fill from the crypto provider now; ice_init() cannot
     * do this itself because it has no access to cfg->crypto. */
    uint64_t tie_breaker = 0u;
    if (cfg->crypto->random_bytes((uint8_t *)&tie_breaker, sizeof(tie_breaker)) != 0) {
        memset(rtc, 0, sizeof(*rtc));
        return NANORTC_ERR_CRYPTO;
    }
    ice_init(&rtc->ice, cfg->role == NANORTC_ROLE_CONTROLLING);
    rtc->ice.tie_breaker = tie_breaker;
    rtc->transmit_token = tie_breaker & UINT64_C(0xffffffff00000000);

    /* Initialize logging only after fallible init-time RNG has succeeded. */
    NANORTC_LOGI(&rtc->config.log, "RTC", "nanortc_init");
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
#if NANORTC_FEATURE_VIDEO_RATE_CONTROL
    rate_control_init(&rtc->rc);
    /* rc_ladder / rc_ladder_n stay NULL/0 (rtc memset) until the application
     * calls nanortc_set_capability_ladder() — controller idle until then. */
#endif
#if NANORTC_FEATURE_VIDEO_PACING
    /* Token bucket starts full so the first frame keeps a small initial burst
     * (up to NANORTC_PACING_MAX_BURST_BYTES) before metering engages. All
     * other pacer fields are already zeroed by the rtc memset in init. */
    rtc->pacer.budget_bytes = NANORTC_PACING_MAX_BURST_BYTES;
#endif
#endif

    /* Process ICE servers (WebRTC RTCConfiguration.iceServers) */
#if NANORTC_FEATURE_TURN
    turn_init(&rtc->turn);
#endif
    if (cfg->ice_servers && cfg->ice_server_count > 0) {
        int irc = nano_rtc_apply_ice_servers(rtc, cfg->ice_servers, cfg->ice_server_count);
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
    NANORTC_LOGI(&rtc->config.log, "RTC", "nanortc_destroy");
    dtls_destroy(&rtc->dtls);
    rtc->state = NANORTC_STATE_CLOSED;
    rtc->transmit_pending = false;
    rtc->out_head = rtc->out_tail;
    rtc->pending_event_count = 0;
}

#if NANORTC_FEATURE_DATACHANNEL
static int rtc_deliver_sctp_to_dc(nanortc_t *rtc, const nano_sctp_message_t *message,
                                  nanortc_output_t *out);
#endif

static int rtc_poll_queued_output(nanortc_t *rtc, nanortc_output_t *out)
{
    while (rtc->out_head != rtc->out_tail) {
        uint16_t slot = rtc->out_head & (NANORTC_OUT_QUEUE_SIZE - 1);
        *out = rtc->out_queue[slot];
        if (out->type == NANORTC_OUTPUT_TRANSMIT && rtc->transmit_pending)
            return NANORTC_ERR_WOULD_BLOCK;
        if (out->type == NANORTC_OUTPUT_TRANSMIT && !nano_rtc_peer_can_send(rtc) &&
            rtc_is_peer_payload(out->transmit.data, out->transmit.len)) {
            rtc->stats_packets_dropped++;
            rtc->out_head++;
            rtc_release_output_backing(rtc);
            continue;
        }

#if NANORTC_FEATURE_TURN
        if (out->type == NANORTC_OUTPUT_TRANSMIT && rtc->out_wrap_meta[slot].via_turn) {
            const nanortc_addr_t *peer = &rtc->out_wrap_meta[slot].peer_dest;
            uint16_t channel = 0;
            size_t wrap_len = 0;
            int wrc;

            if (turn_find_channel_for_peer(&rtc->turn, peer->addr, peer->family, peer->port,
                                           &channel)) {
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
                __atomic_fetch_add(&rtc->stats_wrap_dropped, 1, __ATOMIC_RELAXED);
                NANORTC_LOGW(&rtc->config.log, "TURN", "lazy wrap failed, dropping output");
                rtc->out_head++;
                rtc_release_output_backing(rtc);
                continue;
            }

            /* Lifetime contract (see nanortc.h `nanortc_output_t`):
             * `rtc->turn_buf` is per-rtc shared scratch; the next
             * nanortc_poll_output() call may overwrite it for the next
             * lazy-wrapped output. The caller MUST consume out->transmit
             * (or memcpy it) before re-entering the library. */
            out->transmit.data = rtc->turn_buf;
            out->transmit.len = wrap_len;
            out->transmit.dest.family = (rtc->turn.server_family == STUN_FAMILY_IPV4) ? 4 : 6;
            memcpy(out->transmit.dest.addr, rtc->turn.server_addr, NANORTC_ADDR_SIZE);
            out->transmit.dest.port = rtc->turn.server_port;
            /* The relayed candidate is an address on the TURN server, not a
             * local interface the host can bind. Let the platform choose the
             * client socket's source for this outer TURN datagram. */
            memset(&out->transmit.src, 0, sizeof(out->transmit.src));
        }
#endif /* NANORTC_FEATURE_TURN */

        if (out->type == NANORTC_OUTPUT_TRANSMIT) {
            if (rtc->transmit_token == UINT64_MAX)
                return NANORTC_ERR_STATE;
            rtc->transmit_pending = true;
            rtc->transmit_pending_len = (uint16_t)out->transmit.len;
#if NANORTC_HAVE_MEDIA_TRANSPORT
            rtc->transmit_pending_seq = out->transmit._internal.transport_seq;
            rtc->transmit_pending_payload = out->transmit._internal.payload_bytes;
            rtc->transmit_pending_mid = out->transmit._internal.mid;
            rtc->transmit_pending_flags = out->transmit._internal.flags;
#endif
            out->transmit.token = ++rtc->transmit_token;
        }
        rtc->out_head++;
        rtc_release_output_backing(rtc);
        return NANORTC_OK;
    }

    return NANORTC_ERR_NO_DATA;
}

int nanortc_poll_output(nanortc_t *rtc, nanortc_output_t *out)
{
    if (!rtc || !out) {
        return NANORTC_ERR_INVALID_PARAM;
    }

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING
    /* Release any paced video fragments that have come due (relative to
     * rtc->now_ms from the last handle_input) before draining the queue, so
     * metered media interleaves with control output on the universal
     * handle_input → drain-poll_output loop. */
    if (!rtc->transmit_pending && rtc->state != NANORTC_STATE_CLOSED)
        nano_rtc_pacer_pump(rtc);
#endif

    int queued = rtc_poll_queued_output(rtc, out);
    if (queued != NANORTC_ERR_NO_DATA)
        return queued;

    if (rtc->pending_event_count) {
        memset(out, 0, sizeof(*out));
        out->type = NANORTC_OUTPUT_EVENT;
        out->event = rtc->pending_events[0];
        rtc->pending_event_count--;
        memmove(rtc->pending_events, rtc->pending_events + 1,
                rtc->pending_event_count * sizeof(rtc->pending_events[0]));
        return NANORTC_OK;
    }

    if (rtc->state == NANORTC_STATE_CLOSED)
        return NANORTC_ERR_NO_DATA;

    if (nano_rtc_candidate_produce(rtc, out) == NANORTC_OK)
        return NANORTC_OK;

#if NANORTC_FEATURE_DATACHANNEL
    for (;;) {
        nano_sctp_message_t message;
        int rc = nsctp_poll_delivery(&rtc->sctp, &message);
        if (rc == NANORTC_ERR_WOULD_BLOCK)
            break;
        if (rc != NANORTC_OK)
            return rc;
        rc = rtc_deliver_sctp_to_dc(rtc, &message, out);
        if (rc != NANORTC_ERR_NO_DATA)
            return rc;
    }
#endif

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER
    /* Output queue drained: release one reordered video NAL directly into *out
     * (one per call so the application consumes the event before the next call's
     * pop reuses the shared depkt buffer). A skip during the drain may enqueue
     * an auto-PLI — dispatch it through the same queue drain (including TURN wrap). */
    if (nano_rtc_media_reorder_produce(rtc, out) == NANORTC_OK) {
        return NANORTC_OK;
    }
    if (rtc->out_head != rtc->out_tail) {
        return rtc_poll_queued_output(rtc, out);
    }
#endif
#if NANORTC_FEATURE_AUDIO
    /* Output queue drained: release one due audio frame directly into *out (one
     * per call so the application consumes it before the next reuses media_buf). */
    if (nano_rtc_media_audio_produce(rtc, out) == NANORTC_OK) {
        return NANORTC_OK;
    }
#endif
    return NANORTC_ERR_NO_DATA;
}

/* ----------------------------------------------------------------
 * nanortc_next_timeout_ms — deadline aggregator
 * ----------------------------------------------------------------
 * Combines protocol and playout deadlines using the caller's clock. Pending
 * output wakes immediately; an unarmed connection uses a 1-second idle cap.
 */
#define NANORTC_TIMEOUT_IDLE_CAP_MS 1000u

int nanortc_report_transmit(nanortc_t *rtc, uint64_t token, nanortc_transmit_result_t result,
                            uint32_t at_ms)
{
    if (!rtc || !rtc->transmit_pending || token != rtc->transmit_token ||
        (result != NANORTC_TRANSMIT_SENT && result != NANORTC_TRANSMIT_DROPPED))
        return NANORTC_ERR_INVALID_PARAM;
    rtc->transmit_pending = false;
    if (result == NANORTC_TRANSMIT_DROPPED) {
        rtc->stats_packets_dropped++;
#if NANORTC_FEATURE_VIDEO
        bwe_record_drop(&rtc->bwe, at_ms);
#endif
        return NANORTC_OK;
    }
    rtc->stats_packets_sent++;
    rtc->stats_bytes_sent += rtc->transmit_pending_len;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    uint8_t flags = rtc->transmit_pending_flags;
    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, rtc->transmit_pending_mid);
    if ((flags & NANO_TX_SR) != 0 && m) {
        m->rtcp.last_tx_sr_ntp = rtc->transmit_pending_seq;
        m->rtcp.last_tx_sr_ms = at_ms;
        m->rtcp.last_tx_sr_valid = true;
    }
    if ((flags & NANO_TX_RTP) != 0 && m) {
        m->rtcp.packets_sent++;
        m->rtcp.octets_sent += rtc->transmit_pending_payload;
        rate_window_on_bytes(&m->rate_window, at_ms, rtc->transmit_pending_len);
    }
#if NANORTC_FEATURE_VIDEO
    if ((flags & (NANO_TX_RTP | NANO_TX_FEC)) != 0)
        bwe_record_transmit(&rtc->bwe, rtc->transmit_pending_seq, rtc->transmit_pending_len, at_ms,
                            (flags & NANO_TX_TWCC) != 0, (flags & NANO_TX_RETRANSMIT) != 0);
#endif
#else
    (void)at_ms;
#endif
    return NANORTC_OK;
}

void nano_rtc_tag_last_sr(nanortc_t *rtc, uint8_t mid, uint32_t compact_ntp)
{
    nanortc_output_t *out = &rtc->out_queue[(rtc->out_tail - 1u) & (NANORTC_OUT_QUEUE_SIZE - 1u)];
    out->transmit._internal.mid = mid;
    out->transmit._internal.transport_seq = compact_ntp;
    out->transmit._internal.flags = NANO_TX_SR;
}

int nanortc_get_stats(const nanortc_t *rtc, nanortc_stats_t *stats)
{
    if (!rtc || !stats)
        return NANORTC_ERR_INVALID_PARAM;
    memset(stats, 0, sizeof(*stats));
    stats->connection_state = rtc->state;
    stats->ice_state = rtc->ice.state;
    stats->is_controlling = rtc->ice.is_controlling;
    stats->remote_candidate_count = rtc->ice.remote_candidate_count;
    for (uint8_t i = 0; i < rtc->ice.remote_candidate_count; i++)
        if (rtc->ice.remote_candidates[i].type == NANORTC_ICE_CAND_RELAY)
            stats->remote_relay_candidate_count++;
    stats->selected_local_type = rtc->ice.selected_local_type;
    stats->selected_remote_type = rtc->ice.selected_type;
    stats->selected_local.family = rtc->ice.selected_local_family;
    stats->selected_local.port = rtc->ice.selected_local_port;
    memcpy(stats->selected_local.addr, rtc->ice.selected_local_addr, NANORTC_ADDR_SIZE);
    stats->selected_remote.family = rtc->ice.selected_family;
    stats->selected_remote.port = rtc->ice.selected_port;
    memcpy(stats->selected_remote.addr, rtc->ice.selected_addr, NANORTC_ADDR_SIZE);
    stats->packets_sent = rtc->stats_packets_sent;
    stats->bytes_sent = rtc->stats_bytes_sent;
    stats->packets_dropped = rtc->stats_packets_dropped;
    stats->tx_queue_full = rtc->stats_tx_queue_full;
    stats->output_free_slots = nanortc_output_free_slots(rtc);
    stats->output_queued = (uint16_t)(rtc->out_tail - rtc->out_head);
    stats->transmit_pending = rtc->transmit_pending;
#if NANORTC_FEATURE_TURN
    stats->enqueue_direct = rtc->stats_enqueue_direct;
    stats->enqueue_via_turn = rtc->stats_enqueue_via_turn;
    stats->wrap_dropped = rtc->stats_wrap_dropped;
#endif
#if NANORTC_FEATURE_VIDEO
    stats->packet_ring_overrun = rtc->stats_pkt_ring_overrun;
    stats->feedback_history_missed = rtc->bwe.history_missed;
    stats->estimated_bitrate_bps = bwe_get_bitrate(&rtc->bwe);
    stats->remb_cap_bps = rtc->bwe.remb_limit;
    stats->remb_cap_valid = rtc->bwe.remb_count != 0;
#if NANORTC_FEATURE_VIDEO_FEC
    stats->fec_sent = rtc->stats_fec_sent;
    stats->fec_dropped_resource = rtc->stats_fec_dropped_resource;
    stats->fec_recovered = rtc->stats_fec_recovered;
#endif
#if NANORTC_FEATURE_VIDEO_PACING
    stats->pacer_queued = (uint16_t)(rtc->pacer.tail - rtc->pacer.head);
    stats->paced_packets = rtc->stats_paced_packets;
    stats->pace_catchup = rtc->stats_pace_catchup;
#endif
#endif
    return NANORTC_OK;
}

int nanortc_next_timeout_ms(const nanortc_t *rtc, uint32_t now_ms, uint32_t *out_ms)
{
    if (!rtc || !out_ms) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (rtc->state == NANORTC_STATE_CLOSED) {
        *out_ms = rtc->out_head != rtc->out_tail || rtc->pending_event_count
                      ? 0u
                      : NANORTC_TIMEOUT_IDLE_CAP_MS;
        return NANORTC_OK;
    }
    bool blocked = rtc->transmit_pending;
    uint32_t best = (!blocked && (rtc->out_head != rtc->out_tail || rtc->pending_event_count ||
                                  rtc->dtls.out_len || rtc->srflx_candidate_pending))
                        ? 0u
                        : UINT32_MAX;
    for (uint8_t i = 0; i < NANORTC_MAX_LOCAL_CANDIDATES; i++)
        if (!blocked && rtc->host_candidate_pending[i])
            best = 0;
#if NANORTC_FEATURE_TURN
    if (!blocked && rtc->relay_candidate_pending)
        best = 0;
#endif

    /* ICE: connectivity checks + consent freshness + consent expiry. */
    {
        nano_ice_check_t check;
        uint32_t d = rtc->ice.state == NANORTC_ICE_STATE_CONNECTED
                         ? ice_next_timeout_ms(&rtc->ice, now_ms)
                         : rtc_ice_get_check(rtc, now_ms, &check);
        if (d < best) {
            best = d;
        }
        if (rtc->ice_restarting && rtc->ice.consent_expiry_ms &&
            !nano_time_is_due(now_ms, rtc->ice.consent_expiry_ms)) {
            uint32_t expiry = nano_time_until(now_ms, rtc->ice.consent_expiry_ms);
            if (expiry < best)
                best = expiry;
        }
    }

    /* STUN srflx retry (RFC 8445 §5.1.1.1). */
    if (rtc->stun_server_configured && !rtc->srflx_discovered && rtc->stun_retries < 3) {
        if (rtc->stun_retry_at_ms == 0) {
            best = 0;
        } else {
            uint32_t left = nano_time_until(now_ms, rtc->stun_retry_at_ms);
            if (left < best) {
                best = left;
            }
        }
    }

#if NANORTC_FEATURE_TURN
    {
        uint32_t d = turn_next_timeout_ms(&rtc->turn, now_ms);
        if (d < best) {
            best = d;
        }
        if (rtc_turn_next_permission_candidate(rtc, now_ms) != NULL ||
            rtc_turn_can_bind_channel(rtc)) {
            best = 0u;
        }
    }
#endif

#if NANORTC_FEATURE_DATACHANNEL
    {
        uint32_t d = nsctp_next_timeout_ms(&rtc->sctp, now_ms);
        if (d < best) {
            best = d;
        }
        if (!blocked && (rtc->dtls.out_len > 0u || nsctp_has_pending_output(&rtc->sctp) ||
                         (rtc->sctp.state == NANORTC_SCTP_STATE_ESTABLISHED &&
                          dc_has_pending_output(&rtc->datachannel)))) {
            best = 0u;
        }
    }
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* RTCP Sender Report cadence (RFC 3550 §6.2). Only armed once SRTP
     * keys are derived — before that, the RTCP block in rtc_process_timers
     * is a no-op. */
    if (rtc->srtp.ready) {
        uint32_t left = 0u;
        if (rtc->last_rtcp_send_valid) {
            uint32_t since = nano_time_elapsed(now_ms, rtc->last_rtcp_send_ms);
            left = (since >= NANORTC_RTCP_INTERVAL_MS)
                       ? 0u
                       : (uint32_t)(NANORTC_RTCP_INTERVAL_MS - since);
        }
        if (left < best) {
            best = left;
        }
    }

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_PACING
    /* Send pacer: wake when the next paced video fragment is due so the
     * application drains and pumps it on schedule (UINT32_MAX when idle). */
    {
        uint32_t d = blocked ? UINT32_MAX : nano_rtc_pacer_next_deadline_ms(rtc, now_ms);
        if (d < best) {
            best = d;
        }
    }
#endif
#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER
    /* Receive reorder buffer: wake when a held packet hits the skip cap so the
     * gap is declared lost and the frame released even if no further RTP comes. */
    {
        uint32_t d = nano_rtc_media_reorder_next_timeout(rtc, now_ms);
        if (d < best) {
            best = d;
        }
    }
#endif
#endif

    uint32_t dtls_left = dtls_next_timeout_ms(&rtc->dtls, now_ms);
    if (dtls_left < best)
        best = dtls_left;
#if NANORTC_FEATURE_AUDIO
    for (uint8_t i = 0; i < rtc->media_count; i++) {
        const nanortc_track_t *m = &rtc->media[i];
        if (!m->active || m->kind != NANORTC_TRACK_AUDIO)
            continue;
        uint32_t left = jitter_next_timeout_ms(&m->track.audio.jitter, now_ms);
        if (left < best)
            best = left;
    }
#endif

    /* Idle cap: if nothing armed a deadline, return the conservative
     * default so callers don't block forever on a fully idle session. */
    if (best == UINT32_MAX) {
        best = NANORTC_TIMEOUT_IDLE_CAP_MS;
    }
    if ((blocked || !nano_rtc_peer_can_send(rtc)) && best == 0u)
        best = 1u; /* Socket completion owns the blocked send deadline. */

    *out_ms = best;
    return NANORTC_OK;
}

uint16_t nanortc_output_free_slots(const nanortc_t *rtc)
{
    if (!rtc) {
        return 0;
    }
    uint16_t used = (uint16_t)(rtc->out_tail - rtc->out_head);
    if (used >= NANORTC_OUT_QUEUE_SIZE) {
        return 0;
    }
    return (uint16_t)(NANORTC_OUT_QUEUE_SIZE - used);
}

/* Init DTLS (if needed) and begin handshake after ICE connects.
 * State is only advanced to DTLS_HANDSHAKING after dtls_start() succeeds.
 * On failure, state remains at ICE_CONNECTED so the caller can retry
 * DTLS startup from the ICE-connected state. Note: this only governs
 * the FSM; the inbound DTLS gate is handle_input's `state >= ICE_CONNECTED`
 * check, so DTLS records remain accepted on the failure-retry path. */
static int rtc_begin_dtls_handshake(nanortc_t *rtc, const nanortc_addr_t *src)
{
    int is_server = (rtc->sdp.local_setup == NANORTC_SDP_SETUP_PASSIVE);

    /* accept_offer() does early init; this guard covers create_offer() path */
    if (!rtc->dtls.crypto_ctx) {
        int rc = dtls_init(&rtc->dtls, rtc->config.crypto, is_server);
        if (rc != NANORTC_OK)
            return rc;
    }

    if (rtc->sdp.has_remote_fingerprint) {
        int rc = dtls_set_peer_fingerprint(&rtc->dtls, rtc->sdp.remote_fingerprint);
        if (rc != NANORTC_OK)
            return rc;
    }

    rtc->remote_addr = *src;
    dtls_set_time(&rtc->dtls, rtc->now_ms);
    if (!is_server) {
        int rc = dtls_start(&rtc->dtls);
        if (rc != NANORTC_OK)
            return rc;
    }

    rtc->state = NANORTC_STATE_DTLS_HANDSHAKING;

    if (!is_server) {
        int rc = rtc_drain_dtls_output(rtc, src);
        if (rc == NANORTC_ERR_WOULD_BLOCK) {
            return NANORTC_OK; /* DTLS retains the flight; the next tick drains it */
        }
        if (rc != NANORTC_OK) {
            return rc;
        }
    }
    return NANORTC_OK;
}

#if NANORTC_FEATURE_DATACHANNEL
/* ----------------------------------------------------------------
 * Internal: deliver one SCTP message to DataChannel layer + emit events
 * ---------------------------------------------------------------- */

static int rtc_deliver_sctp_to_dc(nanortc_t *rtc, const nano_sctp_message_t *message,
                                  nanortc_output_t *out)
{
    bool opened;
    int rc = dc_handle_message(&rtc->datachannel, message->stream_id, message->ppid, message->data,
                               message->len, &opened);
    if (rc != NANORTC_OK)
        return rc;
    if (message->ppid == DCEP_PPID_CONTROL && !opened)
        return NANORTC_ERR_NO_DATA;
    memset(out, 0, sizeof(*out));
    out->type = NANORTC_OUTPUT_EVENT;
    nanortc_event_t *event = &out->event;
    if (message->ppid == DCEP_PPID_CONTROL) {
        event->type = NANORTC_EV_DATACHANNEL_OPEN;
        event->datachannel_open.id = message->stream_id;
        const nano_dc_channel_t *ch = dc_find_channel(&rtc->datachannel, message->stream_id);
        event->datachannel_open.label = ch->label;
        event->datachannel_open.protocol = ch->protocol;
        event->datachannel_open.ordered = ch->ordered;
        event->datachannel_open.partial_reliability =
            (ch->channel_type & 0x7f) == DCEP_CHANNEL_REXMIT;
        event->datachannel_open.max_retransmits = ch->max_retransmits;
    } else {
        event->type = NANORTC_EV_DATACHANNEL_DATA;
        event->datachannel_data.id = message->stream_id;
        event->datachannel_data.data = message->data;
        event->datachannel_data.len =
            (message->ppid == DCEP_PPID_BINARY_EMPTY || message->ppid == DCEP_PPID_STRING_EMPTY)
                ? 0
                : message->len;
        event->datachannel_data.binary =
            message->ppid == DCEP_PPID_BINARY || message->ppid == DCEP_PPID_BINARY_EMPTY;
    }
    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * Internal: drain SCTP output through DTLS encrypt → transmit queue
 * ---------------------------------------------------------------- */

static int rtc_pump_sctp_through_dtls(nanortc_t *rtc, const nanortc_addr_t *dest)
{
    if (!nano_rtc_peer_can_send(rtc))
        return NANORTC_ERR_WOULD_BLOCK;
    int rc = rtc_drain_dtls_output(rtc, dest);
    if (rc != NANORTC_OK && rc != NANORTC_ERR_NO_DATA) {
        return rc;
    }

    size_t nsctp_out = 0;
    uint8_t nsctp_buf[NANORTC_SCTP_MTU];
    for (;;) {
        uint8_t *tx_buf = NULL;
        uint8_t tx_slot = 0;
        rc = nano_rtc_tx_slot_acquire(rtc, &tx_buf, &tx_slot);
        if (rc != NANORTC_OK) {
            return rc;
        }
        rtc->sctp.now_ms = rtc->now_ms;
        rc = nsctp_poll_output(&rtc->sctp, nsctp_buf, sizeof(nsctp_buf), &nsctp_out);
        if (rc == NANORTC_ERR_NO_DATA || nsctp_out == 0) {
            return NANORTC_OK;
        }
        if (rc != NANORTC_OK) {
            return rc;
        }
        rc = dtls_encrypt(&rtc->dtls, nsctp_buf, nsctp_out);
        if (rc != NANORTC_OK) {
            return rc;
        }
        size_t enc_len = 0;
        rc = dtls_poll_output(&rtc->dtls, tx_buf, NANORTC_TX_SLOT_SIZE, &enc_len);
        if (rc != NANORTC_OK) {
            return rc;
        }
        rc = nano_rtc_tx_slot_commit(rtc, tx_slot, enc_len, dest, false);
        if (rc != NANORTC_OK) {
            return rc;
        }
    }
}

/* SCTP copies the DCEP bytes before the per-channel work is committed. */
static int rtc_pump_dc_through_sctp(nanortc_t *rtc, const nanortc_addr_t *dest)
{
    if (rtc->sctp.state != NANORTC_SCTP_STATE_ESTABLISHED)
        return NANORTC_OK;
    uint8_t buf[NANORTC_DC_OUT_BUF_SIZE];
    size_t len;
    uint16_t stream;
    int rc = dc_peek_output(&rtc->datachannel, buf, sizeof(buf), &len, &stream);
    if (rc == NANORTC_ERR_NO_DATA)
        return NANORTC_OK;
    if (rc != NANORTC_OK)
        return rc;
    rc = nsctp_send_options(&rtc->sctp, stream, DCEP_PPID_CONTROL, buf, len, false, -1);
    if (rc != NANORTC_OK)
        return rc;
    dc_commit_output(&rtc->datachannel, stream);
    return rtc_pump_sctp_through_dtls(rtc, dest);
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

static int rtc_finish_dtls(nanortc_t *rtc, const nanortc_addr_t *src)
{
    /* Check for DTLS state transition → emit event */
    if (rtc->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED &&
        rtc->state < NANORTC_STATE_DTLS_CONNECTED) {
#if NANORTC_HAVE_MEDIA_TRANSPORT
        uint8_t rtp_seed[NANORTC_MAX_MEDIA_TRACKS][sizeof(uint32_t) + sizeof(uint16_t)];
        if (rtc->dtls.keying_material_ready) {
            /* Stage every track seed before committing connection/media
             * state so fail-on-Nth RNG cannot leave half-seeded tracks. */
            for (uint8_t ti = 0; ti < rtc->media_count; ti++) {
                if (rtc->media[ti].active &&
                    rtc->config.crypto->random_bytes(rtp_seed[ti], sizeof(rtp_seed[ti])) != 0) {
                    return NANORTC_ERR_CRYPTO;
                }
            }
        }
#endif
        rtc->state = NANORTC_STATE_DTLS_CONNECTED;
        rtc->remote_addr = *src; /* save for timeout-driven output */

        nano_rtc_cache_fingerprint(rtc);

#if NANORTC_HAVE_MEDIA_TRANSPORT
        /* Derive SRTP keys from DTLS keying material (RFC 5764 §4.2) */
        if (rtc->dtls.keying_material_ready) {
            int is_client = !rtc->dtls.is_server;
            nano_srtp_init(&rtc->srtp, rtc->config.crypto, is_client);
            nano_srtp_derive_keys(&rtc->srtp, rtc->dtls.keying_material, NANORTC_DTLS_KEYING_SIZE);
            /* Anchor the first periodic SR to key activation. Timestamp
             * zero is valid, so keep validity explicit. */
            rtc->last_rtcp_send_ms = rtc->now_ms;
            rtc->last_rtcp_send_valid = true;

            /* Commit the staged SSRC + initial sequence for every track. */
            for (uint8_t ti = 0; ti < rtc->media_count; ti++) {
                nanortc_track_t *m = &rtc->media[ti];
                if (!m->active)
                    continue;
                uint32_t ssrc = nanortc_read_u32be(rtp_seed[ti]);
                uint16_t init_seq = nanortc_read_u16be(rtp_seed[ti] + sizeof(uint32_t));
                /* Find negotiated PT from SDP mline */
                uint8_t pt = m->rtp.payload_type;
                nano_sdp_mline_t *ml = sdp_find_mline(&rtc->sdp, m->mid);
                if (ml && ml->pt != 0)
                    pt = ml->pt;
                rtp_init(&m->rtp, ssrc, pt);
                m->rtp.seq = init_seq;
                m->rtcp.ssrc = ssrc;
                ssrc_map_register(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, ssrc, m->mid);
                NANORTC_LOGD(&rtc->config.log, "RTP", "track RTP initialized");
            }
            NANORTC_LOGI(&rtc->config.log, "RTC", "SRTP keys derived, RTP ready");
        }
#endif

#if NANORTC_FEATURE_DATACHANNEL
        /* Initiate SCTP only if m=application was negotiated */
        if (rtc->sdp.has_datachannel) {
            /* DTLS client sends INIT (RFC 8831) */
            if (!rtc->dtls.is_server) {
                int sctp_rc = nsctp_start(&rtc->sctp);
                if (sctp_rc != NANORTC_OK) {
                    return sctp_rc;
                }
                rtc->state = NANORTC_STATE_SCTP_CONNECTING;

                /* Drain SCTP output (INIT) through DTLS encrypt */
                sctp_rc = rtc_pump_sctp_through_dtls(rtc, src);
                if (sctp_rc != NANORTC_OK && sctp_rc != NANORTC_ERR_WOULD_BLOCK) {
                    return sctp_rc;
                }
            }
        } else {
            /* Media-only session — DTLS connected is final state */
            rtc->state = NANORTC_STATE_CONNECTED;
            rtc_emit_connected(rtc);
            NANORTC_LOGI(&rtc->config.log, "RTC", "connected (media only, no SCTP)");
        }
#else
        /* No DataChannel — DTLS connected is final state */
        rtc->state = NANORTC_STATE_CONNECTED;
        rtc_emit_connected(rtc);
        NANORTC_LOGI(&rtc->config.log, "RTC", "connected (no DC)");
#endif
    }
    return NANORTC_OK;
}

static int rtc_process_receive(nanortc_t *rtc, const uint8_t *data, size_t len,
                               const nanortc_addr_t *src, uint8_t local_idx, bool via_turn)
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
                /* Propagate the outer packet's local_idx so USE-CANDIDATE on
                 * a relay-delivered Binding Request records the correct local
                 * candidate (the socket that received the ChannelData from
                 * the TURN server) instead of silently degrading to idx 0 on
                 * multi-candidate setups. If the outer packet already had an
                 * UNKNOWN idx, that propagates unchanged. */
                return rtc_process_receive(rtc, cd_payload, cd_len, &peer_src, local_idx, true);
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
        bool pending_srflx_response =
            rtc->stun_server_configured && !rtc->srflx_discovered &&
            src->family == rtc->stun_server_family && src->port == rtc->stun_server_port &&
            memcmp(src->addr, rtc->stun_server_addr, src->family == 4 ? 4u : 16u) == 0 &&
            len >= STUN_HEADER_SIZE && nanortc_read_u16be(data) == STUN_BINDING_RESPONSE &&
            memcmp(data + 8, rtc->stun_txid, STUN_TXID_SIZE) == 0;
        if (!pending_srflx_response && rtc->turn.configured &&
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
                /* See ChannelData branch above for local_idx rationale. */
                return rtc_process_receive(rtc, payload, payload_len, &peer_src, local_idx, true);
            }

            /* Not a Data indication — try as TURN response */
            nano_turn_state_t prev_state = rtc->turn.state;
            int trc = turn_handle_response(&rtc->turn, rtc->now_ms, data, len, rtc->config.crypto);
            if (trc != NANORTC_OK) {
                return trc;
            }

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
                rtc->relay_candidate_pending = true;

                /* RFC 8445 §5.1.1.2: the relayed address is a real local
                 * candidate and must participate in the checklist. */
                if (rtc->ice.local_candidate_count < NANORTC_MAX_LOCAL_CANDIDATES) {
                    bool dup = false;
                    for (uint8_t i = 0; i < rtc->ice.local_candidate_count; i++) {
                        const nano_ice_candidate_t *c = &rtc->ice.local_candidates[i];
                        if (c->family == relay_fam && c->port == rtc->turn.relay_port &&
                            memcmp(c->addr, rtc->turn.relay_addr, NANORTC_ADDR_SIZE) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        uint8_t idx = rtc->ice.local_candidate_count;
                        rtc->ice.local_candidates[idx].family = relay_fam;
                        memcpy(rtc->ice.local_candidates[idx].addr, rtc->turn.relay_addr,
                               NANORTC_ADDR_SIZE);
                        rtc->ice.local_candidates[idx].port = rtc->turn.relay_port;
                        rtc->ice.local_candidates[idx].type = NANORTC_ICE_CAND_RELAY;
                        rtc->ice.local_candidate_count = (uint8_t)(idx + 1);
                    }
                }

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
                    rtc->srflx_candidate_pending = true;

                    NANORTC_LOGI(&rtc->config.log, "RTC", "srflx candidate discovered");

#if NANORTC_FEATURE_ICE_SRFLX
                    /* RFC 8445 §5.1.1.2: pair srflx with all remote candidates
                     * by adding it to the local candidate set. Without this,
                     * connectivity checks never traverse srflx — they stay
                     * pinned to the host candidate that was added at startup.
                     * Dedup against existing entries so repeated Binding
                     * Responses (multiple STUN servers, retries) don't
                     * accumulate duplicate slots. */
                    if (rtc->ice.local_candidate_count < NANORTC_MAX_LOCAL_CANDIDATES) {
                        bool dup = false;
                        for (uint8_t i = 0; i < rtc->ice.local_candidate_count; i++) {
                            const nano_ice_candidate_t *c = &rtc->ice.local_candidates[i];
                            if (c->family == mapped_fam && c->port == smsg.mapped_port &&
                                memcmp(c->addr, smsg.mapped_addr, NANORTC_ADDR_SIZE) == 0) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) {
                            uint8_t idx = rtc->ice.local_candidate_count;
                            rtc->ice.local_candidates[idx].family = mapped_fam;
                            memcpy(rtc->ice.local_candidates[idx].addr, smsg.mapped_addr,
                                   NANORTC_ADDR_SIZE);
                            rtc->ice.local_candidates[idx].port = smsg.mapped_port;
                            rtc->ice.local_candidates[idx].type = NANORTC_ICE_CAND_SRFLX;
                            rtc->ice.local_candidate_count = (uint8_t)(idx + 1);
                        }
                    }
#endif /* NANORTC_FEATURE_ICE_SRFLX */
                }
                return NANORTC_OK;
            }
        }

        bool was_consent_pending = rtc->ice.consent_pending;
        uint8_t *stun_out = rtc->stun_buf;
        size_t stun_out_size = sizeof(rtc->stun_buf);
        uint8_t tx_slot = 0;
        bool response_slot = nanortc_read_u16be(data) == STUN_BINDING_REQUEST;
        if (response_slot) {
            int arc = nano_rtc_tx_slot_acquire(rtc, &stun_out, &tx_slot);
            if (arc != NANORTC_OK) {
                return arc; /* request not consumed: drain and retry the same input */
            }
            stun_out_size = NANORTC_TX_SLOT_SIZE;
        }
        size_t resp_len = 0;
        uint8_t candidates_before = rtc->ice.remote_candidate_count;
        int rc = ice_handle_stun(&rtc->ice, data, len, src, local_idx, via_turn, rtc->config.crypto,
                                 stun_out, stun_out_size, &resp_len);
        if (rc != NANORTC_OK) {
            return rc;
        }
        for (uint8_t i = candidates_before; i < rtc->ice.remote_candidate_count; i++)
            memset(&rtc->remote_candidate_related[i], 0, sizeof(rtc->remote_candidate_related[i]));

        /* RFC 7675 §5.1: only a matching authenticated response renews consent;
         * an incoming request merely asks for consent in the other direction. */
        if (was_consent_pending && !rtc->ice.consent_pending) {
            rtc->ice.consent_expiry_ms =
                nano_time_deadline(rtc->now_ms, NANORTC_ICE_CONSENT_TIMEOUT_MS);
        }

        /* Enqueue STUN response for transmission. Use nano_rtc_enqueue_transmit so
         * that if `src` is a peer reached via the TURN relay (the recursive
         * call from turn_unwrap_data / turn_unwrap_channel_data hands us the
         * inner peer address, not the TURN server), the response is wrapped
         * in a Send indication / ChannelData and routed back through the
         * relay. Direct sendto() on a NAT'd cellular peer address has no
         * route from the device. */
        if (resp_len > 0) {
            if (!response_slot) {
                return NANORTC_ERR_PROTOCOL; /* only Binding Requests may produce a response */
            }
            /* RFC 8445 §7.2.2: response follows the request's arrival path.
             * If the Binding Request came in via a TURN unwrap (via_turn),
             * the response must also go back through the relay — even before
             * USE-CANDIDATE has flipped selected_type. Without this, the
             * first pre-nomination responses leak direct and the peer ICE
             * stack builds a prflx direct candidate on loopback / LAN. */
            uint8_t base = ice_local_base_idx(&rtc->ice, local_idx);
            if (!via_turn && base < rtc->ice.local_candidate_count) {
                const nano_ice_candidate_t *c = &rtc->ice.local_candidates[base];
                nanortc_addr_t local = {.family = c->family, .port = c->port};
                memcpy(local.addr, c->addr, NANORTC_ADDR_SIZE);
                rc = rtc_tx_slot_commit_direct(rtc, tx_slot, resp_len, src, &local);
            } else {
                rc = nano_rtc_tx_slot_commit(rtc, tx_slot, resp_len, src, via_turn);
            }
            if (rc != NANORTC_OK) {
                return rc;
            }
        }

        /* Check for ICE state transition → init DTLS + emit event */
        if (rtc->ice.state == NANORTC_ICE_STATE_CONNECTED &&
            (rtc->state < NANORTC_STATE_ICE_CONNECTED || rtc->ice_restarting)) {
            if (!rtc->ice_restarting)
                rtc->state = NANORTC_STATE_ICE_CONNECTED;
            /* Arm consent freshness timers (RFC 7675) */
            rtc->ice.consent_next_ms =
                nano_time_deadline(rtc->now_ms, NANORTC_ICE_CONSENT_INTERVAL_MS);
            rtc->ice.consent_expiry_ms =
                nano_time_deadline(rtc->now_ms, NANORTC_ICE_CONSENT_TIMEOUT_MS);
            /* Emit ICE_STATE_CHANGE (CONNECTED emitted later when fully ready) */
            {
                nanortc_event_t ice_evt;
                memset(&ice_evt, 0, sizeof(ice_evt));
                ice_evt.type = NANORTC_EV_ICE_STATE_CHANGE;
                ice_evt.ice_state = (uint16_t)NANORTC_ICE_STATE_CONNECTED;
                nano_rtc_emit_event_full(rtc, &ice_evt);
            }

            if (rtc->ice_restarting) {
                rtc->remote_addr = *src;
                rtc->ice_restarting = false;
                for (uint16_t cursor = rtc->out_head; cursor != rtc->out_tail; cursor++) {
                    uint16_t slot = cursor & (NANORTC_OUT_QUEUE_SIZE - 1u);
                    nanortc_output_t *pending = &rtc->out_queue[slot];
                    if (pending->type != NANORTC_OUTPUT_TRANSMIT ||
                        !rtc_is_peer_payload(pending->transmit.data, pending->transmit.len))
                        continue;
                    pending->transmit.dest = *src;
                    pending->transmit.src.family = rtc->ice.selected_local_family;
                    pending->transmit.src.port = rtc->ice.selected_local_port;
                    memcpy(pending->transmit.src.addr, rtc->ice.selected_local_addr,
                           NANORTC_ADDR_SIZE);
#if NANORTC_FEATURE_TURN
                    rtc->out_wrap_meta[slot].via_turn =
                        rtc->ice.selected_local_type == NANORTC_ICE_CAND_RELAY;
                    rtc->out_wrap_meta[slot].peer_dest = *src;
#endif
                }
                return NANORTC_OK;
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

        dtls_set_time(&rtc->dtls, rtc->now_ms);
        int drc = dtls_handle_data(&rtc->dtls, data, len);
        if (drc < 0) {
            return drc;
        }

        /* Drain DTLS output into transmit queue */
        int qrc = rtc_drain_dtls_output(rtc, src);
        if (qrc != NANORTC_OK && qrc != NANORTC_ERR_WOULD_BLOCK) {
            return qrc;
        }

        int complete_rc = rtc_finish_dtls(rtc, src);
        if (complete_rc != NANORTC_OK)
            return complete_rc;

#if NANORTC_FEATURE_DATACHANNEL
        /* If DTLS is established, check for decrypted app data → SCTP */
        if (rtc->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED) {
            const uint8_t *app_data = NULL;
            size_t app_len = 0;
            while (dtls_poll_app_data(&rtc->dtls, &app_data, &app_len) == NANORTC_OK &&
                   app_len > 0) {
                /* Feed decrypted data to SCTP */
                int sctp_rc = nsctp_handle_data(&rtc->sctp, app_data, app_len);
                if (sctp_rc != NANORTC_OK) {
                    return sctp_rc;
                }

                /* Check for SCTP state transition */
                if (rtc->sctp.state == NANORTC_SCTP_STATE_ESTABLISHED &&
                    rtc->state < NANORTC_STATE_CONNECTED) {
                    rtc->sctp.last_heartbeat_ms = rtc->now_ms;
                    rtc->state = NANORTC_STATE_CONNECTED;
                    rtc_emit_connected(rtc);
                    NANORTC_LOGI(&rtc->config.log, "RTC", "connected (SCTP established)");
                }

                /* Drain SCTP output (SACK, handshake) through DTLS */
                sctp_rc = rtc_pump_sctp_through_dtls(rtc, src);
                bool sctp_blocked = sctp_rc == NANORTC_ERR_WOULD_BLOCK;
                if (sctp_rc != NANORTC_OK && !sctp_blocked) {
                    return sctp_rc;
                }

                /* DCEP OPEN/ACK is retained until SCTP is established and has
                 * send capacity. A busy TX slot leaves the copied SCTP DATA
                 * pending for the timer pump instead of losing the DC output. */
                int dc_rc = rtc_pump_dc_through_sctp(rtc, src);
                bool dc_blocked = dc_rc == NANORTC_ERR_WOULD_BLOCK;
                if (dc_rc != NANORTC_OK && !dc_blocked) {
                    return dc_rc;
                }
                if (sctp_blocked || dc_blocked) {
                    return NANORTC_OK;
                }

                app_len = 0;
            }
        }
#endif /* NANORTC_FEATURE_DATACHANNEL */

        return NANORTC_OK;

    } else if (first >= 128 && first <= 191) {
        /* RTP/RTCP [0x80-0xBF] — RFC 7983 §3. Body lives in nano_rtc_media.c
         * (Phase 10 PR-4 slice 3); the dispatch arm here only routes. */
#if NANORTC_HAVE_MEDIA_TRANSPORT
        return nano_rtc_media_handle_rtp_or_rtcp(rtc, data, len);
#else
        return NANORTC_OK; /* No media transport compiled — silently discard */
#endif
    }

    return NANORTC_ERR_PROTOCOL; /* Unknown packet type */
}

/* ----------------------------------------------------------------
 * Timer processing (extracted from former nanortc_handle_timeout)
 * ---------------------------------------------------------------- */

static int rtc_process_timers(nanortc_t *rtc, uint32_t now_ms)
{
    nano_ice_check_t check;
    if (rtc_ice_get_check(rtc, now_ms, &check) == 0) {
        nano_ice_state_t prev_ice = rtc->ice.state;
        uint8_t local_before = check.local_idx;
        uint8_t remote_before = check.remote_idx;
        size_t out_len = 0;
        uint8_t *tx_buf = NULL;
        uint8_t tx_slot = 0;
        int rc = nano_rtc_tx_slot_acquire(rtc, &tx_buf, &tx_slot);
        if (rc == NANORTC_ERR_WOULD_BLOCK) {
            return NANORTC_OK; /* leave ICE deadline/state unchanged; drain then tick */
        }
        if (rc != NANORTC_OK) {
            return rc;
        }
        rc = ice_send_check(&rtc->ice, now_ms, &check, rtc->config.crypto, tx_buf,
                            NANORTC_TX_SLOT_SIZE, &out_len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        if (out_len > 0 && remote_before < rtc->ice.remote_candidate_count &&
            local_before < rtc->ice.local_candidate_count) {
            nanortc_addr_t dest;
            memset(&dest, 0, sizeof(dest));
            dest.family = rtc->ice.remote_candidates[remote_before].family;
            memcpy(dest.addr, rtc->ice.remote_candidates[remote_before].addr, NANORTC_ADDR_SIZE);
            dest.port = rtc->ice.remote_candidates[remote_before].port;
            nanortc_addr_t local_src;
            memset(&local_src, 0, sizeof(local_src));
            uint8_t base = ice_local_base_idx(&rtc->ice, local_before);
            if (base < rtc->ice.local_candidate_count) {
                local_src.family = rtc->ice.local_candidates[base].family;
                memcpy(local_src.addr, rtc->ice.local_candidates[base].addr, NANORTC_ADDR_SIZE);
                local_src.port = rtc->ice.local_candidates[base].port;
            }
#if NANORTC_FEATURE_TURN
            if (rtc->ice.local_candidates[local_before].type == NANORTC_ICE_CAND_RELAY) {
                rc = nano_rtc_tx_slot_commit(rtc, tx_slot, out_len, &dest, true);
            } else
#endif
            {
                rc = rtc_tx_slot_commit_direct(rtc, tx_slot, out_len, &dest, &local_src);
            }
            if (rc != NANORTC_OK) {
                return rc;
            }
        }

        /* Emit ICE_STATE_CHANGE on transition to CHECKING */
        if (rtc->ice.state != prev_ice && rtc->ice.state == NANORTC_ICE_STATE_CHECKING) {
            nanortc_event_t isce;
            memset(&isce, 0, sizeof(isce));
            isce.type = NANORTC_EV_ICE_STATE_CHANGE;
            isce.ice_state = (uint16_t)NANORTC_ICE_STATE_CHECKING;
            nano_rtc_emit_event_full(rtc, &isce);
        }

        /* Propagate ICE failure */
        if (rtc->ice.state == NANORTC_ICE_STATE_FAILED) {
            nanortc_event_t fice;
            memset(&fice, 0, sizeof(fice));
            fice.type = NANORTC_EV_ICE_STATE_CHANGE;
            fice.ice_state = (uint16_t)NANORTC_ICE_STATE_FAILED;
            nano_rtc_emit_event_full(rtc, &fice);
            rtc_close(rtc, NANORTC_DISCONNECT_ICE_FAILED);
            return NANORTC_OK;
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
            nano_rtc_emit_event_full(rtc, &dice);
            rtc_close(rtc, NANORTC_DISCONNECT_CONSENT_EXPIRED);
            return NANORTC_OK;
        } else if (rtc->ice.consent_next_ms == 0u ||
                   nano_time_is_due(now_ms, rtc->ice.consent_next_ms)) {
            /* Generate consent check if due */
            size_t consent_len = 0;
            uint8_t *tx_buf = NULL;
            uint8_t tx_slot = 0;
            int crc = nano_rtc_tx_slot_acquire(rtc, &tx_buf, &tx_slot);
            if (crc == NANORTC_ERR_WOULD_BLOCK) {
                return NANORTC_OK;
            }
            if (crc != NANORTC_OK) {
                return crc;
            }
            crc = ice_generate_consent(&rtc->ice, now_ms, rtc->config.crypto, tx_buf,
                                       NANORTC_TX_SLOT_SIZE, &consent_len);
            if (crc != NANORTC_OK) {
                return crc;
            }
            if (crc == NANORTC_OK && consent_len > 0) {
                /* Use nano_rtc_enqueue_transmit so the consent check is wrapped in
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
                crc = nano_rtc_tx_slot_commit(rtc, tx_slot, consent_len, &consent_dest, false);
                if (crc != NANORTC_OK) {
                    return crc;
                }
            }
        }
    }

#if NANORTC_FEATURE_TURN
    if (rtc->turn.configured) {
        uint8_t *tx_buf = NULL;
        uint8_t tx_slot = 0;
        int trc = nano_rtc_tx_slot_acquire(rtc, &tx_buf, &tx_slot);
        if (trc == NANORTC_ERR_WOULD_BLOCK)
            return NANORTC_OK;
        if (trc != NANORTC_OK)
            return trc;
        size_t len = 0;
        trc = turn_poll_output(&rtc->turn, now_ms, rtc->config.crypto, tx_buf, NANORTC_TX_SLOT_SIZE,
                               &len);
        if (trc != NANORTC_OK)
            return trc;
        if (len == 0u) {
            const nano_ice_candidate_t *candidate = rtc_turn_next_permission_candidate(rtc, now_ms);
            if (candidate != NULL)
                trc = turn_create_permission(&rtc->turn, candidate->addr, candidate->family,
                                             candidate->port, rtc->config.crypto, tx_buf,
                                             NANORTC_TX_SLOT_SIZE, &len);
            else if (rtc_turn_can_bind_channel(rtc))
                trc = turn_channel_bind(&rtc->turn, rtc->ice.selected_addr,
                                        rtc->ice.selected_family, rtc->ice.selected_port,
                                        rtc->config.crypto, tx_buf, NANORTC_TX_SLOT_SIZE, &len);
        }
        if (trc != NANORTC_OK)
            return trc;
        if (len > 0u) {
            nanortc_addr_t dest = {0};
            dest.family = rtc->turn.server_family;
            memcpy(dest.addr, rtc->turn.server_addr, NANORTC_ADDR_SIZE);
            dest.port = rtc->turn.server_port;
            trc = rtc_tx_slot_commit_direct(rtc, tx_slot, len, &dest, NULL);
            if (trc != NANORTC_OK)
                return trc;
        }
    }
#endif /* NANORTC_FEATURE_TURN */

    if (rtc->dtls.out_len > 0 && rtc->remote_addr.family != 0) {
        int drc = rtc_drain_dtls_output(rtc, &rtc->remote_addr);
        if (drc == NANORTC_ERR_WOULD_BLOCK) {
            return NANORTC_OK;
        }
        if (drc != NANORTC_OK) {
            return drc;
        }
    }

    if (rtc->state == NANORTC_STATE_DTLS_HANDSHAKING) {
        int rc = dtls_handle_timeout(&rtc->dtls, now_ms);
        if (rc != NANORTC_OK)
            return rc;
        rc = rtc_finish_dtls(rtc, &rtc->remote_addr);
        if (rc != NANORTC_OK)
            return rc;
        rc = rtc_drain_dtls_output(rtc, &rtc->remote_addr);
        if (rc != NANORTC_OK && rc != NANORTC_ERR_WOULD_BLOCK)
            return rc;
    }

    /* STUN: server-reflexive candidate discovery (RFC 8445 §5.1.1.1) */
    if (rtc->stun_server_configured && !rtc->srflx_discovered) {
        if (rtc->stun_retry_at_ms == 0u || nano_time_is_due(now_ms, rtc->stun_retry_at_ms)) {
            if (rtc->stun_retries < 3) {
                uint8_t *tx_buf = NULL;
                uint8_t tx_slot = 0;
                int src = nano_rtc_tx_slot_acquire(rtc, &tx_buf, &tx_slot);
                if (src == NANORTC_ERR_WOULD_BLOCK) {
                    return NANORTC_OK;
                }
                if (src != NANORTC_OK) {
                    return src;
                }
                /* Generate random txid on first attempt */
                uint8_t txid[STUN_TXID_SIZE];
                if (rtc->stun_retries == 0) {
                    if (rtc->config.crypto->random_bytes(txid, sizeof(txid)) != 0) {
                        return NANORTC_ERR_CRYPTO;
                    }
                } else {
                    memcpy(txid, rtc->stun_txid, sizeof(txid));
                }

                size_t req_len = 0;
                src = stun_encode_simple_binding_request(txid, tx_buf, NANORTC_TX_SLOT_SIZE,
                                                         &req_len);
                if (src != NANORTC_OK) {
                    return src;
                }
                if (src == NANORTC_OK && req_len > 0) {
                    nanortc_addr_t dest;
                    memset(&dest, 0, sizeof(dest));
                    dest.family = rtc->stun_server_family;
                    memcpy(dest.addr, rtc->stun_server_addr, NANORTC_ADDR_SIZE);
                    dest.port = rtc->stun_server_port;
                    nanortc_addr_t *local_src_p = NULL;
                    nanortc_addr_t local_src;
                    if (rtc->stun_retries == 0) {
                        rtc->ice.srflx_base_idx = NANORTC_ICE_LOCAL_IDX_UNKNOWN;
                        for (uint8_t i = 0; i < rtc->ice.local_candidate_count; i++) {
                            if (rtc->ice.local_candidates[i].family == dest.family &&
                                rtc->ice.local_candidates[i].type == NANORTC_ICE_CAND_HOST) {
                                rtc->ice.srflx_base_idx = i;
                                break;
                            }
                        }
                    }
                    uint8_t base = rtc->ice.srflx_base_idx;
                    if (base < rtc->ice.local_candidate_count) {
                        memset(&local_src, 0, sizeof(local_src));
                        local_src.family = rtc->ice.local_candidates[base].family;
                        memcpy(local_src.addr, rtc->ice.local_candidates[base].addr,
                               NANORTC_ADDR_SIZE);
                        local_src.port = rtc->ice.local_candidates[base].port;
                        local_src_p = &local_src;
                    }
                    src = rtc_tx_slot_commit_direct(rtc, tx_slot, req_len, &dest, local_src_p);
                    if (src != NANORTC_OK) {
                        return src;
                    }
                    memcpy(rtc->stun_txid, txid, sizeof(txid));
                }

                rtc->stun_retries++;
                rtc->stun_retry_at_ms = nano_time_deadline(now_ms, 500u); /* retry in 500ms */
            }
        }
    }

#if NANORTC_FEATURE_DATACHANNEL
    /* SCTP: retransmission + heartbeat timers */
    if (rtc->sctp.state != NANORTC_SCTP_STATE_CLOSED) {
        int src = nsctp_handle_timeout(&rtc->sctp, now_ms);
        if (src != NANORTC_OK && !rtc->sctp.closed_due_to_failure) {
            return src;
        }
    }

    /* Handshake responses can also remain queued when a one-slot TX ring was
     * occupied by the DTLS record processed in the same call. Retry every tick
     * for every live association state, then move any retained DCEP output. */
    if (rtc->sctp.state != NANORTC_SCTP_STATE_CLOSED &&
        rtc->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED) {
        int src = rtc_pump_sctp_through_dtls(rtc, &rtc->remote_addr);
        if (src != NANORTC_OK && src != NANORTC_ERR_WOULD_BLOCK) {
            return src;
        }
        src = rtc_pump_dc_through_sctp(rtc, &rtc->remote_addr);
        if (src != NANORTC_OK && src != NANORTC_ERR_WOULD_BLOCK) {
            return src;
        }
    }

    /* SCTP retransmit exhaustion → emit DISCONNECTED. Mirrors the ICE
     * failure path (lines 1744-1758): nsctp_handle_timeout() flips state
     * to CLOSED but cannot reach the event queue. Check the flag outside
     * the ESTABLISHED guard above — once the timeout fires, that guard
     * becomes false on the next tick and we'd otherwise lose the signal.
     * Clear on read so the event fires exactly once. */
    if (rtc->sctp.closed_due_to_failure) {
        rtc->sctp.closed_due_to_failure = false;
        rtc_close(rtc, NANORTC_DISCONNECT_SCTP_FAILED);
    }

#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* Periodic RTCP Sender Report (RFC 3550 §6.2) — body lives in
     * nano_rtc_media.c (Phase 10 PR-4 slice 3). */
    {
        int rrc = nano_rtc_media_emit_rtcp_sr_cadence(rtc, now_ms);
        if (rrc != NANORTC_OK && rrc != NANORTC_ERR_WOULD_BLOCK) {
            return rrc;
        }
    }
#endif

    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * nanortc_handle_input — unified input entry point
 * ---------------------------------------------------------------- */

/*
 * Resolve a local destination address to a local_candidates[] index.
 *
 * Two-pass match so a specific-typed candidate (e.g. srflx on the same
 * port as a wildcard host) wins over the wildcard:
 *
 *   1. Exact pass  — family + port + full addr match. This is the
 *      deterministic case; if any candidate stores a concrete address
 *      that equals dst, pick it first.
 *   2. Wildcard pass — family + port, with the candidate's stored addr
 *      being all-zeros (INADDR_ANY / IN6ADDR_ANY from a wildcard bind).
 *      Used as a fallback on single-socket setups where the registered
 *      host candidate has no specific IP.
 *
 * Returns NANORTC_ICE_LOCAL_IDX_UNKNOWN when no candidate matches;
 * ice_handle_stun then falls back to idx 0 for legacy behaviour.
 */
static uint8_t rtc_resolve_local_idx(const nanortc_t *rtc, const nanortc_addr_t *dst)
{
    if (!dst || dst->family == 0) {
        return NANORTC_ICE_LOCAL_IDX_UNKNOWN;
    }
    /* Pass 1: exact family + port + addr match. */
    for (uint8_t i = 0; i < rtc->ice.local_candidate_count; i++) {
        const nano_ice_candidate_t *c = &rtc->ice.local_candidates[i];
        if (c->family != dst->family || c->port != dst->port) {
            continue;
        }
        if (memcmp(c->addr, dst->addr, NANORTC_ADDR_SIZE) == 0) {
            return i;
        }
    }
    /* Pass 2: wildcard (all-zero addr) fallback. */
    for (uint8_t i = 0; i < rtc->ice.local_candidate_count; i++) {
        const nano_ice_candidate_t *c = &rtc->ice.local_candidates[i];
        if (c->family != dst->family || c->port != dst->port) {
            continue;
        }
        bool addr_wild = true;
        for (size_t k = 0; k < NANORTC_ADDR_SIZE; k++) {
            if (c->addr[k] != 0) {
                addr_wild = false;
                break;
            }
        }
        if (addr_wild) {
            return i;
        }
    }
    return NANORTC_ICE_LOCAL_IDX_UNKNOWN;
}

int nanortc_handle_input(nanortc_t *rtc, const nanortc_input_t *in)
{
    if (!rtc || !in) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (rtc->state == NANORTC_STATE_CLOSED)
        return NANORTC_ERR_STATE;
    rtc->now_ms = in->now_ms;
#if NANORTC_FEATURE_DATACHANNEL
    rtc->sctp.now_ms = in->now_ms;
#endif
    dtls_set_time(&rtc->dtls, in->now_ms);

    /* Always process timers (ICE checks, SCTP retransmits) */
    int trc = rtc_process_timers(rtc, in->now_ms);
    if (trc != NANORTC_OK) {
        NANORTC_LOGW(&rtc->config.log, "RTC", "protocol timer failed");
        if (rtc->dtls.state == NANORTC_DTLS_STATE_ERROR)
            rtc_close(rtc, NANORTC_DISCONNECT_AUTH_FAILED);
        return trc;
    }
    if (rtc->state == NANORTC_STATE_CLOSED)
        return NANORTC_OK;

    /* If packet data provided, process the incoming UDP packet. family==0
     * on src is treated as "no packet" (timer-only tick). family==0 on dst
     * means the caller can't identify the local socket — equivalent to
     * passing dst=NULL in the previous API. */
    if (in->data && in->len > 0 && in->src.family != 0) {
        const nanortc_addr_t *dst_p = (in->dst.family != 0) ? &in->dst : NULL;
        uint8_t local_idx = rtc_resolve_local_idx(rtc, dst_p);
        int rc = rtc_process_receive(rtc, in->data, in->len, &in->src, local_idx, false);
        if (rc != NANORTC_OK) {
            NANORTC_LOGW(&rtc->config.log, "RTC", "input rejected");
            if (rtc->dtls.state == NANORTC_DTLS_STATE_ERROR)
                rtc_close(rtc, NANORTC_DISCONNECT_AUTH_FAILED);
        } else if (rtc->dtls.state == NANORTC_DTLS_STATE_CLOSED) {
            rtc_close(rtc, NANORTC_DISCONNECT_PEER);
        }
        return rc;
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

    if (rtc->state == NANORTC_STATE_CLOSED)
        return NANORTC_ERR_STATE;

    bool partial = options && (options->partial_reliability || options->max_retransmits != 0);
    if (partial && !NANORTC_FEATURE_DC_RELIABLE)
        return NANORTC_ERR_NOT_IMPLEMENTED;

    if (!rtc->sdp.has_datachannel && rtc->signaling_state != NANORTC_SIGNALING_READY)
        return NANORTC_ERR_NOT_IMPLEMENTED;

    bool ordered = options ? !options->unordered : true;
    uint16_t max_rexmit = options ? options->max_retransmits : 0;

    uint16_t sid = rtc_alloc_stream_id(rtc);
    int rc = dc_open_options(&rtc->datachannel, sid, label, options ? options->protocol : NULL,
                             ordered, partial, max_rexmit);
    if (rc != NANORTC_OK) {
        return rc;
    }

    if (!rtc->sdp.has_datachannel) {
        rtc->sdp.has_datachannel = true;
        rtc->sdp.dc_mid = rtc->sdp.mid_count++;
    }

    /* If already connected, drain DCEP OPEN through SCTP→DTLS. The helper
     * retains it on backpressure for the next timer tick. */
    if (rtc->state == NANORTC_STATE_CONNECTED) {
        (void)rtc_pump_dc_through_sctp(rtc, &rtc->remote_addr);
    }

    NANORTC_LOGI(&rtc->config.log, "RTC", "datachannel created");
    return (int)sid;
}

static int rtc_datachannel_send(nanortc_t *rtc, uint16_t id, const void *data, size_t len,
                                bool binary)
{
    if (!rtc || !data)
        return NANORTC_ERR_INVALID_PARAM;
    if (rtc->state != NANORTC_STATE_CONNECTED)
        return NANORTC_ERR_STATE;
    nano_dc_channel_t *ch = dc_find_channel(&rtc->datachannel, id);
    if (!ch || ch->state != NANORTC_DC_STATE_OPEN)
        return NANORTC_ERR_STATE;
    uint32_t ppid = binary ? (len ? DCEP_PPID_BINARY : DCEP_PPID_BINARY_EMPTY)
                           : (len ? DCEP_PPID_STRING : DCEP_PPID_STRING_EMPTY);
    /* RFC 8831 §6.6: an empty message has a distinct PPID and one ignored byte. */
    static const uint8_t empty = 0;
    return nsctp_send_options(
        &rtc->sctp, id, ppid, len ? (const uint8_t *)data : &empty, len ? len : 1, !ch->ordered,
        (ch->channel_type & 0x7f) == DCEP_CHANNEL_REXMIT ? ch->max_retransmits : -1);
}

int nanortc_datachannel_send(nanortc_t *rtc, uint16_t id, const void *data, size_t len)
{
    return rtc_datachannel_send(rtc, id, data, len, true);
}

int nanortc_datachannel_send_text(nanortc_t *rtc, uint16_t id, const char *str, size_t len)
{
    return rtc_datachannel_send(rtc, id, str, len, false);
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
    return nanortc_datachannel_send_text(rtc, id, str, len);
}

int nanortc_datachannel_close(nanortc_t *rtc, uint16_t id)
{
    if (!rtc || !dc_find_channel(&rtc->datachannel, id))
        return NANORTC_ERR_INVALID_PARAM;
    if (rtc->state == NANORTC_STATE_CLOSED)
        return NANORTC_ERR_STATE;
    /* RFC 8831 §6.7: closing requires SCTP stream reset (not implemented). */
    return NANORTC_ERR_NOT_IMPLEMENTED;
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

int nanortc_ice_restart(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (rtc->state == NANORTC_STATE_CLOSED)
        return NANORTC_ERR_STATE;
    if (rtc->dtls.state == NANORTC_DTLS_STATE_HANDSHAKING)
        return NANORTC_ERR_STATE;
    if (rtc->transmit_pending)
        return NANORTC_ERR_WOULD_BLOCK;
    /* Generate the new credentials before touching live connection state. A
     * fail-on-Nth provider therefore leaves the existing session intact. */
    uint8_t ufrag_bytes[NANORTC_ICE_UFRAG_LEN / 2];
    uint8_t pwd_bytes[NANORTC_ICE_PWD_LEN / 2];
    if (rtc->config.crypto->random_bytes(ufrag_bytes, sizeof(ufrag_bytes)) != 0 ||
        rtc->config.crypto->random_bytes(pwd_bytes, sizeof(pwd_bytes)) != 0) {
        return NANORTC_ERR_CRYPTO;
    }
    char new_ufrag[NANORTC_ICE_UFRAG_SIZE];
    char new_pwd[NANORTC_ICE_PWD_SIZE];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(ufrag_bytes); i++) {
        new_ufrag[i * 2u] = hex[ufrag_bytes[i] >> 4];
        new_ufrag[i * 2u + 1u] = hex[ufrag_bytes[i] & 0x0Fu];
    }
    new_ufrag[NANORTC_ICE_UFRAG_LEN] = '\0';
    for (size_t i = 0; i < sizeof(pwd_bytes); i++) {
        new_pwd[i * 2u] = hex[pwd_bytes[i] >> 4];
        new_pwd[i * 2u + 1u] = hex[pwd_bytes[i] & 0x0Fu];
    }
    new_pwd[NANORTC_ICE_PWD_LEN] = '\0';

    NANORTC_LOGI(&rtc->config.log, "RTC", "ICE restart");

    /* RFC 8842 §6: an ICE restart retains the authenticated association. */
    nanortc_addr_t old_local = {.family = rtc->ice.selected_local_family,
                                .port = rtc->ice.selected_local_port};
    memcpy(old_local.addr, rtc->ice.selected_local_addr, NANORTC_ADDR_SIZE);
    uint32_t old_consent_expiry = rtc->ice.consent_expiry_ms;
    uint8_t old_local_type = rtc->ice.selected_local_type;
    uint8_t old_remote_type = rtc->ice.selected_type;
    uint8_t old_local_idx = rtc->ice.selected_local_idx;

    /* Reset ICE state (preserves role + tie_breaker, bumps generation) */
    int rc = ice_restart(&rtc->ice);
    if (rc != NANORTC_OK) {
        return rc;
    }
    memset(rtc->remote_candidate_related, 0, sizeof(rtc->remote_candidate_related));
#if NANORTC_FEATURE_TURN
    /* An established association can use the old relay until consent expires.
     * Its allocation and existing permissions are independent of ICE generation. */
    if (rtc->dtls.state != NANORTC_DTLS_STATE_ESTABLISHED)
        turn_reset_peer_state(&rtc->turn);
#endif

    memcpy(rtc->ice.local_ufrag, new_ufrag, sizeof(new_ufrag));
    rtc->ice.local_ufrag_len = NANORTC_ICE_UFRAG_LEN;
    memcpy(rtc->ice.local_pwd, new_pwd, sizeof(new_pwd));
    rtc->ice.local_pwd_len = NANORTC_ICE_PWD_LEN;

    /* Copy new credentials into SDP state */
    memcpy(rtc->sdp.local_ufrag, rtc->ice.local_ufrag, NANORTC_ICE_UFRAG_SIZE);
    memcpy(rtc->sdp.local_pwd, rtc->ice.local_pwd, NANORTC_ICE_PWD_SIZE);

    /* Reset SDP candidate state */
    rtc->sdp.candidate_count = 0;
    rtc->sdp.end_of_candidates = false;

    rtc->signaling_state = NANORTC_SIGNALING_READY;
    rtc->ice_restarting = rtc->dtls.state == NANORTC_DTLS_STATE_ESTABLISHED;
    if (rtc->ice_restarting) {
        /* RFC 8445 §9 / RFC 7675 §5: retain the selected route only while
         * its existing consent remains valid; new checks use new credentials. */
        memcpy(rtc->ice.selected_addr, rtc->remote_addr.addr, NANORTC_ADDR_SIZE);
        rtc->ice.selected_port = rtc->remote_addr.port;
        rtc->ice.selected_family = rtc->remote_addr.family;
        rtc->ice.selected_type = old_remote_type;
        memcpy(rtc->ice.selected_local_addr, old_local.addr, NANORTC_ADDR_SIZE);
        rtc->ice.selected_local_port = old_local.port;
        rtc->ice.selected_local_family = old_local.family;
        rtc->ice.selected_local_type = old_local_type;
        rtc->ice.selected_local_idx = old_local_idx;
        rtc->ice.consent_expiry_ms = old_consent_expiry;
    }
    if (!rtc->ice_restarting)
        rtc->state = NANORTC_STATE_NEW;

    /* Emit ICE state change */
    nanortc_event_t ice_evt;
    memset(&ice_evt, 0, sizeof(ice_evt));
    ice_evt.type = NANORTC_EV_ICE_STATE_CHANGE;
    ice_evt.ice_state = (uint16_t)NANORTC_ICE_STATE_NEW;
    nano_rtc_emit_event_full(rtc, &ice_evt);

    return NANORTC_OK;
}

void nanortc_disconnect(nanortc_t *rtc)
{
    if (!rtc) {
        return;
    }
    rtc_close(rtc, NANORTC_DISCONNECT_LOCAL);
}

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
