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
    uint32_t estimated_bitrate; /* bps — current smoothed estimate */
    uint32_t last_remb_bitrate; /* bps — last raw REMB value received */
    uint32_t last_update_ms;    /* monotonic time of last REMB update */
    uint32_t remb_count;        /* number of REMB packets processed */
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

#endif /* NANORTC_BWE_H_ */
