/*
 * nanortc_app_config.h — macos_camera configuration overrides
 *
 * Included via NANORTC_CONFIG_FILE before nanortc_config.h defaults.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_APP_CONFIG_H_
#define NANORTC_APP_CONFIG_H_

/* 720p/1080p H.264: keyframe can reach 50-300 KB → 42-250 FU-A
 * fragments at MTU 1200. pkt_ring slots are indexed by out_tail, so
 * the queue must be large enough to avoid ring wrap-around collisions. */
#define NANORTC_OUT_QUEUE_SIZE 256

#endif /* NANORTC_APP_CONFIG_H_ */
