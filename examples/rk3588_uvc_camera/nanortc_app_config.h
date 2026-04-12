/*
 * nanortc_app_config.h — rk3588_uvc_camera build-time config override
 *
 * Included via NANORTC_CONFIG_FILE before nanortc_config.h defaults.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_APP_CONFIG_H_
#define NANORTC_APP_CONFIG_H_

/* HD/4K H.264 IDR frames produce many FU-A fragments at MTU 1200:
 *   1080p IDR ~150KB → ~125 fragments
 *   4K IDR   ~500KB → ~420 fragments
 * pkt_ring slots are indexed by out_tail, so the queue must be large
 * enough that a single keyframe never wraps the ring before the host
 * has drained it. 512 covers 4K with headroom. */
#define NANORTC_OUT_QUEUE_SIZE 512

#endif /* NANORTC_APP_CONFIG_H_ */
