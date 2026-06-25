/*
 * nanortc_app_config.h — linux_uvc_camera build-time config override
 *
 * Included via NANORTC_CONFIG_FILE before nanortc_config.h defaults.
 *
 * NOTE: NANORTC_FEATURE_H265 / NANORTC_FEATURE_VIDEO are NOT set here —
 * they come from the CMake options (-DNANORTC_FEATURE_H265=ON) as PUBLIC
 * -D defines on the nanortc target. Defining them here too would be a
 * macro redefinition. This file carries only the size/pacing overrides
 * that have no CMake option.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_APP_CONFIG_H_
#define NANORTC_APP_CONFIG_H_

/* HD H.265 IDR frames produce many FU fragments at MTU 1200:
 *   1080p IDR ~120-180KB → ~100-150 fragments
 * pkt_ring slots are indexed by out_tail, so the queue must be large
 * enough that a single keyframe never wraps the ring before the host
 * has drained it. 512 covers 1080p/4K with headroom. */
#define NANORTC_OUT_QUEUE_SIZE 512

/* The default 3000-byte pacer burst cap starves HD: a 150KB IDR drains
 * in dozens of pump cycles and the tail just ages out at MAX_QUEUE_MS,
 * defeating smooth pacing. 16000 lets ~13 packets burst before metering,
 * spreading an IDR over roughly one frame interval on a high-capacity LAN. */
#define NANORTC_PACING_MAX_BURST_BYTES 16000

#endif /* NANORTC_APP_CONFIG_H_ */
