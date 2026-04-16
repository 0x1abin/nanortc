/*
 * nanortc — Bandwidth estimation internal interface (MEDIA profile)
 *
 * Implements REMB (Receiver Estimated Maximum Bitrate) parsing
 * from RTCP PSFB feedback (draft-alvestrand-rmcat-remb-03).
 *
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_BWE_H_
#define NANORTC_BWE_H_

#include <stdint.h>
#include <stddef.h>
#include "nanortc_config.h"

/* BWE configurable limits */

/** @brief Minimum bitrate (bps). Default 30 kbps. */
#ifndef NANORTC_BWE_MIN_BITRATE
#define NANORTC_BWE_MIN_BITRATE 30000
#endif

/** @brief Maximum bitrate (bps). Default 2 Mbps. */
#ifndef NANORTC_BWE_MAX_BITRATE
#define NANORTC_BWE_MAX_BITRATE 2000000
#endif

/** @brief Initial bitrate (bps). Default 300 kbps. */
#ifndef NANORTC_BWE_INITIAL_BITRATE
#define NANORTC_BWE_INITIAL_BITRATE 300000
#endif

/** @brief EMA smoothing factor numerator (out of 256). Default 204 (~0.8).
 *  new_rate = (alpha * remb + (256 - alpha) * old_rate) / 256 */
#ifndef NANORTC_BWE_EMA_ALPHA
#define NANORTC_BWE_EMA_ALPHA 204
#endif

/* REMB packet identification (draft-alvestrand-rmcat-remb-03 §2) */
#define BWE_REMB_FMT       15         /* RTCP PSFB FMT=15 for REMB */
#define BWE_REMB_MIN_SIZE  16         /* Minimum REMB packet size: header(8) + SSRC(4) + REMB(4) */
#define BWE_REMB_UNIQUE_ID 0x52454D42 /* "REMB" in ASCII */

typedef struct nano_bwe {
    uint32_t estimated_bitrate;  /* bps — current smoothed estimate */
    uint32_t prev_event_bitrate; /* bps — bitrate at last event emission */
    uint32_t last_remb_bitrate;  /* bps — last raw REMB value received */
    uint32_t last_update_ms;     /* monotonic time of last REMB update */
    uint32_t remb_count;         /* number of REMB packets processed */
    uint32_t twcc_count;         /* number of TWCC feedbacks processed */
    uint8_t last_source;         /* nanortc_bwe_source_t of most recent update */

    /* Runtime-tunable parameters (0 means "use compile-time default").
     * Changed via nanortc_set_bitrate_bounds/_initial_bitrate/
     * _bwe_event_threshold. */
    uint32_t runtime_min_bps;
    uint32_t runtime_max_bps;
    uint8_t runtime_event_threshold_pct;
} nano_bwe_t;

/**
 * @brief Initialize bandwidth estimator.
 * @param bwe  Bandwidth estimator state.
 * @return NANORTC_OK on success.
 */
int bwe_init(nano_bwe_t *bwe);

/**
 * @brief Process incoming RTCP feedback for bandwidth estimation.
 *
 * Parses REMB (RTCP PSFB FMT=15) packets and updates the estimated
 * bitrate using exponential moving average smoothing.
 *
 * @param bwe   Bandwidth estimator state.
 * @param data  Raw RTCP packet (after SRTCP unprotect).
 * @param len   Length of RTCP packet.
 * @param now_ms  Current monotonic time in milliseconds.
 * @return NANORTC_OK if REMB was processed, negative error otherwise.
 * @retval NANORTC_ERR_INVALID_PARAM  NULL pointer.
 * @retval NANORTC_ERR_PARSE          Not a valid REMB packet.
 */
int bwe_on_rtcp_feedback(nano_bwe_t *bwe, const uint8_t *data, size_t len, uint32_t now_ms);

/**
 * @brief Get current estimated bitrate.
 * @param bwe  Bandwidth estimator state.
 * @return Estimated bitrate in bps, or 0 if bwe is NULL.
 */
uint32_t bwe_get_bitrate(const nano_bwe_t *bwe);

/**
 * @brief Parse REMB bitrate from raw RTCP PSFB packet.
 *
 * Extracts the bitrate from a REMB feedback message
 * (draft-alvestrand-rmcat-remb-03 §2).
 *
 * @param data      Raw RTCP PSFB packet data.
 * @param len       Packet length.
 * @param bitrate   Output: REMB bitrate in bps.
 * @return NANORTC_OK on success, NANORTC_ERR_PARSE if not a REMB packet.
 */
int bwe_parse_remb(const uint8_t *data, size_t len, uint32_t *bitrate);

/**
 * @brief Check if the estimate changed enough to warrant an event.
 *
 * Compares current estimated_bitrate against prev_event_bitrate.
 * If the change exceeds NANORTC_BWE_EVENT_THRESHOLD_PCT, returns true
 * and updates prev_event_bitrate.
 *
 * @param bwe  Bandwidth estimator state.
 * @return true if an event should be emitted.
 */
int bwe_should_emit_event(nano_bwe_t *bwe);

/**
 * @brief Feed a loss-based TWCC signal into the bandwidth estimator.
 *
 * Implements a legacy libwebrtc-style loss-based controller:
 *   - loss_fraction_q8 <  5  (< ~2%): increase estimate by 8 %.
 *   - loss_fraction_q8 in [5, 25] (2 %–10 %): hold estimate.
 *   - loss_fraction_q8 >  25 (> ~10%): decrease estimate by (0.5 * loss).
 *
 * The resulting value is clamped to [NANORTC_BWE_MIN_BITRATE,
 * NANORTC_BWE_MAX_BITRATE] and then combined with the existing estimate
 * via the same EMA used for REMB (alpha = NANORTC_BWE_EMA_ALPHA / 256).
 * This parallels bwe_on_rtcp_feedback() so the two signal paths stay
 * coherent.
 *
 * @param bwe               Bandwidth estimator state.
 * @param loss_fraction_q8  Packet loss as an unsigned 8-bit fraction
 *                          (count * 256 / total). 0 = no loss, 256 = all.
 * @param now_ms            Current monotonic time in milliseconds.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p bwe is NULL.
 */
int bwe_on_twcc_loss(nano_bwe_t *bwe, uint16_t loss_fraction_q8, uint32_t now_ms);

#endif /* NANORTC_BWE_H_ */
