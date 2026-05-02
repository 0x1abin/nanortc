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
int nano_rtc_enqueue_transmit(nanortc_t *rtc, const uint8_t *data, size_t len,
                              const nanortc_addr_t *peer_dest, bool force_via_turn)
{
    /* Lifetime contract (see nanortc.h `nanortc_output_t`): @p data must
     * remain valid until the next nanortc_poll_output() / handle_input() /
     * destroy() call on this nanortc_t. Producers either point into a
     * caller-owned static scratch (DTLS, STUN, SRTP/RTP) or a managed slot
     * (video pkt_ring) — none of them copy here. */
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
        NANORTC_LOGW("RTC", "tx queue full, dropping output");
        return rc;
    }

#if NANORTC_FEATURE_TURN
    /* Wrap decision:
     *   - selected_type == RELAY: steady-state once USE-CANDIDATE nominated
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
    if (rtc->turn.configured && rtc->turn.state == NANORTC_TURN_ALLOCATED &&
        (__atomic_load_n(&rtc->ice.selected_type, __ATOMIC_RELAXED) == NANORTC_ICE_CAND_RELAY ||
         force_via_turn)) {
        rtc->out_wrap_meta[slot].via_turn = true;
        rtc->out_wrap_meta[slot].peer_dest = *peer_dest;
        __atomic_fetch_add(&rtc->stats_enqueue_via_turn, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&rtc->stats_enqueue_direct, 1, __ATOMIC_RELAXED);
    }
#else
    (void)force_via_turn;
#endif

    return NANORTC_OK;
}

/* A3: Drain DTLS output into the transmit queue (uses relay wrapping if needed) */
static void rtc_drain_dtls_output(nanortc_t *rtc, const nanortc_addr_t *dest)
{
    size_t dout_len = 0;
    while (dtls_poll_output(&rtc->dtls, rtc->dtls_scratch, sizeof(rtc->dtls_scratch), &dout_len) ==
               NANORTC_OK &&
           dout_len > 0) {
        nano_rtc_enqueue_transmit(rtc, rtc->dtls_scratch, dout_len, dest, false);
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

/* Emit a typed event with full event struct.
 * Non-static so nano_negotiate.c can call it via nano_rtc_internal.h. */
int nano_rtc_emit_event_full(nanortc_t *rtc, const nanortc_event_t *event)
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

    return nano_rtc_emit_event_full(rtc, &event);
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
    /* RFC 8445 §5.2: tie-breaker MUST be a cryptographically random 64-bit
     * value. It feeds ICE-CONTROLLING / ICE-CONTROLLED on every outgoing
     * check and resolves 487 Role Conflict; a fixed zero is both non-RFC
     * and predictable. Fill from the crypto provider now; ice_init() cannot
     * do this itself because it has no access to cfg->crypto. */
    if (cfg->crypto && cfg->crypto->random_bytes) {
        (void)cfg->crypto->random_bytes((uint8_t *)&rtc->ice.tie_breaker,
                                        sizeof(rtc->ice.tie_breaker));
    }
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
    NANORTC_LOGI("RTC", "nanortc_destroy");
    dtls_destroy(&rtc->dtls);
    nano_log_cleanup();
    rtc->state = NANORTC_STATE_CLOSED;
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
                __atomic_fetch_add(&rtc->stats_wrap_dropped, 1, __ATOMIC_RELAXED);
                NANORTC_LOGW("TURN", "lazy wrap failed, dropping output");
                rtc->out_head++;
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
        }
#endif /* NANORTC_FEATURE_TURN */

        rtc->out_head++;
        return NANORTC_OK;
    }
    return NANORTC_ERR_NO_DATA;
}

/* ----------------------------------------------------------------
 * nanortc_next_timeout_ms — deadline aggregator
 * ----------------------------------------------------------------
 * Reads the per-subsystem deadline accessors (ICE, TURN, SCTP) plus
 * the rtc-owned STUN-srflx retry and RTCP-SR cadence. Cap at MIN_POLL
 * during DTLS handshake because mbedtls / wolfssl drive their own
 * retransmit clock internally and do not surface a deadline. UINT32_MAX
 * "no deadline armed" is mapped to a 1-second idle cap so callers
 * never sleep indefinitely on a fully idle but still-alive connection.
 */
#define NANORTC_TIMEOUT_IDLE_CAP_MS 1000u

int nanortc_next_timeout_ms(const nanortc_t *rtc, uint32_t now_ms, uint32_t *out_ms)
{
    if (!rtc || !out_ms) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    uint32_t best = UINT32_MAX;

    /* ICE: connectivity checks + consent freshness + consent expiry. */
    {
        uint32_t d = ice_next_timeout_ms(&rtc->ice, now_ms);
        if (d < best) {
            best = d;
        }
    }

    /* STUN srflx retry (RFC 8445 §5.1.1.1). */
    if (rtc->stun_server_configured && !rtc->srflx_discovered && rtc->stun_retries < 3) {
        if (rtc->stun_retry_at_ms == 0) {
            best = 0;
        } else {
            uint32_t left =
                (now_ms >= rtc->stun_retry_at_ms) ? 0u : (rtc->stun_retry_at_ms - now_ms);
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
    }
#endif

#if NANORTC_FEATURE_DATACHANNEL
    {
        uint32_t d = nsctp_next_timeout_ms(&rtc->sctp, now_ms);
        if (d < best) {
            best = d;
        }
    }
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* RTCP Sender Report cadence (RFC 3550 §6.2). Only armed once SRTP
     * keys are derived — before that, the RTCP block in rtc_process_timers
     * is a no-op. */
    if (rtc->srtp.ready) {
        uint32_t since = now_ms - rtc->last_rtcp_send_ms;
        uint32_t left =
            (since >= NANORTC_RTCP_INTERVAL_MS) ? 0u : (NANORTC_RTCP_INTERVAL_MS - since);
        if (left < best) {
            best = left;
        }
    }
#endif

    /* DTLS handshake retransmit is owned by the crypto provider and not
     * surfaced as a deadline. Cap to MIN_POLL so retransmits still get
     * a chance to fire while the handshake progresses. */
    if (rtc->state == NANORTC_STATE_DTLS_HANDSHAKING) {
        if (best > NANORTC_MIN_POLL_INTERVAL_MS) {
            best = NANORTC_MIN_POLL_INTERVAL_MS;
        }
    }

    /* Idle cap: if nothing armed a deadline, return the conservative
     * default so callers don't block forever on a fully idle session. */
    if (best == UINT32_MAX) {
        best = NANORTC_TIMEOUT_IDLE_CAP_MS;
    }

    *out_ms = best;
    return NANORTC_OK;
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

    if (!is_server) {
        int rc = dtls_start(&rtc->dtls);
        if (rc != NANORTC_OK)
            return rc;
    }

    rtc->state = NANORTC_STATE_DTLS_HANDSHAKING;

    if (!is_server) {
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
        nano_rtc_emit_event_full(rtc, &oevt);
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
        nano_rtc_emit_event_full(rtc, &devt);
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
            nano_rtc_enqueue_transmit(rtc, rtc->dtls_scratch, enc_len, dest, false);
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
                /* See ChannelData branch above for local_idx rationale. */
                return rtc_process_receive(rtc, payload, payload_len, &peer_src, local_idx, true);
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
                nano_rtc_build_candidate_str(rtc->relay_cand_str, 2, 16777215,
                                             rtc->sdp.relay_candidate_ip, ip_len,
                                             rtc->turn.relay_port, "relay", 5);
                nano_rtc_emit_ice_candidate(rtc, rtc->relay_cand_str);

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
                    nano_rtc_build_candidate_str(rtc->srflx_cand_str, 3, 1090519295,
                                                 rtc->sdp.srflx_candidate_ip, ip_len,
                                                 smsg.mapped_port, "srflx", 5);
                    nano_rtc_emit_ice_candidate(rtc, rtc->srflx_cand_str);

                    NANORTC_LOGI("RTC", "srflx candidate discovered");

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
        size_t resp_len = 0;
        int rc = ice_handle_stun(&rtc->ice, data, len, src, local_idx, via_turn, rtc->config.crypto,
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

        /* Enqueue STUN response for transmission. Use nano_rtc_enqueue_transmit so
         * that if `src` is a peer reached via the TURN relay (the recursive
         * call from turn_unwrap_data / turn_unwrap_channel_data hands us the
         * inner peer address, not the TURN server), the response is wrapped
         * in a Send indication / ChannelData and routed back through the
         * relay. Direct sendto() on a NAT'd cellular peer address has no
         * route from the device. */
        if (resp_len > 0) {
            /* RFC 8445 §7.2.2: response follows the request's arrival path.
             * If the Binding Request came in via a TURN unwrap (via_turn),
             * the response must also go back through the relay — even before
             * USE-CANDIDATE has flipped selected_type. Without this, the
             * first pre-nomination responses leak direct and the peer ICE
             * stack builds a prflx direct candidate on loopback / LAN. */
            nano_rtc_enqueue_transmit(rtc, rtc->stun_buf, resp_len, src, via_turn);
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
                nano_rtc_emit_event_full(rtc, &ice_evt);
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

            nano_rtc_cache_fingerprint(rtc);

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
            nano_rtc_emit_event_full(rtc, &isce);
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
            nano_rtc_emit_event_full(rtc, &fice);
            /* TD-018: emit DISCONNECTED symmetric to the consent-expiry path
             * so applications see a consistent signal across both failure
             * modes (ICE check exhaustion and consent loss). */
            nanortc_event_t fdev;
            memset(&fdev, 0, sizeof(fdev));
            fdev.type = NANORTC_EV_DISCONNECTED;
            nano_rtc_emit_event_full(rtc, &fdev);
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
            nano_rtc_emit_event_full(rtc, &dice);
            /* Emit DISCONNECTED application event */
            nanortc_event_t dev;
            memset(&dev, 0, sizeof(dev));
            dev.type = NANORTC_EV_DISCONNECTED;
            nano_rtc_emit_event_full(rtc, &dev);
            rtc->state = NANORTC_STATE_CLOSED;
        } else {
            /* Generate consent check if due */
            size_t consent_len = 0;
            int crc = ice_generate_consent(&rtc->ice, now_ms, rtc->config.crypto, rtc->stun_buf,
                                           sizeof(rtc->stun_buf), &consent_len);
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
                nano_rtc_enqueue_transmit(rtc, rtc->stun_buf, consent_len, &consent_dest, false);
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
         * Request" packet-level bug on a downstream camera SDK. */
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

    /* SCTP retransmit exhaustion → emit DISCONNECTED. Mirrors the ICE
     * failure path (lines 1744-1758): nsctp_handle_timeout() flips state
     * to CLOSED but cannot reach the event queue. Check the flag outside
     * the ESTABLISHED guard above — once the timeout fires, that guard
     * becomes false on the next tick and we'd otherwise lose the signal.
     * Clear on read so the event fires exactly once. */
    if (rtc->sctp.closed_due_to_failure) {
        rtc->sctp.closed_due_to_failure = false;
        if (rtc->state == NANORTC_STATE_CONNECTED) {
            nanortc_event_t fdev;
            memset(&fdev, 0, sizeof(fdev));
            fdev.type = NANORTC_EV_DISCONNECTED;
            nano_rtc_emit_event_full(rtc, &fdev);
            rtc->state = NANORTC_STATE_CLOSED;
            NANORTC_LOGW("RTC", "SCTP retransmit exhaustion → DISCONNECTED");
        }
    }
#endif

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* Periodic RTCP Sender Report (RFC 3550 §6.2) — body lives in
     * nano_rtc_media.c (Phase 10 PR-4 slice 3). */
    nano_rtc_media_emit_rtcp_sr_cadence(rtc, now_ms);
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

    rtc->now_ms = in->now_ms;

    /* Always process timers (ICE checks, SCTP retransmits) */
    int trc = rtc_process_timers(rtc, in->now_ms);
    if (trc != NANORTC_OK) {
        return trc;
    }

    /* If packet data provided, process the incoming UDP packet. family==0
     * on src is treated as "no packet" (timer-only tick). family==0 on dst
     * means the caller can't identify the local socket — equivalent to
     * passing dst=NULL in the previous API. */
    if (in->data && in->len > 0 && in->src.family != 0) {
        const nanortc_addr_t *dst_p = (in->dst.family != 0) ? &in->dst : NULL;
        uint8_t local_idx = rtc_resolve_local_idx(rtc, dst_p);
        return rtc_process_receive(rtc, in->data, in->len, &in->src, local_idx, false);
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
    nano_rtc_emit_event_full(rtc, &cevt);

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

int nanortc_ice_restart(nanortc_t *rtc)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    NANORTC_LOGI("RTC", "ICE restart");

    /* Tear down the DTLS context so the next accept_offer/create_offer
     * re-initialises it with a fresh cert and BIO. Without this, the
     * `if (!rtc->dtls.crypto_ctx)` guard in rtc_begin_dtls_handshake
     * would skip dtls_init and reuse the previous DTLS state across the
     * ICE restart — possible key-material reuse and orphan handshake
     * timers. dtls_destroy is a no-op when crypto_ctx is NULL. */
    dtls_destroy(&rtc->dtls);

    /* Invalidate cached fingerprints tied to the destroyed DTLS identity.
     * nano_rtc_cache_fingerprint() is a write-once cache that bails when
     * sdp.local_fingerprint is non-empty; without this clear, the next
     * create_offer/accept_offer would advertise the *previous* cert's
     * fingerprint while dtls_init() generates a new one — DTLS verify
     * would then fail on the peer side. */
    memset(rtc->sdp.local_fingerprint, 0, sizeof(rtc->sdp.local_fingerprint));
    memset(rtc->dtls.local_fingerprint, 0, sizeof(rtc->dtls.local_fingerprint));

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
    nano_rtc_emit_event_full(rtc, &ice_evt);

    return NANORTC_OK;
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
