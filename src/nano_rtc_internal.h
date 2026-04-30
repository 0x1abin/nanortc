/*
 * nanortc — Internal helpers shared across the rtc orchestration files.
 *
 * Not part of the public API. Lives under src/ specifically so that
 * <include/> stays untouched. The split exists because the offer/answer
 * + ICE-server entry surface (nano_negotiate.c) is a cleanly
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
int rtc_emit_event_full(nanortc_t *rtc, const nanortc_event_t *event);

/**
 * Format an RFC 8839 §5.1 SDP candidate line ("candidate:<f> 1 UDP <p>
 * <ip> <port> typ <type>") into @p buf and NUL-terminate. Defined in
 * nano_negotiate.c; declared here so rtc_process_receive() (in
 * nano_rtc.c) can format trickle-style srflx and relay candidate strings
 * after STUN srflx discovery / TURN allocation.
 *
 * @return Length of the produced string (excluding the NUL terminator).
 */
size_t rtc_build_candidate_str(char *buf, uint16_t foundation, uint32_t priority, const char *ip,
                               size_t ip_len, uint16_t port, const char *type, size_t type_len);

/**
 * Emit a NANORTC_EV_ICE_CANDIDATE event carrying the trickle string.
 * Caller owns the lifetime of @p candidate_str through the next
 * nanortc_poll_output() call (matches the pointer-lifetime contract on
 * `nanortc_output_t`). Defined in nano_negotiate.c.
 */
void rtc_emit_ice_candidate(nanortc_t *rtc, const char *candidate_str);

/**
 * Cache the local DTLS fingerprint with the "sha-256 " prefix into
 * `sdp.local_fingerprint` (RFC 8122 §5). Idempotent — the first
 * non-empty cache wins so callers can invoke this cheaply from multiple
 * places (early DTLS init, post-handshake completion). Defined in
 * nano_negotiate.c.
 */
void rtc_cache_fingerprint(nanortc_t *rtc);

/**
 * Apply a WebRTC-style iceServers array (RFC 7064 / RFC 7065 URL form)
 * to the rtc instance: configures the first STUN URL for srflx
 * discovery and, when the TURN feature is enabled, the first TURN URL
 * for relay allocation. Defined in nano_negotiate.c; called from
 * nanortc_init() in nano_rtc.c so cfg-supplied servers are wired up
 * during construction.
 */
int rtc_apply_ice_servers(nanortc_t *rtc, const nanortc_ice_server_t *servers, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_RTC_INTERNAL_H_ */
