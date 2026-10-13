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
#include <stdbool.h>
#include "nano_twcc.h"
#include "nanortc_config.h"

/* REMB packet identification (draft-alvestrand-rmcat-remb-03 §2) */
#define BWE_REMB_FMT       15         /* RTCP PSFB FMT=15 for REMB */
#define BWE_REMB_MIN_SIZE  16         /* Minimum REMB packet size: header(8) + SSRC(4) + REMB(4) */
#define BWE_REMB_UNIQUE_ID 0x52454D42 /* "REMB" in ASCII */

typedef struct {
    uint32_t sequence;
    uint32_t sent_ms;
    uint16_t bytes;
    uint8_t flags;
} nano_bwe_sent_t;

typedef struct nano_bwe {
    uint32_t estimated_bitrate;  /* bps — current bounded target */
    uint32_t prev_event_bitrate; /* bps — bitrate at last event emission */
    uint32_t last_update_ms;     /* monotonic time of last update */
    uint32_t remb_count;         /* number of REMB packets processed */
    uint32_t twcc_count;         /* number of TWCC feedbacks processed */
    uint16_t smoothed_loss_q8;   /* Smoothed accepted loss fraction (q8: 256 = 100%). */

    /* Runtime-tunable parameters (0 means "use compile-time default").
     * Changed via nanortc_set_bitrate_bounds/_initial_bitrate/
     * _bwe_event_threshold. */
    uint32_t runtime_min_bps;
    uint32_t runtime_max_bps;
    uint32_t local_target;
    uint32_t remb_limit;
    uint32_t latest_sequence;
    uint32_t history_missed;
    nano_bwe_sent_t sent[NANORTC_BWE_SEND_HISTORY_SIZE];
    uint32_t send_bytes[NANORTC_BWE_RATE_BUCKETS];
    uint32_t bucket_start_ms;
    uint32_t control_ms;
    uint32_t stable_since_ms;
    uint32_t last_probe_ms;
    uint32_t last_feedback_ms;
    uint32_t last_bad_send_ms;
    uint32_t delay_first_send_ms;
    int64_t delay_first_arrival_us;
    int64_t delay_last_arrival_us;
    uint16_t delay_samples;
    bool bucket_valid;
    bool control_valid;
    bool stable_valid;
    bool bad_send_valid;
    bool delay_valid;
    bool delay_growing;
    bool feedback_valid;
    bool sequence_valid;
    uint8_t runtime_event_threshold_pct;
} nano_bwe_t;

void bwe_record_transmit(nano_bwe_t *bwe, uint32_t sequence, uint16_t bytes, uint32_t at_ms,
                         bool has_twcc, bool retransmit);
void bwe_record_drop(nano_bwe_t *bwe, uint32_t at_ms);
int bwe_on_twcc_feedback(nano_bwe_t *bwe, const uint8_t *data, size_t len, uint32_t now_ms);

/**
 * @brief Initialize bandwidth estimator.
 * @param bwe  Bandwidth estimator state.
 * @return NANORTC_OK on success.
 */
int bwe_init(nano_bwe_t *bwe);

/**
 * @brief Transactionally update runtime bitrate bounds.
 *
 * A zero bound selects its compile-time default.  The effective bounds are
 * validated before state is changed; the current estimate and event baseline
 * are clamped immediately after a successful update.
 *
 * @param bwe      Bandwidth estimator state.
 * @param min_bps  Runtime minimum, or zero for NANORTC_BWE_MIN_BITRATE.
 * @param max_bps  Runtime maximum, or zero for NANORTC_BWE_MAX_BITRATE.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM on invalid input.
 */
int bwe_set_bounds(nano_bwe_t *bwe, uint32_t min_bps, uint32_t max_bps);

/**
 * @brief Set the pre-feedback bitrate estimate within the effective bounds.
 *
 * A zero value selects NANORTC_BWE_INITIAL_BITRATE.  Once REMB or TWCC
 * feedback has been processed this function intentionally becomes a no-op.
 *
 * @param bwe  Bandwidth estimator state.
 * @param bps  Initial bitrate, or zero for the compile-time default.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p bwe is NULL.
 */
int bwe_set_initial_bitrate(nano_bwe_t *bwe, uint32_t bps);

/**
 * @brief Process incoming RTCP feedback for bandwidth estimation.
 *
 * Parses REMB (RTCP PSFB FMT=15) as an immediate receiver cap.
 * With no matched TWCC, REMB directly supplies the local target.
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
 * @brief Smoothed accepted packet-loss fraction (q8: 256 = 100%).
 *
 * Zero until the first accepted loss sample. Adaptive FEC uses this value.
 * @param bwe  Bandwidth estimator state.
 * @return loss fraction in 256-ths (0..256), or 0 if @p bwe is NULL.
 */
uint16_t bwe_get_loss_q8(const nano_bwe_t *bwe);

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
 * @brief Feed a summary-only loss signal; it cannot authorize growth.
 *
 * At most once per control interval, loss above 10% reduces the local target
 * by half the loss fraction. Lower loss holds the target; growth additionally
 * requires matched send history, fresh feedback and measured offered load.
 * The independent REMB cap also applies below the configured encoder minimum.
 *
 * @param bwe               Bandwidth estimator state.
 * @param loss_fraction_q8  Packet loss as an unsigned 8-bit fraction
 *                          (count * 256 / total). 0 = no loss, 256 = all.
 * @param now_ms            Current monotonic time in milliseconds.
 * @return NANORTC_OK on success, NANORTC_ERR_INVALID_PARAM if @p bwe is NULL.
 */
int bwe_on_twcc_loss(nano_bwe_t *bwe, uint16_t loss_fraction_q8, uint32_t now_ms);

#endif /* NANORTC_BWE_H_ */
