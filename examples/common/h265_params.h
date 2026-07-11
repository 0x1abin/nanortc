/*
 * h265_params.h — shared H.265/HEVC parameter-set (VPS/SPS/PPS) extraction
 *
 * Chrome's and Safari's WebRTC HEVC decoders only start after seeing
 * sprop-vps/sps/pps in the SDP fmtp (RFC 7798 §7.1) — in-band parameter sets
 * carried in every IDR are not sufficient. Hardware-encoder publishers
 * (Rockchip MPP, Sophgo SG2002 CVI, NVENC) all need to pull VPS/SPS/PPS out of
 * the first IDR and hand the raw NALs to the SDK via
 * uipcat_client_config.h265_* before signaling comes up. This is that shared
 * extractor, used by camera-rk3588 / camera-nvenc / lichee-rv-nano.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NANORTC_EXAMPLES_COMMON_H265_PARAMS_H_
#define NANORTC_EXAMPLES_COMMON_H265_PARAMS_H_

#include "nanortc_config.h" /* NANORTC_FEATURE_H265 */

#if NANORTC_FEATURE_H265

#include <signal.h> /* sig_atomic_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Retry/budget tunables — #define before including, or pass via -D, to override. */
#ifndef NANORTC_APP_H265_PS_MAX_RETRY
#define NANORTC_APP_H265_PS_MAX_RETRY 3
#endif
#ifndef NANORTC_APP_H265_PS_BUDGET_MS
#define NANORTC_APP_H265_PS_BUDGET_MS 2000
#endif

#define H265_PARAM_SET_SCRATCH_SIZE 4096 /* VPS+SPS+PPS < 200 B in practice */

typedef struct {
    uint8_t bytes[H265_PARAM_SET_SCRATCH_SIZE];
    size_t vps_off, vps_len;
    size_t sps_off, sps_len;
    size_t pps_off, pps_len;
    bool ready;
} h265_params_t;

/* Extract VPS (NUT=32), SPS (NUT=33), PPS (NUT=34) from one Annex-B frame.
 * Returns 0 and sets ps->ready once all three are captured, else -1. */
int h265_params_extract(h265_params_t *ps, const uint8_t *annex_b, size_t len);

/* Pop frames from `q` for up to `max_retry` attempts of `budget_ms` each,
 * forcing a keyframe at the start of every attempt (`force_keyframe` may be
 * NULL) so the next IDR carries the parameter sets, until h265_params_extract()
 * succeeds. `quit` (may be NULL) aborts early once it becomes nonzero. Logs
 * progress (WARN per failed attempt, ERROR on give-up, INFO with the byte
 * counts on success) prefixed with `log_tag`. Returns 0 (ps->ready) or -1. */
int h265_params_extract_from_queue(h265_params_t *ps, media_queue_t *q, int max_retry,
                                   int budget_ms, const volatile sig_atomic_t *quit,
                                   void (*force_keyframe)(void), const char *log_tag);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_FEATURE_H265 */
#endif /* NANORTC_EXAMPLES_COMMON_H265_PARAMS_H_ */
