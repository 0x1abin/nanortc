/*
 * nanortc — Internal helpers shared across the rtc orchestration files.
 *
 * Not part of the public API. Lives under src/ specifically so that
 * <include/> stays untouched. The split exists because the offer/answer
 * + ICE-server entry surface (nano_rtc_negotiate.c) is a cleanly
 * separable contour from the receive/timer backbone in nano_rtc.c, but
 * the two units still share a handful of small helpers — listed here.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_RTC_INTERNAL_H_
#define NANORTC_RTC_INTERNAL_H_

#include "nanortc.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Emit a typed event into the output queue. Defined in nano_rtc.c next
 * to the rest of the queue/event machinery; declared here so that the
 * offer/answer file can attach ICE-candidate / media-added events to
 * the same queue without re-implementing the helper.
 */
int nano_rtc_emit_event_full(nanortc_t *rtc, const nanortc_event_t *event);

/**
 * Format an RFC 8839 §5.1 SDP candidate line ("candidate:<f> 1 UDP <p>
 * <ip> <port> typ <type>") into @p buf and NUL-terminate. Defined in
 * nano_rtc_negotiate.c; declared here so rtc_process_receive() (in
 * nano_rtc.c) can format trickle-style srflx and relay candidate strings
 * after STUN srflx discovery / TURN allocation.
 *
 * @return Length of the produced string (excluding the NUL terminator).
 */
size_t nano_rtc_build_candidate_str(char *buf, uint16_t foundation, uint32_t priority,
                                    const char *ip, size_t ip_len, uint16_t port, const char *type,
                                    size_t type_len);

/**
 * Emit a NANORTC_EV_ICE_CANDIDATE event carrying the trickle string.
 * Caller owns the lifetime of @p candidate_str through the next
 * nanortc_poll_output() call (matches the pointer-lifetime contract on
 * `nanortc_output_t`). Defined in nano_rtc_negotiate.c.
 */
void nano_rtc_emit_ice_candidate(nanortc_t *rtc, const char *candidate_str);

/**
 * Cache the local DTLS fingerprint with the "sha-256 " prefix into
 * `sdp.local_fingerprint` (RFC 8122 §5). Idempotent — the first
 * non-empty cache wins so callers can invoke this cheaply from multiple
 * places (early DTLS init, post-handshake completion). Defined in
 * nano_rtc_negotiate.c.
 */
void nano_rtc_cache_fingerprint(nanortc_t *rtc);

/**
 * Apply a WebRTC-style iceServers array (RFC 7064 / RFC 7065 URL form)
 * to the rtc instance: configures the first STUN URL for srflx
 * discovery and, when the TURN feature is enabled, the first TURN URL
 * for relay allocation. Defined in nano_rtc_negotiate.c; called from
 * nanortc_init() in nano_rtc.c so cfg-supplied servers are wired up
 * during construction.
 */
int nano_rtc_apply_ice_servers(nanortc_t *rtc, const nanortc_ice_server_t *servers, size_t count);

/**
 * Enqueue a NANORTC_OUTPUT_TRANSMIT entry into rtc->out_queue. Used by
 * every tx producer (DTLS, STUN, RTP/SRTP, RTCP feedback, video pkt_ring,
 * audio packetizer). Defined in nano_rtc.c next to the output-queue
 * machinery; declared here so nano_rtc_media.c can enqueue audio/video
 * and RTCP feedback packets without re-implementing the lazy TURN wrap
 * path or the lifetime contract documented on `nanortc_output_t`
 * (`include/nanortc.h`).
 *
 * @p data must satisfy the lifetime contract — valid until the next
 * `nanortc_poll_output()` / `nanortc_handle_input()` / `nanortc_destroy()`
 * on the same nanortc_t. @p force_via_turn is the RFC 8445 §7.2.2
 * symmetric-path override (only meaningful for STUN responses received
 * via a TURN unwrap); all other callers pass false.
 *
 * @return NANORTC_OK, or NANORTC_ERR_BUFFER_TOO_SMALL when the queue is
 * full (also bumps `rtc->stats_tx_queue_full`).
 */
int nano_rtc_enqueue_transmit(nanortc_t *rtc, const uint8_t *data, size_t len,
                              const nanortc_addr_t *peer_dest, bool force_via_turn);

#if NANORTC_HAVE_MEDIA_TRANSPORT
/**
 * Handle an SRTP-protected RTP or RTCP datagram. Mirrors the RFC 7983 §3
 * dispatch arm for first-byte [0x80-0xBF] and performs the RFC 5761 §4
 * RTP/RTCP discrimination internally. Defined in nano_rtc_media.c so the
 * receive backbone in nano_rtc.c stays free of media-specific state
 * (jitter buffer, depacketizer, BWE feedback, NACK retx from pkt_ring).
 *
 * Return contract matches `rtc_process_receive()`: NANORTC_OK on success
 * or any silent-discard path (bad SRTP, unknown SSRC, malformed RTP);
 * NANORTC_ERR_PARSE / NANORTC_ERR_BUFFER_TOO_SMALL on hard failure.
 */
int nano_rtc_media_handle_rtp_or_rtcp(nanortc_t *rtc, const uint8_t *data, size_t len);

/**
 * Periodic RTCP Sender Report emission (RFC 3550 §6.2). Called once per
 * timer tick from `rtc_process_timers()`; cadence-gates on
 * `rtc->last_rtcp_send_ms` and `NANORTC_RTCP_INTERVAL_MS` internally and
 * is a no-op when `srtp.ready` is false. Defined in nano_rtc_media.c.
 */
void nano_rtc_media_emit_rtcp_sr_cadence(nanortc_t *rtc, uint32_t now_ms);
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_RTC_INTERNAL_H_ */
