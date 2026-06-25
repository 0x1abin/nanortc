/*
 * nanortc — Video receive reorder buffer (internal interface)
 * @internal Not part of the public API.
 *
 * Fixed-size buffer indexed by RTP sequence number that releases inbound video
 * packets in order, healing benign reordering before it reaches the FU
 * depacketizer (which cannot tolerate out-of-order fragments) and making the
 * loss signal precise (a genuine gap vs a reorder). Sans I/O: no allocation,
 * driven by the caller's now_ms. Arrival-driven: the latency cap
 * (NANORTC_VIDEO_REORDER_MAX_WAIT_MS) is enforced when the next packet pushes,
 * which is continuous for a live stream.
 *
 * Gated by NANORTC_FEATURE_VIDEO_REORDER (opt-in — see nanortc_config.h).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_REORDER_H_
#define NANORTC_REORDER_H_

#include "nanortc_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER

typedef struct nano_reorder_slot {
    uint8_t data[NANORTC_MEDIA_BUF_SIZE]; /* RTP payload (post header strip) */
    size_t len;
    uint32_t ts;         /* RTP timestamp */
    uint32_t arrival_ms; /* local time the packet arrived (latency cap) */
    uint16_t seq;        /* the RTP seq stored here (validates slot reuse) */
    uint8_t marker;      /* RTP marker bit */
    bool occupied;
} nano_reorder_slot_t;

typedef struct nano_reorder {
    nano_reorder_slot_t slots[NANORTC_VIDEO_REORDER_SLOTS];
    uint16_t next_seq; /* next RTP seq to release */
    bool inited;       /* next_seq seeded from the first packet */
    bool pending_lost; /* a gap was skipped; flag the next popped packet */
} nano_reorder_t;

/** Reset a reorder buffer to empty. */
void reorder_init(nano_reorder_t *r);

/**
 * Insert one received RTP packet payload (seq/ts/marker + data). Late or
 * duplicate packets (seq < next_seq) are dropped. A packet farther than the
 * window ahead force-advances next_seq (declaring the skipped span lost), so
 * progress is always bounded. @return NANORTC_OK, or NANORTC_ERR_INVALID_PARAM.
 */
int reorder_push(nano_reorder_t *r, uint16_t seq, uint32_t ts, uint8_t marker, const uint8_t *data,
                 size_t len, uint32_t now_ms);

/**
 * Pop the next in-order packet if present, or skip a missing predecessor that a
 * buffered packet has waited past NANORTC_VIDEO_REORDER_MAX_WAIT_MS for. Call in
 * a loop after each push until it returns NANORTC_ERR_NO_DATA.
 *
 * On NANORTC_OK, @p out_data points into the buffer (valid until the next push
 * that reuses the slot — consume it synchronously). @p *lost is true when one
 * or more packets were skipped immediately before this one (a gap precedes it).
 */
int reorder_pop(nano_reorder_t *r, uint32_t now_ms, uint16_t *out_seq, uint32_t *out_ts,
                uint8_t *out_marker, const uint8_t **out_data, size_t *out_len, bool *lost);

/**
 * Milliseconds until reorder_pop() would next release or skip-release a packet:
 * 0 if next_seq is already buffered (release now), else the time until the
 * oldest in-window packet ahead of a missing next_seq ages out
 * NANORTC_VIDEO_REORDER_MAX_WAIT_MS, else UINT32_MAX (nothing pending). Folded
 * into nanortc_next_timeout_ms() so a held frame flushes on schedule even when
 * no further RTP arrives.
 */
uint32_t reorder_next_timeout_ms(const nano_reorder_t *r, uint32_t now_ms);

#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_REORDER */
#endif /* NANORTC_REORDER_H_ */
