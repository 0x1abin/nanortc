/*
 * nanortc — Media send/receive paths
 *
 * Owns audio/video packetization, the video pkt_ring scratch ring, the
 * track add / direction APIs, the video keyframe-request (PLI) feedback
 * path, the BWE runtime knobs / per-track stats accessors, the
 * SRTP-protected RTP/RTCP receive backbone (RFC 7983 §3 [0x80-0xBF]
 * arm), and the periodic RTCP Sender Report cadence (RFC 3550 §6.2).
 *
 * Lifted out of nano_rtc.c across two slices of Phase 10 PR-4:
 *   slice 2 (2026-05-01) — send paths + track APIs + BWE knobs.
 *   slice 3 (2026-05-02) — receive RTP/RTCP demux + RTCP SR cadence.
 *
 * The split preserves the public API, the module dependency graph, and
 * the output-queue lifetime contract on `nanortc_output_t` (see
 * include/nanortc.h). The transport backbone in nano_rtc.c continues to
 * own ICE, TURN, DTLS, SCTP, and the output queue itself.
 *
 * Cross-file enqueue goes through `nano_rtc_enqueue_transmit()` — the
 * previously file-static helper in nano_rtc.c, promoted to internal
 * linkage and declared in nano_rtc_internal.h. All other helpers either
 * stay file-static here or live in their own protocol modules.
 *
 * Under CORE_ONLY / DATA-only builds (NANORTC_HAVE_MEDIA_TRANSPORT == 0)
 * the body is fully `#if`-guarded out and this translation unit compiles
 * to a no-op object — the empty-TU sentinel below keeps strict ISO C
 * happy.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_log.h"

#if NANORTC_HAVE_MEDIA_TRANSPORT
#include "nano_media.h"
#include "nano_rtp.h"
#include "nano_rtcp.h"
#include "nano_srtp.h"
#include "nano_sdp.h"
#endif

#if NANORTC_FEATURE_VIDEO
#include "nano_h264.h"
#include "nano_bwe.h"
#include "nano_twcc.h"
#if NANORTC_FEATURE_H265
#include "nano_h265.h"
#include "nano_base64.h"
#include "nano_annex_b.h"
#endif
#if NANORTC_FEATURE_VIDEO_FEC
#include "nano_fec.h"
/* Forward decl: the FEC capture hook fires from the send path (above), but the
 * helper is defined alongside the other FEC helpers further down. */
static void rtc_fec_capture(nanortc_t *rtc, nanortc_track_t *m, const uint8_t *pkt, size_t len);
#endif
/* Internal video flags for RTP packetization */
#define NANORTC_VIDEO_FLAG_KEYFRAME 0x01 /* NAL is part of a keyframe (IDR) */
#define NANORTC_VIDEO_FLAG_MARKER   0x02 /* Last NAL in access unit (RTP marker bit) */
#endif

#include <string.h>

#if NANORTC_HAVE_MEDIA_TRANSPORT

static int nanortc_add_track(nanortc_t *rtc, nanortc_track_kind_t kind,
                             nanortc_direction_t direction, nanortc_codec_t codec,
                             uint32_t sample_rate, uint8_t channels)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->media_count >= NANORTC_MAX_MEDIA_TRACKS) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Determine PT for SDP */
    uint8_t pt = 0;
    uint8_t sdp_kind = SDP_MLINE_AUDIO;
    if (kind == NANORTC_TRACK_AUDIO) {
        if (codec == NANORTC_CODEC_PCMU)
            pt = 0;
        else if (codec == NANORTC_CODEC_PCMA)
            pt = 8;
        else
            pt = 111; /* Opus dynamic PT */
        sdp_kind = SDP_MLINE_AUDIO;
    } else {
#if NANORTC_FEATURE_H265
        if (codec == NANORTC_CODEC_H265) {
            pt = NANORTC_VIDEO_H265_DEFAULT_PT; /* 98 */
        } else
#endif
        {
            pt = NANORTC_VIDEO_DEFAULT_PT; /* 96 */
        }
        sdp_kind = SDP_MLINE_VIDEO;
    }

    /* Add SDP m-line (returns MID) */
    int mid =
        sdp_add_mline(&rtc->sdp, sdp_kind, (uint8_t)codec, pt, sample_rate, channels, direction);
    if (mid < 0) {
        return mid;
    }

    /* Initialize media track at the next available slot (not by MID index —
     * DC can occupy SDP MIDs without consuming media track slots). */
    uint8_t tidx = rtc->media_count;

    uint32_t jitter_ms = 0;
#if NANORTC_FEATURE_AUDIO
    jitter_ms = rtc->config.jitter_depth_ms;
#endif
    int rc = track_init(&rtc->media[tidx], (uint8_t)mid, kind, direction, (uint8_t)codec,
                        sample_rate, channels, jitter_ms);
    if (rc != NANORTC_OK) {
        return rc;
    }
    rtc->media_count = tidx + 1;

    NANORTC_LOGI("RTC", "media track added");
    return mid;
}

int nanortc_add_audio_track(nanortc_t *rtc, nanortc_direction_t direction, nanortc_codec_t codec,
                            uint32_t sample_rate, uint8_t channels)
{
    return nanortc_add_track(rtc, NANORTC_TRACK_AUDIO, direction, codec, sample_rate, channels);
}

int nanortc_add_video_track(nanortc_t *rtc, nanortc_direction_t direction, nanortc_codec_t codec)
{
    return nanortc_add_track(rtc, NANORTC_TRACK_VIDEO, direction, codec, 90000, 0);
}

#if NANORTC_FEATURE_H265
/* Emit "<tag><base64(nal)>" into dst[*pos], advancing *pos. */
static int h265_sprop_emit(char *dst, size_t cap, size_t *pos, const char *tag, size_t tag_len,
                           const uint8_t *nal, size_t nal_len)
{
    if (*pos + tag_len > cap) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(dst + *pos, tag, tag_len);
    *pos += tag_len;

    size_t enc_len = 0;
    int rc = nano_base64_encode(nal, nal_len, dst + *pos, cap - *pos, &enc_len);
    if (rc != NANORTC_OK) {
        return rc;
    }
    *pos += enc_len;
    return NANORTC_OK;
}

int nanortc_video_set_h265_parameter_sets(nanortc_t *rtc, uint8_t mid, const uint8_t *vps,
                                          size_t vps_len, const uint8_t *sps, size_t sps_len,
                                          const uint8_t *pps, size_t pps_len)
{
    if (!rtc || !vps || vps_len < H265_NAL_HEADER_SIZE || !sps || sps_len < H265_NAL_HEADER_SIZE ||
        !pps || pps_len < H265_NAL_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m || m->kind != NANORTC_TRACK_VIDEO || m->codec != NANORTC_CODEC_H265) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    nano_sdp_mline_t *ml = sdp_find_mline(&rtc->sdp, mid);
    if (!ml) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    char *dst = ml->h265_sprop_fmtp;
    const size_t cap = NANORTC_H265_SPROP_FMTP_SIZE;
    size_t pos = 0;

    int rc;
    if ((rc = h265_sprop_emit(dst, cap, &pos, "sprop-vps=", 10, vps, vps_len)) != NANORTC_OK ||
        (rc = h265_sprop_emit(dst, cap, &pos, ";sprop-sps=", 11, sps, sps_len)) != NANORTC_OK ||
        (rc = h265_sprop_emit(dst, cap, &pos, ";sprop-pps=", 11, pps, pps_len)) != NANORTC_OK) {
        return rc;
    }

    ml->h265_sprop_fmtp_len = (uint16_t)pos;

    /* Extract profile_space / tier_flag / profile_idc / level_idc from the
     * VPS profile_tier_level() so the SDP fmtp advertises the actual stream
     * level. Safari's WebRTC decoder drops frames silently when SDP level-id
     * understates the stream's general_level_idc.
     *
     * RBSP layout (H.265 §7.3.2.1 with max_sub_layers_minus1 = 0), logical
     * byte offsets (i.e. after stripping emulation-prevention 0x03 bytes
     * per §7.4.1.1):
     *   [0..1] NAL header (2 bytes)
     *   [2] vps_video_parameter_set_id(4) | base_layer_internal(1) |
     *       base_layer_available(1) | max_layers_minus1[5:4](2 MSB)
     *   [3] max_layers_minus1[3:0](4) | max_sub_layers_minus1(3) |
     *       temporal_id_nesting(1)
     *   [4..5] vps_reserved_0xffff_16bits
     *   [6] general_profile_space(2) | general_tier_flag(1) |
     *       general_profile_idc(5)
     *   [7..10] general_profile_compatibility_flag[0..31]
     *   [11..16] progressive/interlaced/non_packed/frame_only + 43 reserved
     *            bits + inbld = 48 bits total
     *   [17] general_level_idc
     *
     * The caller hands us the raw NAL (with EP bytes still in). Our range
     * of interest (up to byte 17) can contain up to three 00 00 03 triples
     * on real encoders, so we must scan logically rather than index. */
    if (vps_len >= 18) {
        uint8_t ptl0 = 0;
        uint8_t level = 0;
        size_t logical = 0;
        bool got_ptl0 = false;
        uint8_t prev2 = 0xFF, prev1 = 0xFF;
        for (size_t i = 0; i < vps_len; i++) {
            uint8_t b = vps[i];
            if (i >= 2 && prev2 == 0x00 && prev1 == 0x00 && b == 0x03) {
                /* Skip emulation-prevention byte; does not count toward
                 * logical offset. */
                prev2 = prev1;
                prev1 = b;
                continue;
            }
            if (logical == 6) {
                ptl0 = b;
                got_ptl0 = true;
            } else if (logical == 17) {
                level = b;
                break;
            }
            logical++;
            prev2 = prev1;
            prev1 = b;
        }
        if (got_ptl0) {
            ml->h265_profile_id = (uint8_t)(ptl0 & 0x1F);
            ml->h265_tier_flag = (uint8_t)((ptl0 >> 5) & 0x01);
            ml->h265_level_id = level;
        }
    }
    NANORTC_LOGI("SDP", "H265 sprop-vps/sps/pps stored");
    return NANORTC_OK;
}
#endif /* NANORTC_FEATURE_H265 */

/* Send audio: RTP pack → SRTP protect → enqueue */
static int rtc_send_audio(nanortc_t *rtc, nanortc_track_t *m, uint32_t timestamp,
                          const uint8_t *data, size_t len)
{
    size_t rtp_len = 0;
    int rc = rtp_pack(&m->rtp, timestamp, data, len, m->media_buf, sizeof(m->media_buf), &rtp_len);
    if (rc != NANORTC_OK)
        return rc;

    size_t srtp_len = 0;
    rc = nano_srtp_protect(&rtc->srtp, m->media_buf, rtp_len, &srtp_len);
    if (rc != NANORTC_OK)
        return rc;

    m->rtcp.packets_sent++;
    m->rtcp.octets_sent += (uint32_t)len;

    return nano_rtc_enqueue_transmit(rtc, m->media_buf, srtp_len, &rtc->remote_addr, false);
}

#if NANORTC_FEATURE_VIDEO
/* Reserve the next pkt_ring slot for an outbound video fragment.
 *
 * pkt_ring_tail is the ring's own write cursor — decoupled from out_tail
 * so NANORTC_VIDEO_PKT_RING_SIZE can be tuned independently of
 * NANORTC_OUT_QUEUE_SIZE. See nanortc_config.h for the slot-reuse
 * invariant when PKT_RING_SIZE < OUT_QUEUE_SIZE.
 *
 * Aliasing guard. out_queue[].transmit.data stores a pointer into
 * pkt_ring[]. nanortc_send_video() emits every FU-A fragment of one
 * access unit before returning, so the application has no chance to
 * drain mid-frame. If the in-flight depth has already reached
 * PKT_RING_SIZE, advancing pkt_ring_tail wraps into a slot whose pointer
 * is still pending — silent corruption. (out_tail - out_head) is a
 * conservative upper bound on that depth; bump stats_pkt_ring_overrun
 * and emit a single static-string NANORTC_LOGW so under-sizing surfaces
 * in integration smoke tests rather than as glitched IDRs on the wire.
 * Live counters are in stats_pkt_ring_overrun + out_tail / out_head. */
static uint8_t *pkt_ring_alloc_slot(nanortc_t *rtc, uint16_t *out_pslot)
{
    uint16_t out_inflight = (uint16_t)(rtc->out_tail - rtc->out_head);
#if NANORTC_FEATURE_VIDEO_PACING
    /* With pacing, unreleased fragments live in the pace FIFO (not yet in
     * out_queue) but still occupy pkt_ring slots that must not be wrapped
     * over. Add the pace backlog to the occupancy tripwire. */
    out_inflight = (uint16_t)(out_inflight + (uint16_t)(rtc->pacer.tail - rtc->pacer.head));
#endif
    if (out_inflight >= NANORTC_VIDEO_PKT_RING_SIZE) {
        __atomic_fetch_add(&rtc->stats_pkt_ring_overrun, 1, __ATOMIC_RELAXED);
        NANORTC_LOGW("RTC", "pkt_ring overrun — raise NANORTC_VIDEO_PKT_RING_SIZE");
    }
    uint16_t pslot = rtc->pkt_ring_tail & (NANORTC_VIDEO_PKT_RING_SIZE - 1);
    *out_pslot = pslot;
    return rtc->pkt_ring[pslot];
}

/* Stamp NACK retransmit metadata on the slot just written and advance
 * the ring's write cursor. rtp_pack() has already incremented m->rtp.seq,
 * so the seq value on the wire for this packet is (m->rtp.seq - 1). */
static void pkt_ring_commit_slot(nanortc_t *rtc, uint16_t pslot, uint16_t wire_seq,
                                 uint16_t srtp_len)
{
    rtc->pkt_ring_meta[pslot].seq = wire_seq;
    rtc->pkt_ring_meta[pslot].len = srtp_len;
    rtc->pkt_ring_tail++;
}

#if NANORTC_FEATURE_VIDEO_PACING
/* ================================================================
 * Send pacer (RFC-agnostic; mirrors libwebrtc PacedSender intent)
 *
 * A leaky token bucket meters video fragments out of pkt_ring into the
 * output queue at `BWE_estimate * NANORTC_PACING_FACTOR_PCT%`, so a
 * multi-fragment IDR is spread across ~one frame interval instead of
 * bursting onto the wire and overrunning the network bottleneck buffer
 * (self-inflicted loss → PLI → larger IDR). The FIFO is the [head, tail)
 * window over the pkt_ring slots: `tail` tracks pkt_ring_tail (every
 * committed fragment is pace-enqueued), `head` is the release cursor.
 * Catch-up: any fragment older than NANORTC_PACING_MAX_QUEUE_MS is flushed
 * immediately, so the pacer never adds unbounded latency. Control packets,
 * NACK retransmits and audio bypass the pacer entirely.
 * ================================================================ */

/* Effective pacing rate (bits/sec) = max(BWE, floor) * factor. 64-bit math
 * so a high BWE estimate * factor cannot overflow. */
static uint32_t pacer_rate_bps(const nanortc_t *rtc)
{
    uint32_t est = rtc->bwe.estimated_bitrate;
    if (est < NANORTC_PACING_MIN_RATE_BPS) {
        est = NANORTC_PACING_MIN_RATE_BPS;
    }
    uint64_t rate = ((uint64_t)est * (uint64_t)NANORTC_PACING_FACTOR_PCT) / 100u;
    return (rate > UINT32_MAX) ? UINT32_MAX : (uint32_t)rate;
}

void nano_rtc_pacer_enqueue(nanortc_t *rtc, uint16_t pslot)
{
    /* pkt_ring_commit_slot() already advanced pkt_ring_tail past this slot;
     * keep the pacer tail in lock-step so depth = tail - head counts the
     * unreleased fragments. Stamp the enqueue time for the catch-up cap. */
    rtc->pacer.enqueue_ms[pslot] = rtc->now_ms;
    rtc->pacer.tail = rtc->pkt_ring_tail;
}

void nano_rtc_pacer_pump(nanortc_t *rtc)
{
    nano_pacer_t *p = &rtc->pacer;
    if (p->head == p->tail) {
        return; /* nothing pending */
    }

    uint32_t now = rtc->now_ms;
    uint32_t rate = pacer_rate_bps(rtc);

    /* Token-bucket refill. now_ms is monotonic per the Sans-I/O contract, so
     * `now - last` here and the `aged` check below both use plain modular
     * subtraction — the same convention as every other timer in the stack —
     * which stays correct across the ~49-day uint32 wrap (a guard like
     * `now < last ? 0` would instead mis-age packets straddling the wrap). The
     * `== 0` test only anchors the very first pump so it never credits a full
     * epoch. A non-monotonic (backward) clock is out of contract; the elapsed
     * clamp + burst cap bound it to one capped refill (and, below, one bounded
     * catch-up flush) — never unbounded latency, credit, or 64-bit overflow. */
    if (p->last_refill_ms == 0) {
        p->last_refill_ms = now;
    }
    {
        uint32_t elapsed = now - p->last_refill_ms;
        if (elapsed > 1000u) {
            elapsed = 1000u; /* >1 s of credit only ever fills to the burst cap */
        }
        if (elapsed > 0) {
            uint64_t budget = (uint64_t)p->budget_bytes + ((uint64_t)rate * elapsed) / 8000u;
            if (budget > NANORTC_PACING_MAX_BURST_BYTES) {
                budget = NANORTC_PACING_MAX_BURST_BYTES;
            }
            p->budget_bytes = (uint32_t)budget;
            p->last_refill_ms = now;
        }
    }

    /* Release due fragments into out_queue. */
    while (p->head != p->tail) {
        uint16_t slot = p->head & (NANORTC_VIDEO_PKT_RING_SIZE - 1);
        uint16_t len = rtc->pkt_ring_meta[slot].len;
        if (len == 0) {
            p->head++; /* defensive: skip a stale/empty slot */
            continue;
        }

        bool have_budget = (p->budget_bytes >= len);
        bool aged = (uint32_t)(now - p->enqueue_ms[slot]) >= (uint32_t)NANORTC_PACING_MAX_QUEUE_MS;
        if (!have_budget && !aged) {
            break; /* not enough credit and not yet aged out — wait */
        }

        /* Respect out_queue capacity; if full, retry on the next pump. */
        if ((uint16_t)(rtc->out_tail - rtc->out_head) >= NANORTC_OUT_QUEUE_SIZE) {
            break;
        }
        if (nano_rtc_enqueue_transmit(rtc, rtc->pkt_ring[slot], len, &rtc->remote_addr, false) !=
            NANORTC_OK) {
            break;
        }

        if (have_budget) {
            p->budget_bytes -= len;
        } else {
            /* Catch-up release: latency cap forced this out without credit.
             * Atomic to match stats_pkt_ring_overrun — read cross-task by
             * example /debug endpoints. */
            __atomic_fetch_add(&rtc->stats_pace_catchup, 1, __ATOMIC_RELAXED);
            p->budget_bytes = 0;
        }
        __atomic_fetch_add(&rtc->stats_paced_packets, 1, __ATOMIC_RELAXED);
        p->head++;
    }

    /* Cache the next-release deadline for nanortc_next_timeout_ms(). */
    if (p->head == p->tail) {
        p->next_release_ms = now; /* drained */
    } else {
        uint16_t slot = p->head & (NANORTC_VIDEO_PKT_RING_SIZE - 1);
        uint16_t len = rtc->pkt_ring_meta[slot].len;
        uint32_t deficit = (len > p->budget_bytes) ? (uint32_t)(len - p->budget_bytes) : 0u;
        /* ms to accrue `deficit` bytes at `rate` (ceil): deficit*8000/rate. */
        uint32_t wait =
            (rate > 0) ? (uint32_t)(((uint64_t)deficit * 8000u + rate - 1u) / rate) : 1u;
        /* Never wait past the catch-up age cap. */
        uint32_t age = now - p->enqueue_ms[slot];
        uint32_t until_age =
            (age >= NANORTC_PACING_MAX_QUEUE_MS) ? 0u : (NANORTC_PACING_MAX_QUEUE_MS - age);
        if (until_age < wait) {
            wait = until_age;
        }
        p->next_release_ms = now + wait;
    }
}

uint32_t nano_rtc_pacer_next_deadline_ms(const nanortc_t *rtc, uint32_t now_ms)
{
    const nano_pacer_t *p = &rtc->pacer;
    if (p->head == p->tail) {
        return UINT32_MAX; /* pace FIFO empty — no deadline armed */
    }
    /* next_release_ms is the absolute deadline cached by the last pump.
     * A stale-low value only costs one extra (idempotent) pump, never a
     * missed release, so clamping to 0 here is always safe. */
    return (p->next_release_ms > now_ms) ? (p->next_release_ms - now_ms) : 0u;
}
#endif /* NANORTC_FEATURE_VIDEO_PACING */

#if NANORTC_FEATURE_H265
/* Context + callback used by the H.265 send path. H.264 no longer uses this
 * (see rtc_send_video below — it drives h264_fragment_iter_* directly for
 * zero-copy packetization), but h265_packetize_au() is still callback-based,
 * so these helpers stay alive under the H265 feature flag until a parallel
 * zero-copy refactor lands for H.265. */
typedef struct {
    nanortc_t *rtc;
    nanortc_track_t *media;
    uint32_t timestamp;
    int last_rc;
    int is_last_nal;
} video_send_ctx_t;

static int video_send_fragment_cb(const uint8_t *payload, size_t len, int marker, void *userdata)
{
    video_send_ctx_t *ctx = (video_send_ctx_t *)userdata;
    nanortc_t *rtc = ctx->rtc;
    nanortc_track_t *m = ctx->media;

    m->rtp.marker = (uint8_t)((marker && ctx->is_last_nal) ? 1 : 0);

    uint16_t pslot;
    uint8_t *pkt_buf = pkt_ring_alloc_slot(rtc, &pslot);

    size_t rtp_len = 0;
    int rc =
        rtp_pack(&m->rtp, ctx->timestamp, payload, len, pkt_buf, NANORTC_MEDIA_BUF_SIZE, &rtp_len);
    if (rc != NANORTC_OK) {
        NANORTC_LOGW("RTP", "video rtp_pack failed");
        ctx->last_rc = rc;
        return rc;
    }

#if NANORTC_FEATURE_VIDEO_FEC
    /* FEC protects the plaintext RTP — capture before SRTP encrypts in place. */
    rtc_fec_capture(rtc, m, pkt_buf, rtp_len);
#endif

    size_t srtp_len = 0;
    rc = nano_srtp_protect(&rtc->srtp, pkt_buf, rtp_len, &srtp_len);
    if (rc != NANORTC_OK) {
        NANORTC_LOGW("SRTP", "video srtp_protect failed");
        ctx->last_rc = rc;
        return rc;
    }

    m->rtcp.packets_sent++;
    m->rtcp.octets_sent += (uint32_t)len;
    rate_window_on_bytes(&m->rate_window, rtc->now_ms, (uint32_t)srtp_len);

    pkt_ring_commit_slot(rtc, pslot, (uint16_t)(m->rtp.seq - 1), (uint16_t)srtp_len);

#if NANORTC_FEATURE_VIDEO_PACING
    nano_rtc_pacer_enqueue(rtc, pslot);
    ctx->last_rc = NANORTC_OK;
#else
    ctx->last_rc = nano_rtc_enqueue_transmit(rtc, pkt_buf, srtp_len, &rtc->remote_addr, false);
#endif
    return ctx->last_rc;
}
#endif /* NANORTC_FEATURE_H265 */

/* Drive h264_fragment_iter_* directly so each FU-A payload is written into the
 * final pkt_ring[] slot once. The iterator writes at pkt_buf + rtp_hdr_len,
 * then rtp_pack() with payload == that same pointer hits the no-op guard and
 * skips its payload memcpy. End-to-end this saves the 1200-byte stack frame
 * that the old callback-based h264_packetize() reserved per fragment, and
 * removes one memcpy per FU-A fragment. */
static int rtc_send_video(nanortc_t *rtc, nanortc_track_t *m, uint32_t timestamp,
                          const uint8_t *data, size_t len, int flags)
{
    int is_last_nal = (flags & NANORTC_VIDEO_FLAG_MARKER) ? 1 : 0;

    h264_fragment_iter_t it;
    int rc = h264_fragment_iter_init(&it, data, len, NANORTC_VIDEO_MTU);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* Zero-copy scratch offset: RTP header (12 B) + optional TWCC ext (8 B).
     * Constant for the whole call — m->rtp.twcc_ext_id is fixed for this
     * track, so hoist out of the per-fragment loop. The iterator writes
     * the FU-A payload directly at pkt_buf + off; rtp_pack() then writes
     * the RTP header in the leading bytes and detects payload == pkt_buf
     * + off as a no-op. */
    bool has_twcc = (m->rtp.twcc_ext_id != 0 && m->rtp.twcc_ext_id <= 14);
    size_t off = (size_t)RTP_HEADER_SIZE + (has_twcc ? (size_t)RTP_TWCC_EXT_OVERHEAD : 0);
    if (off >= NANORTC_MEDIA_BUF_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    while (h264_fragment_iter_has_next(&it)) {
        uint16_t pslot;
        uint8_t *pkt_buf = pkt_ring_alloc_slot(rtc, &pslot);

        const uint8_t *payload = NULL;
        size_t payload_len = 0;
        int is_last_frag = 0;
        rc = h264_fragment_iter_next(&it, pkt_buf + off, NANORTC_MEDIA_BUF_SIZE - off, &payload,
                                     &payload_len, &is_last_frag);
        if (rc != NANORTC_OK) {
            NANORTC_LOGW("H264", "fragment_iter_next failed");
            return rc;
        }

        /* RFC 6184 §5.1: marker bit on last packet of access unit. */
        m->rtp.marker = (uint8_t)((is_last_frag && is_last_nal) ? 1 : 0);

        size_t rtp_len = 0;
        rc = rtp_pack(&m->rtp, timestamp, payload, payload_len, pkt_buf, NANORTC_MEDIA_BUF_SIZE,
                      &rtp_len);
        if (rc != NANORTC_OK) {
            NANORTC_LOGW("RTP", "video rtp_pack failed");
            return rc;
        }

#if NANORTC_FEATURE_VIDEO_FEC
        /* FEC protects the plaintext RTP — capture before SRTP encrypts in place. */
        rtc_fec_capture(rtc, m, pkt_buf, rtp_len);
#endif

        size_t srtp_len = 0;
        rc = nano_srtp_protect(&rtc->srtp, pkt_buf, rtp_len, &srtp_len);
        if (rc != NANORTC_OK) {
            NANORTC_LOGW("SRTP", "video srtp_protect failed");
            return rc;
        }

        m->rtcp.packets_sent++;
        m->rtcp.octets_sent += (uint32_t)payload_len;

        /* Post-SRTP wire bytes for the per-track rate window — matches the
         * byte count the network sees and the BWE estimate compares against. */
        rate_window_on_bytes(&m->rate_window, rtc->now_ms, (uint32_t)srtp_len);

        pkt_ring_commit_slot(rtc, pslot, (uint16_t)(m->rtp.seq - 1), (uint16_t)srtp_len);

#if NANORTC_FEATURE_VIDEO_PACING
        /* Hand the fragment to the pacer; it releases into out_queue at the
         * BWE-derived rate (admission above guaranteed pkt_ring headroom). */
        nano_rtc_pacer_enqueue(rtc, pslot);
#else
        rc = nano_rtc_enqueue_transmit(rtc, pkt_buf, srtp_len, &rtc->remote_addr, false);
        if (rc != NANORTC_OK) {
            return rc;
        }
#endif
    }

    return NANORTC_OK;
}
#endif /* NANORTC_FEATURE_VIDEO */

void nanortc_set_direction(nanortc_t *rtc, uint8_t mid, nanortc_direction_t dir)
{
    if (!rtc) {
        return;
    }
    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m) {
        return;
    }
    nanortc_direction_t old_dir = m->direction;
    m->direction = dir;

    /* Emit MEDIA_CHANGED event if direction actually changed */
    if (old_dir != dir) {
        nanortc_event_t mce;
        memset(&mce, 0, sizeof(mce));
        mce.type = NANORTC_EV_MEDIA_CHANGED;
        mce.media_changed.mid = mid;
        mce.media_changed.old_direction = old_dir;
        mce.media_changed.new_direction = dir;
        nano_rtc_emit_event_full(rtc, &mce);
    }
}

/* ----------------------------------------------------------------
 * Media send API
 * ---------------------------------------------------------------- */

/** Convert millisecond PTS to RTP clock timestamp. */
static inline uint32_t pts_ms_to_rtp(uint32_t pts_ms, uint32_t clock_rate)
{
    return (uint32_t)((uint64_t)pts_ms * clock_rate / 1000);
}

int nanortc_send_audio(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms, const void *data, size_t len)
{
    if (!rtc || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m || m->kind != NANORTC_TRACK_AUDIO) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    /* RFC 3264 §6: a recvonly / inactive track MUST NOT transmit. Read the
     * live direction (set_direction may change it mid-session). */
    if (m->direction == NANORTC_DIR_RECVONLY || m->direction == NANORTC_DIR_INACTIVE) {
        return NANORTC_ERR_STATE;
    }

    uint32_t rtp_ts = pts_ms_to_rtp(pts_ms, m->sample_rate);
    return rtc_send_audio(rtc, m, rtp_ts, (const uint8_t *)data, len);
}

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_H265
/* RFC 7798 §4.4: h265_packetize_au greedy-packs Single NAL / AP / FU and
 * sets the RTP marker bit on the final callback, so is_last_nal stays 1. */
static int rtc_send_video_h265(nanortc_t *rtc, nanortc_track_t *m, uint32_t timestamp,
                               const uint8_t *buf, size_t len)
{
    h265_nal_ref_t nals[NANORTC_MAX_NALS_PER_AU];
    size_t n_nals = 0;
    size_t offset = 0;

    while (offset < len && n_nals < NANORTC_MAX_NALS_PER_AU) {
        size_t nal_len = 0;
        const uint8_t *nal = nano_annex_b_find_nal(buf, len, &offset, &nal_len);
        if (!nal || nal_len == 0) {
            break;
        }
        nals[n_nals].data = nal;
        nals[n_nals].len = nal_len;
        n_nals++;
    }

    if (n_nals == 0) {
        return NANORTC_OK;
    }

    video_send_ctx_t ctx;
    ctx.rtc = rtc;
    ctx.media = m;
    ctx.timestamp = timestamp;
    ctx.last_rc = NANORTC_OK;
    ctx.is_last_nal = 1; /* packetize_au drives the marker bit internally */

    int rc = h265_packetize_au(nals, n_nals, NANORTC_VIDEO_MTU, video_send_fragment_cb, &ctx);
    if (rc != NANORTC_OK) {
        return rc;
    }
    return ctx.last_rc;
}
#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_H265 */

#if NANORTC_FEATURE_VIDEO
/* Worst-case RTP packet count for one Annex-B frame, computed before any
 * fragment is enqueued so a frame either ships whole or not at all.
 * H.264 (RFC 6184): Single NAL when nal_len <= MTU, else FU-A carries
 * (MTU - 2) payload bytes per fragment over (nal_len - 1) bytes (the NAL
 * header byte is replaced by the FU indicator/header pair).
 * H.265 (RFC 7798): Single NAL / AP when nal_len <= MTU — AP merging only
 * reduces the packet count, so 1 per NAL is a safe upper bound — else FU
 * carries (MTU - 3) payload bytes per fragment over (nal_len - 2) bytes. */
static size_t video_frame_worst_packets(const uint8_t *buf, size_t len, uint8_t codec)
{
    size_t total = 0;
    size_t offset = 0;
    size_t nal_len = 0;

    while (offset < len) {
        const uint8_t *nal = h264_annex_b_find_nal(buf, len, &offset, &nal_len);
        if (!nal || nal_len == 0) {
            break;
        }
        if (nal_len <= NANORTC_VIDEO_MTU) {
            total += 1;
            continue;
        }
#if NANORTC_FEATURE_H265
        if (codec == NANORTC_CODEC_H265) {
            size_t per = NANORTC_VIDEO_MTU - H265_FU_HEADER_SIZE;
            total += (nal_len - H265_NAL_HEADER_SIZE + per - 1) / per;
            continue;
        }
#else
        (void)codec;
#endif
        {
            size_t per = NANORTC_VIDEO_MTU - H264_FUA_HEADER_SIZE;
            total += (nal_len - 1 + per - 1) / per;
        }
    }
    return total;
}

int nanortc_send_video(nanortc_t *rtc, uint8_t mid, uint32_t pts_ms, const void *data, size_t len)
{
    if (!rtc || !data || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        NANORTC_LOGW("RTP", "video send blocked by state");
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m || m->kind != NANORTC_TRACK_VIDEO) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    /* RFC 3264 §6: a recvonly / inactive track MUST NOT transmit. */
    if (m->direction == NANORTC_DIR_RECVONLY || m->direction == NANORTC_DIR_INACTIVE) {
        return NANORTC_ERR_STATE;
    }

    const uint8_t *buf = (const uint8_t *)data;

    /* Atomic admission check: a frame either ships whole or is rejected
     * before anything is enqueued. The pre-guard behavior — enqueue until
     * the queue fills, then abandon the rest of the frame — put truncated
     * frames on the wire, guaranteeing receiver loss → PLI → an even
     * larger IDR that overflows again. Two capacity constraints share one
     * bound: out_queue free slots, and the pkt_ring aliasing window
     * (out_tail - out_head is the conservative upper bound on pkt_ring
     * slots still referenced by undrained outputs; see pkt_ring_alloc_slot). */
    size_t needed = video_frame_worst_packets(buf, len, (uint8_t)m->codec);
    if (needed == 0) {
        return NANORTC_OK; /* no NAL units found — nothing to send */
    }
    {
#if NANORTC_FEATURE_VIDEO_PACING
        /* With pacing, fragments queue in the pkt_ring (pace FIFO) and trickle
         * into out_queue over ~one frame interval, so the binding capacity is
         * the ring — not the output queue. `occupied` reserves headroom for the
         * unreleased pace backlog plus the freshly-pumped media still in
         * out_queue (the contiguous block ending at pkt_ring_tail), so a new
         * frame's `needed` slots never wrap over an unsent fragment. Same
         * whole-frame-or-nothing contract as the pre-pacer gate.
         * NOTE: this entry-count reservation assumes pinned slots are
         * contiguous with that live block. A generic-NACK retransmit pins a
         * *non-contiguous* older slot — but it now enqueues a COPY in the
         * nack_retx_buf ring (TD-023, resolved), so a later send_video wrapping
         * pkt_ring over that slot can no longer corrupt the in-flight
         * retransmit. */
        size_t cap = (size_t)NANORTC_VIDEO_PKT_RING_SIZE;
        if (needed > cap) {
            NANORTC_LOGW("RTP", "video frame exceeds pkt_ring capacity");
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        size_t occupied = (size_t)(uint16_t)(rtc->out_tail - rtc->out_head) +
                          (size_t)(uint16_t)(rtc->pacer.tail - rtc->pacer.head);
        if (occupied >= cap || needed > cap - occupied) {
            /* Retryable backpressure: drain outputs (and let time advance so
             * the pacer releases its backlog), then call again. */
            return NANORTC_ERR_WOULD_BLOCK;
        }
#else
        size_t cap = (NANORTC_VIDEO_PKT_RING_SIZE < NANORTC_OUT_QUEUE_SIZE)
                         ? (size_t)NANORTC_VIDEO_PKT_RING_SIZE
                         : (size_t)NANORTC_OUT_QUEUE_SIZE;
        if (needed > cap) {
            /* Permanently over capacity — retrying cannot help. The frame
             * must shrink (encoder bitrate/GOP) or the rings must grow. */
            NANORTC_LOGW("RTP", "video frame exceeds out_queue/pkt_ring capacity");
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        size_t inflight = (size_t)(uint16_t)(rtc->out_tail - rtc->out_head);
        if (inflight >= cap || needed > cap - inflight) {
            /* Retryable backpressure: drain outputs and call again. */
            return NANORTC_ERR_WOULD_BLOCK;
        }
#endif
    }

    /* One call = one encoded frame for fps accounting. Callers that split
     * a frame across multiple send calls will over-report, which we accept
     * because the API contract is "caller passes one complete frame". */
    rate_window_on_frame(&m->rate_window, rtc->now_ms);

    uint32_t ts = pts_ms_to_rtp(pts_ms, 90000);

#if NANORTC_FEATURE_H265
    if (m->codec == NANORTC_CODEC_H265) {
        return rtc_send_video_h265(rtc, m, ts, buf, len);
    }
#endif

    /* H.264: scan per NAL, dispatch to rtc_send_video (drives the
     * h264_fragment_iter_* zero-copy packetizer). */
    size_t offset = 0;
    size_t nal_len = 0;
    int last_rc = NANORTC_OK;

    while (offset < len) {
        const uint8_t *nal = h264_annex_b_find_nal(buf, len, &offset, &nal_len);
        if (!nal || nal_len == 0) {
            break;
        }

        int flags = 0;
        if ((nal[0] & 0x1F) == 5) {
            flags |= NANORTC_VIDEO_FLAG_KEYFRAME;
        }

        size_t peek_off = offset;
        size_t peek_len = 0;
        if (!h264_annex_b_find_nal(buf, len, &peek_off, &peek_len)) {
            flags |= NANORTC_VIDEO_FLAG_MARKER;
        }

        last_rc = rtc_send_video(rtc, m, ts, nal, nal_len, flags);
        if (last_rc != NANORTC_OK) {
            return last_rc;
        }
    }

    return last_rc;
}
#endif /* NANORTC_FEATURE_VIDEO */

/* Emit one RTCP PLI (RFC 4585 §6.3.1) for video track @p m: generate → SRTCP
 * protect (RFC 3711 §3.4) → enqueue. Shared by the public
 * nanortc_request_keyframe() and the receive-side auto-PLI loss-recovery path.
 * Caller guarantees @p m is a connected video track with SRTP ready. */
static int rtc_emit_pli(nanortc_t *rtc, nanortc_track_t *m)
{
    /* Build into the persistent rtcp_fb_buf, NOT a stack buffer: the auto-PLI
     * path calls this mid-receive-processing, so a stack buffer would be
     * clobbered before nanortc_poll_output() hands the pointer to the caller
     * (the out_queue stores only a pointer — nanortc_output_t lifetime
     * contract). stun_buf is unavailable here too — it holds the inbound RTP
     * packet being depacketized. */
    size_t pli_len = 0;
    int rc = rtcp_generate_pli(m->rtcp.ssrc, m->rtcp.remote_ssrc, rtc->rtcp_fb_buf,
                               sizeof(rtc->rtcp_fb_buf), &pli_len);
    if (rc != NANORTC_OK) {
        return rc;
    }
    size_t srtcp_len = 0;
    rc = nano_srtp_protect_rtcp(&rtc->srtp, rtc->rtcp_fb_buf, pli_len, &srtcp_len);
    if (rc != NANORTC_OK) {
        return rc;
    }
    return nano_rtc_enqueue_transmit(rtc, rtc->rtcp_fb_buf, srtcp_len, &rtc->remote_addr, false);
}

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_NACK_RX
/* Emit one RTCP Generic NACK (RFC 4585 §6.2.1) for video track @p m: PID =
 * first lost seq, @p blp = bitmask of the next 16. Built into the persistent
 * nack_buf (the gap pass may also emit a PLI into rtcp_fb_buf — distinct
 * buffers avoid aliasing). Caller guarantees @p m is connected with SRTP. */
static int rtc_emit_nack(nanortc_t *rtc, nanortc_track_t *m, uint16_t pid, uint16_t blp)
{
    size_t nack_len = 0;
    int rc = rtcp_generate_nack(m->rtcp.ssrc, m->rtcp.remote_ssrc, pid, blp, rtc->nack_buf,
                                sizeof(rtc->nack_buf), &nack_len);
    if (rc != NANORTC_OK) {
        return rc;
    }
    size_t srtcp_len = 0;
    rc = nano_srtp_protect_rtcp(&rtc->srtp, rtc->nack_buf, nack_len, &srtcp_len);
    if (rc != NANORTC_OK) {
        return rc;
    }
    __atomic_fetch_add(&rtc->stats_nack_sent, 1, __ATOMIC_RELAXED);
    return nano_rtc_enqueue_transmit(rtc, rtc->nack_buf, srtcp_len, &rtc->remote_addr, false);
}
#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_NACK_RX */

int nanortc_request_keyframe(nanortc_t *rtc, uint8_t mid)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (rtc->state < NANORTC_STATE_DTLS_CONNECTED || !rtc->srtp.ready) {
        return NANORTC_ERR_STATE;
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, mid);
    if (!m || m->kind != NANORTC_TRACK_VIDEO) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    return rtc_emit_pli(rtc, m);
}

#if NANORTC_FEATURE_VIDEO
/* React to a forward gap preceding the current payload: arm the `contiguous`
 * flag and fire a debounced auto-PLI for keyframe recovery. The loss source
 * differs by build — the reorder buffer's skip (precise) when
 * NANORTC_FEATURE_VIDEO_REORDER is on, else the raw forward-seq gap. */
static void rtc_video_on_lost(nanortc_t *rtc, nanortc_track_t *m, bool lost)
{
#if NANORTC_FEATURE_VIDEO_AUTO_PLI
    if (lost) {
        m->track.video.recv_lost_pending = true;
        uint32_t since = rtc->now_ms - m->track.video.recv_last_pli_ms;
        if (m->track.video.recv_last_pli_ms == 0 || since >= NANORTC_VIDEO_PLI_MIN_INTERVAL_MS) {
            if (rtc_emit_pli(rtc, m) == NANORTC_OK) {
                m->track.video.recv_last_pli_ms = rtc->now_ms;
                __atomic_fetch_add(&rtc->stats_auto_pli_sent, 1, __ATOMIC_RELAXED);
                NANORTC_LOGD("RTCP", "auto-PLI on receive loss");
            }
        }
    }
#else
    (void)rtc;
    (void)m;
    (void)lost;
#endif
}

/* Push one in-order payload into the depacketizer; on a complete NAL, fill @p ev
 * with the NANORTC_EV_MEDIA_DATA and return true (else false). Does NOT emit —
 * the caller decides whether to queue it (non-reorder, at most one NAL per
 * handle_input) or hand it straight to poll_output (reorder, one per poll call),
 * since `ev.media_data.data` aliases the single per-track depkt buffer and must
 * be consumed before the next NAL overwrites it. */
static bool rtc_video_complete_event(nanortc_t *rtc, nanortc_track_t *m, const uint8_t *payload,
                                     size_t payload_len, uint32_t rtp_ts, uint8_t rtp_marker,
                                     nanortc_event_t *ev)
{
    (void)rtc;
    const uint8_t *nalu_out = NULL;
    size_t nalu_len = 0;
    int drc;
    bool is_kf = false;
#if NANORTC_FEATURE_H265
    if (m->codec == NANORTC_CODEC_H265) {
        drc = h265_depkt_push(&m->track.video.depkt.h265, payload, payload_len, rtp_marker,
                              &nalu_out, &nalu_len);
        if (drc == NANORTC_OK && nalu_out && nalu_len > 0) {
            is_kf = h265_is_keyframe(nalu_out, nalu_len) ? true : false;
        }
    } else
#endif
    {
        drc = h264_depkt_push(&m->track.video.depkt.h264, payload, payload_len, rtp_marker,
                              &nalu_out, &nalu_len);
        if (drc == NANORTC_OK && nalu_out && nalu_len > 0) {
            is_kf = h264_is_keyframe(nalu_out, nalu_len) ? true : false;
        }
    }
    if (!(drc == NANORTC_OK && nalu_out && nalu_len > 0)) {
        return false;
    }
    memset(ev, 0, sizeof(*ev));
    ev->type = NANORTC_EV_MEDIA_DATA;
    ev->media_data.mid = m->mid;
    ev->media_data.pt = m->rtp.payload_type;
    ev->media_data.data = nalu_out;
    ev->media_data.len = nalu_len;
    ev->media_data.timestamp = rtp_ts;
    ev->media_data.is_keyframe = is_kf;
#if NANORTC_FEATURE_VIDEO_AUTO_PLI
    /* Report whether a gap preceded this frame, then clear so the next clean
     * frame is flagged contiguous again. Combine with is_keyframe to gate a
     * decoder. */
    ev->media_data.contiguous = !m->track.video.recv_lost_pending;
    m->track.video.recv_lost_pending = false;
#else
    ev->media_data.contiguous = true;
#endif
    return true;
}

#if !NANORTC_FEATURE_VIDEO_REORDER
/* Non-reorder path: react to loss, depacketize, and queue the event (at most
 * one NAL completes per handle_input, so the queued events never alias). */
static void rtc_video_deliver(nanortc_t *rtc, nanortc_track_t *m, const uint8_t *payload,
                              size_t payload_len, uint32_t rtp_ts, uint8_t rtp_marker, bool lost)
{
    rtc_video_on_lost(rtc, m, lost);
    nanortc_event_t ev;
    if (rtc_video_complete_event(rtc, m, payload, payload_len, rtp_ts, rtp_marker, &ev)) {
        nano_rtc_emit_event_full(rtc, &ev);
    }
}
#endif /* !NANORTC_FEATURE_VIDEO_REORDER */

#if NANORTC_FEATURE_VIDEO_REORDER
/* Poll-time reorder drain: release at most ONE completed NAL across all video
 * tracks directly into @p out, so the event is consumed by the application
 * before the next call's pop reuses the per-track depkt buffer (Sans-I/O
 * lifetime contract — fixes the multi-emit aliasing of a synchronous drain).
 * Skips found during the drain emit debounced auto-PLIs into the output queue.
 * @return NANORTC_OK with *out filled, or NANORTC_ERR_NO_DATA. */
int nano_rtc_media_reorder_produce(nanortc_t *rtc, nanortc_output_t *out)
{
    for (uint8_t i = 0; i < rtc->media_count; i++) {
        nanortc_track_t *m = &rtc->media[i];
        if (!m->active || m->kind != NANORTC_TRACK_VIDEO) {
            continue;
        }
        uint16_t r_seq;
        uint32_t r_ts;
        uint8_t r_marker;
        const uint8_t *r_data;
        size_t r_len;
        bool r_lost;
        while (reorder_pop(&m->track.video.reorder, rtc->now_ms, &r_seq, &r_ts, &r_marker, &r_data,
                           &r_len, &r_lost) == NANORTC_OK) {
            rtc_video_on_lost(rtc, m, r_lost);
            if (rtc_video_complete_event(rtc, m, r_data, r_len, r_ts, r_marker, &out->event)) {
                out->type = NANORTC_OUTPUT_EVENT;
                return NANORTC_OK; /* one NAL per call — no depkt-buffer aliasing */
            }
        }
    }
    return NANORTC_ERR_NO_DATA;
}

/* Earliest reorder release/skip deadline across video tracks (UINT32_MAX = none). */
uint32_t nano_rtc_media_reorder_next_timeout(const nanortc_t *rtc, uint32_t now_ms)
{
    uint32_t best = UINT32_MAX;
    for (uint8_t i = 0; i < rtc->media_count; i++) {
        const nanortc_track_t *m = &rtc->media[i];
        if (!m->active || m->kind != NANORTC_TRACK_VIDEO) {
            continue;
        }
        uint32_t d = reorder_next_timeout_ms(&m->track.video.reorder, now_ms);
        if (d < best) {
            best = d;
        }
    }
    return best;
}
#endif /* NANORTC_FEATURE_VIDEO_REORDER */
#endif /* NANORTC_FEATURE_VIDEO */

#if NANORTC_FEATURE_AUDIO
/* Poll-time jitter drain: pop at most ONE due audio frame across audio tracks
 * directly into @p out, so the application consumes the event before the next
 * call's pop reuses the per-track m->media_buf (Sans-I/O lifetime contract —
 * fixes TD-025's multi-emit aliasing of a synchronous in-handle_input drain).
 * @return NANORTC_OK with *out filled, or NANORTC_ERR_NO_DATA. */
int nano_rtc_media_audio_produce(nanortc_t *rtc, nanortc_output_t *out)
{
    for (uint8_t i = 0; i < rtc->media_count; i++) {
        nanortc_track_t *m = &rtc->media[i];
        if (!m->active || m->kind != NANORTC_TRACK_AUDIO) {
            continue;
        }
        size_t pop_len = 0;
        uint32_t pop_ts = 0;
        if (jitter_pop(&m->track.audio.jitter, rtc->now_ms, m->media_buf, sizeof(m->media_buf),
                       &pop_len, &pop_ts) == NANORTC_OK) {
            memset(&out->event, 0, sizeof(out->event));
            out->type = NANORTC_OUTPUT_EVENT;
            out->event.type = NANORTC_EV_MEDIA_DATA;
            out->event.media_data.mid = m->mid;
            out->event.media_data.pt = m->rtp.payload_type;
            out->event.media_data.data = m->media_buf;
            out->event.media_data.len = pop_len;
            out->event.media_data.timestamp = pop_ts;
            out->event.media_data.contiguous = true; /* jitter buffer ensures order */
            return NANORTC_OK; /* one frame per call — no media_buf aliasing */
        }
    }
    return NANORTC_ERR_NO_DATA;
}
#endif /* NANORTC_FEATURE_AUDIO */

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC
/* Adaptive FEC group size from the smoothed TWCC loss fraction (RFC 8888): 0 (no
 * FEC, zero overhead) on a clean link, the configured max group at moderate
 * loss, the smallest group (most protection) at high loss. K only varies DOWN
 * from NANORTC_FEC_GROUP_SIZE, so the fixed-size rings never overflow and the
 * receiver stays K-agnostic (the level-0 mask carries the group size). */
static uint8_t rtc_fec_effective_k(const nanortc_t *rtc)
{
#if NANORTC_FEC_ADAPTIVE
    uint16_t loss = bwe_get_loss_q8(&rtc->bwe);
    uint8_t k;
    if (loss < NANORTC_FEC_LOSS_OFF_Q8) {
        return 0; /* clean link: send no FEC */
    } else if (loss >= NANORTC_FEC_LOSS_HIGH_Q8) {
        k = (uint8_t)NANORTC_FEC_MIN_GROUP; /* high loss: most protection */
    } else {
        k = (uint8_t)NANORTC_FEC_GROUP_SIZE; /* moderate loss: default overhead */
    }
    /* Clamp to the ring size so a (mis)configured MIN_GROUP can never exceed the
     * fixed fec_tx_grp[NANORTC_FEC_GROUP_SIZE] array. */
    if (k > (uint8_t)NANORTC_FEC_GROUP_SIZE) {
        k = (uint8_t)NANORTC_FEC_GROUP_SIZE;
    }
    return k;
#else
    (void)rtc;
    return (uint8_t)NANORTC_FEC_GROUP_SIZE;
#endif
}

/* Capture one plaintext media RTP packet for FEC; on a full group of K, emit one
 * ULPFEC packet on a separate SSRC (media SSRC + 1), PT NANORTC_VIDEO_FEC_PT.
 * K is chosen adaptively per rtc_fec_effective_k(). Called from the video send
 * path after rtp_pack(), before SRTP. */
static void rtc_fec_capture(nanortc_t *rtc, nanortc_track_t *m, const uint8_t *pkt, size_t len)
{
    uint8_t k = rtc_fec_effective_k(rtc);
    if (k == 0) {
        rtc->fec_tx_n = 0; /* FEC disabled (clean link): drop any partial group */
        return;
    }
    if (len < FEC_RTP_HDR || len > NANORTC_MEDIA_BUF_SIZE) {
        return;
    }
    if (rtc->fec_tx_n >= k) {
        rtc->fec_tx_n = 0; /* defensive; also re-seats the group if K just shrank */
    }
    memcpy(rtc->fec_tx_grp[rtc->fec_tx_n], pkt, len);
    rtc->fec_tx_len[rtc->fec_tx_n] = (uint16_t)len;
    rtc->fec_tx_n++;
    if (rtc->fec_tx_n < k) {
        return;
    }
    rtc->fec_tx_n = 0; /* start a fresh group regardless of what follows */

    /* Pick a FEC tx buffer whose previous output has been dequeued (out_head
     * reached its free-at). Each in-flight FEC needs its own buffer so the
     * pointers in out_queue never alias a live output (which would put corrupt
     * FEC on the wire). With NANORTC_FEC_TX_RING slots, a bursty multi-group
     * frame emits FEC for up to N groups in one send call; if all slots are
     * still in flight this group's FEC is dropped (best-effort — reorder/NACK/
     * auto-PLI still cover it). */
    int8_t fslot = -1;
    for (uint8_t i = 0; i < NANORTC_FEC_TX_RING; i++) {
        if ((int16_t)(rtc->out_head - rtc->fec_tx_free_at[i]) >= 0) {
            fslot = (int8_t)i;
            break;
        }
    }
    if (fslot < 0) {
        return; /* all FEC buffers still referenced by undrained outputs */
    }

    const uint8_t *pkts[NANORTC_FEC_GROUP_SIZE];
    for (uint8_t i = 0; i < k; i++) {
        pkts[i] = rtc->fec_tx_grp[i];
    }
    uint8_t *fb = rtc->fec_tx_buf[fslot];
    fb[0] = 0x80; /* V=2 */
    fb[1] = (uint8_t)(NANORTC_VIDEO_FEC_PT & 0x7Fu);
    nanortc_write_u16be(fb + 2, rtc->fec_tx_seq);
    nanortc_write_u32be(fb + 4, nanortc_read_u32be(rtc->fec_tx_grp[k - 1] + 4));
    nanortc_write_u32be(fb + 8, m->rtp.ssrc + 1u); /* FEC SSRC */

    size_t body = 0;
    int rc = fec_encode(pkts, rtc->fec_tx_len, k, fb + FEC_RTP_HDR,
                        NANORTC_FEC_BUF_SIZE - FEC_RTP_HDR - NANORTC_SRTP_AUTH_TAG_SIZE, &body);
    if (rc != NANORTC_OK) {
        return;
    }

    size_t srtp_len = 0;
    if (nano_srtp_protect(&rtc->srtp, fb, FEC_RTP_HDR + body, &srtp_len) != NANORTC_OK) {
        return;
    }
    if (nano_rtc_enqueue_transmit(rtc, fb, srtp_len, &rtc->remote_addr, false) != NANORTC_OK) {
        return; /* queue full — buffer stays free (free_at unchanged) */
    }
    rtc->fec_tx_seq++;
    /* The FEC now occupies the out_queue slot (out_tail - 1); it is dequeued
     * once out_head reaches out_tail, so this slot frees at out_tail. */
    rtc->fec_tx_free_at[fslot] = rtc->out_tail;
    __atomic_fetch_add(&rtc->stats_fec_sent, 1, __ATOMIC_RELAXED);
}

/* Try to recover the one missing member of @p fec_rtp's group from the media
 * ring; on success, re-inject the recovered packet and return true. Returns
 * false if 0 or >= 2 members are absent (level-0 recovers exactly one). */
static bool rtc_fec_try(nanortc_t *rtc, nanortc_track_t *vm, const uint8_t *fec_rtp,
                        size_t fec_rtp_len)
{
    if (fec_rtp_len < (size_t)FEC_RTP_HDR + FEC_OVERHEAD) {
        return false;
    }
    const uint8_t *recv[NANORTC_FEC_GROUP_SIZE];
    uint16_t rlens[NANORTC_FEC_GROUP_SIZE];
    uint8_t have =
        (rtc->fec_rx_n < NANORTC_FEC_GROUP_SIZE) ? rtc->fec_rx_n : NANORTC_FEC_GROUP_SIZE;
    for (uint8_t i = 0; i < have; i++) {
        recv[i] = rtc->fec_rx_med[i];
        rlens[i] = rtc->fec_rx_len[i];
    }

    /* The protected media stream is the remote sender's SSRC. */
    uint32_t media_ssrc = vm->rtcp.remote_ssrc;
    size_t rec_len = 0;
    uint16_t rec_seq = 0;
    if (fec_recover(fec_rtp + FEC_RTP_HDR, fec_rtp_len - FEC_RTP_HDR, media_ssrc, recv, rlens, have,
                    rtc->fec_rx_recov, NANORTC_MEDIA_BUF_SIZE, &rec_len, &rec_seq) != NANORTC_OK) {
        return false;
    }
    __atomic_fetch_add(&rtc->stats_fec_recovered, 1, __ATOMIC_RELAXED);

    /* Re-inject the recovered plaintext media packet into the video path. */
    uint8_t rpt = 0;
    uint16_t rseq = 0;
    uint32_t rts = 0, rssrc = 0;
    const uint8_t *rpayload = NULL;
    size_t rpaylen = 0;
    if (rtp_unpack(rtc->fec_rx_recov, rec_len, &rpt, &rseq, &rts, &rssrc, &rpayload, &rpaylen) !=
        NANORTC_OK) {
        return false;
    }
    (void)rpt;
    (void)rssrc;
    (void)rec_seq;
    uint8_t rmarker = (uint8_t)((rtc->fec_rx_recov[1] >> 7) & 1u);
    /* Re-inject via the reorder buffer only (FEC requires REORDER — see the
     * config guard). reorder_push copies the NAL into its own slot and the
     * poll-time producer releases one-per-poll, so the recovered packet and the
     * live packet that triggered recovery never alias the depacketizer buffer. */
    reorder_push(&vm->track.video.reorder, rseq, rts, rmarker, rpayload, rpaylen, rtc->now_ms);
    return true;
}

/* True if a packet with sequence number @p sn is covered by the most recent
 * pending FEC's protected window (so a receiver NACK for it would duplicate the
 * FEC's recovery — RFC 5109/4585 complementary recovery). */
static bool rtc_fec_protects(const nanortc_t *rtc, uint16_t sn)
{
    if (!rtc->fec_prot_valid) {
        return false;
    }
    uint16_t off = (uint16_t)(sn - rtc->fec_prot_base);
    return off < FEC_MAX_GROUP && (rtc->fec_prot_mask & (uint16_t)(0x8000u >> off));
}

/* Handle a received FEC packet. Recovery is independent of FEC-vs-media wire
 * ordering: if the group's members have not all arrived yet (FEC raced ahead of
 * its media), buffer the FEC and retry as each media packet is admitted. */
static void rtc_fec_on_recv(nanortc_t *rtc, nanortc_track_t *vm, const uint8_t *fec_rtp,
                            size_t fec_rtp_len)
{
    /* Record the protected SN window (RFC 5109 level-0: SN base at body+2, mask
     * at body+12) so the receiver NACK can skip a packet this FEC will recover.
     * Done before recovery so it holds even when the gap is observed first. */
    if (fec_rtp_len >= (size_t)FEC_RTP_HDR + FEC_OVERHEAD) {
        rtc->fec_prot_base = nanortc_read_u16be(fec_rtp + FEC_RTP_HDR + 2);
        rtc->fec_prot_mask = nanortc_read_u16be(fec_rtp + FEC_RTP_HDR + 12);
        rtc->fec_prot_valid = true;
    }
    if (rtc_fec_try(rtc, vm, fec_rtp, fec_rtp_len)) {
        rtc->fec_rx_pending_len = 0;
        return;
    }
    if (fec_rtp_len >= (size_t)FEC_RTP_HDR + FEC_OVERHEAD && fec_rtp_len <= NANORTC_FEC_BUF_SIZE) {
        memcpy(rtc->fec_rx_pending, fec_rtp, fec_rtp_len);
        rtc->fec_rx_pending_len = (uint16_t)fec_rtp_len;
    }
}

/* Buffer one received plaintext media RTP packet for FEC recovery, then retry
 * any pending FEC whose group this packet may now complete. */
static void rtc_fec_buffer_media(nanortc_t *rtc, nanortc_track_t *vm, const uint8_t *pkt,
                                 size_t len)
{
    if (len < FEC_RTP_HDR || len > NANORTC_MEDIA_BUF_SIZE) {
        return;
    }
    uint8_t slot = (uint8_t)(rtc->fec_rx_n % NANORTC_FEC_GROUP_SIZE);
    memcpy(rtc->fec_rx_med[slot], pkt, len);
    rtc->fec_rx_len[slot] = (uint16_t)len;
    rtc->fec_rx_n++;

    if (rtc->fec_rx_pending_len > 0 &&
        rtc_fec_try(rtc, vm, rtc->fec_rx_pending, rtc->fec_rx_pending_len)) {
        rtc->fec_rx_pending_len = 0;
    }
}
#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC */

/* ================================================================
 * Track statistics
 * ================================================================ */

int nanortc_get_track_stats(const nanortc_t *rtc, uint8_t mid, nanortc_track_stats_t *stats)
{
    if (!rtc || !stats) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    const nanortc_track_t *m = NULL;
    for (uint8_t i = 0; i < rtc->media_count; i++) {
        if (rtc->media[i].active && rtc->media[i].mid == mid) {
            m = &rtc->media[i];
            break;
        }
    }
    if (!m) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    memset(stats, 0, sizeof(*stats));
    stats->mid = mid;
    stats->packets_sent = m->rtcp.packets_sent;
    stats->octets_sent = m->rtcp.octets_sent;
    stats->packets_received = m->rtcp.packets_received;
    stats->packets_lost = m->rtcp.packets_lost;
    stats->jitter = m->rtcp.jitter;

    /* RTT from DLSR: if we have a last_sr_recv_ms and the peer has
     * sent us at least one SR, compute round-trip from DLSR.
     * For now, expose raw DLSR data — actual RTT requires knowing
     * the current time, which is only available during handle_input. */
    stats->rtt_ms = 0;
    if (m->rtcp.last_sr_recv_ms > 0 && rtc->now_ms > m->rtcp.last_sr_recv_ms) {
        stats->rtt_ms = rtc->now_ms - m->rtcp.last_sr_recv_ms;
    }

#if NANORTC_FEATURE_VIDEO
    stats->bitrate_bps = rtc->bwe.estimated_bitrate;
    stats->estimated_bitrate_bps = rtc->bwe.estimated_bitrate;
#endif

    /* Phase 9: roll the send-rate window lazily so the snapshot reflects
     * the most recent completed second even when no send has happened
     * recently. Rolling is a no-op if the bucket is still filling. */
    nanortc_track_t *mw = (nanortc_track_t *)m;
    rate_window_roll(&mw->rate_window, rtc->now_ms);
    stats->send_bitrate_bps = m->rate_window.prev_bps;
    stats->send_fps_q8 = m->rate_window.prev_fps_q8;
    stats->fraction_lost = m->fraction_lost;

    return NANORTC_OK;
}

#if NANORTC_FEATURE_VIDEO
uint32_t nanortc_get_estimated_bitrate(const nanortc_t *rtc)
{
    if (!rtc) {
        return 0;
    }
    return bwe_get_bitrate(&rtc->bwe);
}

int nanortc_set_bitrate_bounds(nanortc_t *rtc, uint32_t min_bps, uint32_t max_bps)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    /* Reject inverted bounds when both are non-zero; 0 means "revert to default". */
    if (min_bps != 0 && max_bps != 0 && min_bps > max_bps) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    rtc->bwe.runtime_min_bps = min_bps;
    rtc->bwe.runtime_max_bps = max_bps;

    /* Clamp the current estimate to the new bounds so stats/events reflect
     * the new envelope immediately, without waiting for the next feedback. */
    uint32_t eff_min =
        rtc->bwe.runtime_min_bps ? rtc->bwe.runtime_min_bps : (uint32_t)NANORTC_BWE_MIN_BITRATE;
    uint32_t eff_max =
        rtc->bwe.runtime_max_bps ? rtc->bwe.runtime_max_bps : (uint32_t)NANORTC_BWE_MAX_BITRATE;
    if (rtc->bwe.estimated_bitrate < eff_min) {
        rtc->bwe.estimated_bitrate = eff_min;
    } else if (rtc->bwe.estimated_bitrate > eff_max) {
        rtc->bwe.estimated_bitrate = eff_max;
    }
    return NANORTC_OK;
}

int nanortc_set_initial_bitrate(nanortc_t *rtc, uint32_t bps)
{
    if (!rtc) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    /* Initial only applies before any feedback has driven the estimate
     * (remb_count == 0 && twcc_count == 0). After that the estimate is
     * tracking live feedback and the API is a no-op. */
    if (rtc->bwe.remb_count == 0 && rtc->bwe.twcc_count == 0) {
        rtc->bwe.estimated_bitrate = bps ? bps : (uint32_t)NANORTC_BWE_INITIAL_BITRATE;
        rtc->bwe.prev_event_bitrate = rtc->bwe.estimated_bitrate;
    }
    return NANORTC_OK;
}

int nanortc_set_bwe_event_threshold(nanortc_t *rtc, uint8_t pct)
{
    if (!rtc || pct > 100) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    rtc->bwe.runtime_event_threshold_pct = pct;
    return NANORTC_OK;
}
#endif /* NANORTC_FEATURE_VIDEO */

/* ----------------------------------------------------------------
 * Receive backbone — RFC 7983 §3 dispatch arm for [0x80-0xBF].
 * Called from rtc_process_receive() in nano_rtc.c after STUN / TURN /
 * DTLS demux and after the DTLS keying block has populated rtc->srtp.
 * ---------------------------------------------------------------- */

int nano_rtc_media_handle_rtp_or_rtcp(nanortc_t *rtc, const uint8_t *data, size_t len)
{
    if (!rtc->srtp.ready) {
        return NANORTC_OK; /* SRTP not ready yet, discard */
    }

    /* Distinguish RTP vs RTCP by payload type field (byte 1).
     * RTCP PT range: 200-211 (standard).
     * RFC 5761 §4: RTP PT < 72 or > 76, RTCP PT ∈ {200..211}. */
    if (len < 2) {
        return NANORTC_ERR_PARSE;
    }
    uint8_t second = data[1];

    if (second >= 200 && second <= 211) {
        /* RTCP packet — SRTCP unprotect then parse */
        if (len > sizeof(rtc->stun_buf)) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        /* Copy to scratch for in-place SRTCP unprotect */
        memcpy(rtc->stun_buf, data, len);
        size_t rtcp_len = 0;
        int urc = nano_srtp_unprotect_rtcp(&rtc->srtp, rtc->stun_buf, len, &rtcp_len);
        if (urc != NANORTC_OK) {
            return NANORTC_OK; /* Silently discard bad SRTCP packets */
        }
        /* RTCP datagrams from browsers are COMPOUND (RFC 3550 §6.1): a
         * leading SR/RR, then SDES, then the feedback packets (transport-cc /
         * PLI / NACK). Iterate every sub-packet — parsing only the first
         * member (the RR) silently drops all congestion and keyframe feedback,
         * which stalls BWE adaptation and keyframe recovery against Chrome. */
        size_t rtcp_off = 0;
        while (rtcp_off + RTCP_HEADER_SIZE <= rtcp_len) {
            const uint8_t *sub = rtc->stun_buf + rtcp_off;
            size_t sub_len = ((size_t)nanortc_read_u16be(sub + 2) + 1u) * 4u;
            if (sub_len < RTCP_HEADER_SIZE || rtcp_off + sub_len > rtcp_len) {
                break; /* truncated / malformed compound member */
            }
            nano_rtcp_info_t info;
            memset(&info, 0, sizeof(info));
            int rrc = rtcp_parse(sub, sub_len, &info);
            if (rrc == NANORTC_OK) {
                if (info.type == RTCP_SR) {
                    /* Sender Report — update receiver stats for DLSR (RFC 3550 §6.4.1).
                     * Compact NTP = middle 32 bits of NTP timestamp. */
                    int mid = ssrc_map_lookup(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, info.ssrc);
                    if (mid >= 0) {
                        nanortc_track_t *m =
                            track_find_by_mid(rtc->media, rtc->media_count, (uint8_t)mid);
                        if (m) {
                            m->rtcp.last_sr_ntp =
                                ((info.ntp_sec & 0xFFFFu) << 16) | (info.ntp_frac >> 16);
                            m->rtcp.last_sr_recv_ms = rtc->now_ms;
                            if (info.rb_valid) {
                                m->fraction_lost = info.rb_fraction_lost;
                            }
                        }
                    }
                } else if (info.type == RTCP_RR && info.rb_valid) {
                    /* Receiver Report addressed at our outbound SSRC. Store
                     * fraction_lost so nanortc_get_track_stats() can surface it. */
                    for (uint8_t i = 0; i < rtc->media_count; i++) {
                        nanortc_track_t *m = &rtc->media[i];
                        if (m->active && m->rtp.ssrc == info.rb_source_ssrc) {
                            m->fraction_lost = info.rb_fraction_lost;
                            break;
                        }
                    }
                } else if (info.type == RTCP_PSFB) {
                    /* PSFB — check FMT to distinguish PLI (FMT=1) from REMB (FMT=15) */
                    uint8_t psfb_fmt = sub[0] & 0x1F;
#if NANORTC_FEATURE_VIDEO
                    if (psfb_fmt == BWE_REMB_FMT) {
                        /* REMB — feed to bandwidth estimator */
                        uint32_t prev_bps = rtc->bwe.estimated_bitrate;
                        bwe_on_rtcp_feedback(&rtc->bwe, sub, sub_len, rtc->now_ms);
                        /* Emit event if estimate changed significantly */
                        if (bwe_should_emit_event(&rtc->bwe)) {
                            nanortc_event_t bwe_evt;
                            memset(&bwe_evt, 0, sizeof(bwe_evt));
                            bwe_evt.type = NANORTC_EV_BITRATE_ESTIMATE;
                            uint32_t cur_bps = rtc->bwe.estimated_bitrate;
                            bwe_evt.bitrate_estimate.bitrate_bps = cur_bps;
                            bwe_evt.bitrate_estimate.prev_bitrate_bps = prev_bps;
                            bwe_evt.bitrate_estimate.direction =
                                (cur_bps > prev_bps)   ? (uint8_t)NANORTC_BWE_DIR_UP
                                : (cur_bps < prev_bps) ? (uint8_t)NANORTC_BWE_DIR_DOWN
                                                       : (uint8_t)NANORTC_BWE_DIR_STABLE;
                            bwe_evt.bitrate_estimate.source = (uint8_t)NANORTC_BWE_SRC_REMB;
                            nano_rtc_emit_event_full(rtc, &bwe_evt);
                        }
                    } else
#endif
                        if (psfb_fmt == 1) {
                        /* PLI (RFC 4585 §6.3.1) — match the media-source SSRC
                         * (word 2) to one of our OUTBOUND video tracks, so the
                         * EV_KEYFRAME_REQUEST names the exact stream the peer can't
                         * decode. TD-024: the previous ssrc_map_lookup(sender SSRC)
                         * mapped to a receive track (or nothing for a sendonly
                         * sender) — wrong mid on multi-track, no event for a camera
                         * with no reverse video. */
                        for (uint8_t i = 0; i < rtc->media_count; i++) {
                            nanortc_track_t *vm = &rtc->media[i];
                            if (vm->active && vm->kind == NANORTC_TRACK_VIDEO &&
                                vm->rtp.ssrc == info.psfb_media_ssrc) {
                                nanortc_event_t kfevt;
                                memset(&kfevt, 0, sizeof(kfevt));
                                kfevt.type = NANORTC_EV_KEYFRAME_REQUEST;
                                kfevt.keyframe_request.mid = vm->mid;
                                nano_rtc_emit_event_full(rtc, &kfevt);
                                break;
                            }
                        }
                    }
#if NANORTC_FEATURE_VIDEO
                } else if (info.type == RTCP_RTPFB) {
                    /* Generic NACK (RFC 4585 §6.2.1) — retransmit lost packets
                     * from pkt_ring if they are still available. */
                    uint8_t rtpfb_fmt = sub[0] & 0x1F;
                    if (rtpfb_fmt == 1) {
                        /* Expand PID + BLP into up to 17 lost seq numbers and
                         * retransmit each one found in the pkt_ring. */
                        uint16_t lost[17];
                        int lost_count = 0;
                        lost[lost_count++] = info.nack_pid;
                        for (int bit = 0; bit < 16; bit++) {
                            if (info.nack_blp & (1u << bit)) {
                                lost[lost_count++] = (uint16_t)(info.nack_pid + 1 + bit);
                            }
                        }
                        int retx = 0;
                        for (int i = 0; i < lost_count; i++) {
                            /* Linear scan over pkt_ring_meta for a matching seq.
                             * PKT_RING_SIZE is small (4-256) so this is fast. */
                            for (uint16_t s = 0; s < NANORTC_VIDEO_PKT_RING_SIZE; s++) {
                                if (rtc->pkt_ring_meta[s].len == 0 ||
                                    rtc->pkt_ring_meta[s].seq != lost[i]) {
                                    continue;
                                }
                                /* TD-023: copy the packet into a free retransmit
                                 * scratch slot and enqueue the COPY, so a later
                                 * send_video that wraps pkt_ring over slot s cannot
                                 * corrupt this in-flight retransmit. A slot is free
                                 * once its prior output dequeued (out_head reached
                                 * its free_at) — same guard as the FEC tx ring. */
                                int8_t rslot = -1;
                                for (uint8_t j = 0; j < NANORTC_NACK_RETX_RING; j++) {
                                    if ((int16_t)(rtc->out_head - rtc->nack_retx_free_at[j]) >= 0) {
                                        rslot = (int8_t)j;
                                        break;
                                    }
                                }
                                if (rslot < 0) {
                                    break; /* all retx buffers in flight — best-effort */
                                }
                                uint16_t plen = rtc->pkt_ring_meta[s].len;
                                memcpy(rtc->nack_retx_buf[rslot], rtc->pkt_ring[s], plen);
                                if (nano_rtc_enqueue_transmit(rtc, rtc->nack_retx_buf[rslot], plen,
                                                              &rtc->remote_addr,
                                                              false) == NANORTC_OK) {
                                    rtc->nack_retx_free_at[rslot] = rtc->out_tail;
                                    retx++;
                                }
                                break;
                            }
                        }
                        if (retx > 0) {
                            NANORTC_LOGD("NACK", "retransmitted packet(s)");
                        }
                    } else if (rtpfb_fmt == TWCC_FMT) {
                        /* Transport-wide CC feedback (draft-holmer-rmcat-twcc-01).
                         * Parse into a summary and drive the loss-based controller
                         * in BWE. Any delay-based refinement is deferred (see plan). */
                        nano_twcc_summary_t sum;
                        int prc = twcc_parse_feedback(sub, sub_len, &sum, NULL, NULL);
                        if (prc == NANORTC_OK && sum.packet_status_count > 0) {
                            uint16_t lost =
                                (uint16_t)(sum.packet_status_count - sum.received_count);
                            uint16_t loss_q8 =
                                (uint16_t)(((uint32_t)lost * 256u) / sum.packet_status_count);
                            uint32_t prev_bps = rtc->bwe.estimated_bitrate;
                            bwe_on_twcc_loss(&rtc->bwe, loss_q8, rtc->now_ms);
                            if (bwe_should_emit_event(&rtc->bwe)) {
                                nanortc_event_t bwe_evt;
                                memset(&bwe_evt, 0, sizeof(bwe_evt));
                                bwe_evt.type = NANORTC_EV_BITRATE_ESTIMATE;
                                uint32_t cur_bps = rtc->bwe.estimated_bitrate;
                                bwe_evt.bitrate_estimate.bitrate_bps = cur_bps;
                                bwe_evt.bitrate_estimate.prev_bitrate_bps = prev_bps;
                                bwe_evt.bitrate_estimate.direction =
                                    (cur_bps > prev_bps)   ? (uint8_t)NANORTC_BWE_DIR_UP
                                    : (cur_bps < prev_bps) ? (uint8_t)NANORTC_BWE_DIR_DOWN
                                                           : (uint8_t)NANORTC_BWE_DIR_STABLE;
                                bwe_evt.bitrate_estimate.source =
                                    (uint8_t)NANORTC_BWE_SRC_TWCC_LOSS;
                                nano_rtc_emit_event_full(rtc, &bwe_evt);
                            }
                        }
                    }
#endif /* NANORTC_FEATURE_VIDEO */
                }
            }
            rtcp_off += sub_len;
        }
        return NANORTC_OK;
    }

    /* RTP packet — demux by SSRC → MID.
     * Use stun_buf as scratch for in-place SRTP unprotect: under Sans I/O
     * single-threaded invocation, STUN/RTCP/RTP use of stun_buf is
     * time-disjoint. In media builds stun_buf is sized to
     * NANORTC_MEDIA_BUF_SIZE (see nanortc_config.h), so a full RTP packet
     * fits; in DC-only builds this path is unreachable. */
    if (len > sizeof(rtc->stun_buf)) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    uint8_t *pkt = rtc->stun_buf;
    memcpy(pkt, data, len);
    size_t pkt_len = len;

    /* SRTP unprotect */
    size_t plain_len = 0;
    int src_rc = nano_srtp_unprotect(&rtc->srtp, pkt, pkt_len, &plain_len);
    if (src_rc != NANORTC_OK) {
        return NANORTC_OK; /* Silently discard bad SRTP packets */
    }

    /* Parse RTP header */
    uint8_t rtp_pt = 0;
    uint16_t rtp_seq = 0;
    uint32_t rtp_ts = 0;
    uint32_t rtp_ssrc = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int rrc =
        rtp_unpack(pkt, plain_len, &rtp_pt, &rtp_seq, &rtp_ts, &rtp_ssrc, &payload, &payload_len);
    if (rrc != NANORTC_OK) {
        return NANORTC_OK; /* Malformed RTP, discard */
    }

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC
    /* ULPFEC stream (separate SSRC = media+1, dynamic PT). It is not registered
     * in ssrc_map, so intercept by PT here and recover for the (single) video
     * track before the SSRC lookup discards it. Scope to UNREGISTERED SSRCs so a
     * registered media stream that happens to share the PT is never hijacked
     * (the FEC SSRC is never registered, so genuine FEC still matches). */
    if (rtp_pt == NANORTC_VIDEO_FEC_PT &&
        ssrc_map_lookup(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, rtp_ssrc) < 0) {
        for (uint8_t ti = 0; ti < rtc->media_count; ti++) {
            nanortc_track_t *vm = &rtc->media[ti];
            if (vm->active && vm->kind == NANORTC_TRACK_VIDEO) {
                rtc_fec_on_recv(rtc, vm, pkt, plain_len);
                break;
            }
        }
        return NANORTC_OK;
    }
#endif

    /* SSRC → MID lookup */
    int mid = ssrc_map_lookup(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, rtp_ssrc);
    if (mid < 0) {
        /* First-time SSRC discovery: try PT-based matching */
        for (uint8_t ti = 0; ti < rtc->media_count; ti++) {
            nanortc_track_t *mc = &rtc->media[ti];
            if (!mc->active)
                continue;
            nano_sdp_mline_t *ml = sdp_find_mline(&rtc->sdp, mc->mid);
            if (ml && ml->remote_pt == rtp_pt) {
#if NANORTC_FEATURE_VIDEO
                /* RFC 3550 §8.2: a mid-session SSRC change reusing this track (a
                 * remote sender restart). The reorder buffer / FU depacketizer
                 * still hold the OLD sequence space; re-seed them so the new
                 * RFC-3550-random start seq isn't blackholed by the reorder
                 * buffer's diff<0 late-drop. First discovery (remote_ssrc==0,
                 * already zero from track init) is left untouched. */
                if (mc->kind == NANORTC_TRACK_VIDEO && mc->rtcp.remote_ssrc != 0 &&
                    mc->rtcp.remote_ssrc != rtp_ssrc) {
#if NANORTC_FEATURE_VIDEO_REORDER
                    reorder_init(&mc->track.video.reorder);
#endif
#if NANORTC_FEATURE_VIDEO_AUTO_PLI
                    mc->track.video.recv_seq_inited = false;
#endif
#if NANORTC_FEATURE_VIDEO_NACK_RX
                    mc->track.video.recv_nack_inited = false;
#endif
#if NANORTC_FEATURE_VIDEO_FEC
                    /* Drop the old stream's FEC ring + any pending FEC: its
                     * sequence numbers belong to the previous SSRC and could be
                     * mismatched into a recovery for the new stream. */
                    rtc->fec_rx_n = 0;
                    rtc->fec_rx_pending_len = 0;
                    rtc->fec_prot_valid = false;
#endif
#if NANORTC_FEATURE_H265
                    if (mc->codec == NANORTC_CODEC_H265) {
                        h265_depkt_init(&mc->track.video.depkt.h265);
                    } else
#endif
                    {
                        h264_depkt_init(&mc->track.video.depkt.h264);
                    }
                }
#endif /* NANORTC_FEATURE_VIDEO */
                ssrc_map_register(rtc->ssrc_map, NANORTC_MAX_SSRC_MAP, rtp_ssrc, mc->mid);
                mc->rtcp.remote_ssrc = rtp_ssrc;
                mid = (int)mc->mid;
                break;
            }
        }
    }
    if (mid < 0) {
        return NANORTC_OK; /* Unknown SSRC/PT, discard */
    }

    nanortc_track_t *m = track_find_by_mid(rtc->media, rtc->media_count, (uint8_t)mid);
    if (!m) {
        return NANORTC_OK;
    }

    /* Update RTCP receiver stats */
    m->rtcp.packets_received++;
    if (rtp_seq > m->rtcp.max_seq || m->rtcp.packets_received == 1) {
        m->rtcp.max_seq = rtp_seq;
    }
    if (m->rtcp.remote_ssrc == 0) {
        m->rtcp.remote_ssrc = rtp_ssrc;
    }

    /* Route to audio or video processing */
    if (m->kind == NANORTC_TRACK_AUDIO) {
#if NANORTC_FEATURE_AUDIO
        /* Only BUFFER here. Due frames are popped one-per-poll_output by
         * nano_rtc_media_audio_produce() so each event is consumed before the
         * next reuses m->media_buf — a synchronous drain loop here would queue
         * multiple EV_MEDIA_DATA all aliasing m->media_buf (TD-025). */
        jitter_push(&m->track.audio.jitter, rtp_seq, rtp_ts, payload, payload_len, rtc->now_ms);
#endif
    } else {
#if NANORTC_FEATURE_VIDEO
        uint8_t rtp_marker = (pkt[1] >> 7) & 1;
#if NANORTC_FEATURE_VIDEO_FEC
        /* Keep the plaintext packet for FEC recovery; retries any pending FEC
         * whose group this packet completes. */
        rtc_fec_buffer_media(rtc, m, pkt, plain_len);
#endif
#if NANORTC_FEATURE_VIDEO_NACK_RX
        /* Receiver NACK (RFC 4585 §6.2.1): on a forward seq gap, ask the sender
         * to retransmit the lost packets from its pkt_ring. Independent of the
         * reorder/auto-PLI seq tracking. The reorder buffer (if enabled) holds
         * the gap so the retransmit can fill it within the latency cap. */
        if (!m->track.video.recv_nack_inited) {
            m->track.video.recv_nack_inited = true;
            m->track.video.recv_nack_seq = rtp_seq;
        } else {
            int16_t nadv = (int16_t)(rtp_seq - m->track.video.recv_nack_seq);
            if (nadv > 1) {
                /* Build the NACK over the lost run (recv_nack_seq+1 .. rtp_seq-1),
                 * but skip any SN a pending FEC will recover so FEC and NACK do
                 * not both request the same loss (RFC 5109 §10.1 + RFC 4585). */
                uint16_t first_lost = (uint16_t)(m->track.video.recv_nack_seq + 1);
                uint16_t n_lost = (uint16_t)(nadv - 1);
                uint16_t nack_pid = 0;
                uint16_t nack_blp = 0;
                bool have_pid = false;
                bool suppressed = false;
                for (uint16_t k = 0; k < n_lost && k < 17u; k++) {
                    uint16_t sn = (uint16_t)(first_lost + k);
#if NANORTC_FEATURE_VIDEO_FEC
                    if (rtc_fec_protects(rtc, sn)) {
                        suppressed = true;
                        continue;
                    }
#endif
                    if (!have_pid) {
                        nack_pid = sn;
                        have_pid = true;
                    } else {
                        uint16_t bit = (uint16_t)(sn - nack_pid - 1u);
                        if (bit < 16u) {
                            nack_blp |= (uint16_t)(1u << bit);
                        }
                    }
                }
#if NANORTC_FEATURE_VIDEO_FEC
                if (suppressed) {
                    __atomic_fetch_add(&rtc->stats_nack_suppressed_fec, 1, __ATOMIC_RELAXED);
                }
#endif
                if (have_pid) {
                    rtc_emit_nack(rtc, m, nack_pid, nack_blp);
                }
                m->track.video.recv_nack_seq = rtp_seq;
            } else if (nadv > 0) {
                m->track.video.recv_nack_seq = rtp_seq;
            }
        }
#endif /* NANORTC_FEATURE_VIDEO_NACK_RX */
#if NANORTC_FEATURE_VIDEO_REORDER
        /* Reorder buffer: only BUFFER the packet here. Released NALs are drained
         * one-per-poll_output by nano_rtc_media_reorder_produce() so each event
         * is consumed before the next reuses the shared depkt buffer, and
         * nanortc_next_timeout_ms() arms the skip deadline so a held frame
         * flushes even if no further RTP arrives. The buffer's skip is the
         * precise loss signal (a genuine gap, not a reorder). */
        reorder_push(&m->track.video.reorder, rtp_seq, rtp_ts, rtp_marker, payload, payload_len,
                     rtc->now_ms);
#else
        /* No reorder buffer: raw forward-gap detection is the loss signal.
         * (int16_t)(seq - last) handles the 16-bit RTP wrap (RFC 3550 §5.1):
         *   > 1 forward gap (lost); == 1 in order; <= 0 late/reordered (ignored
         *   here but still delivered to the depacketizer in arrival order).
         * Without the reorder buffer a reorder reads as loss; the auto-PLI
         * debounce (NANORTC_VIDEO_PLI_MIN_INTERVAL_MS) caps the cost. */
        bool lost = false;
#if NANORTC_FEATURE_VIDEO_AUTO_PLI
        if (!m->track.video.recv_seq_inited) {
            m->track.video.recv_seq_inited = true;
            m->track.video.recv_last_seq = rtp_seq;
        } else {
            int16_t adv = (int16_t)(rtp_seq - m->track.video.recv_last_seq);
            if (adv > 1) {
                m->track.video.recv_last_seq = rtp_seq;
                lost = true;
            } else if (adv > 0) {
                m->track.video.recv_last_seq = rtp_seq;
            }
        }
#endif
        rtc_video_deliver(rtc, m, payload, payload_len, rtp_ts, rtp_marker, lost);
#endif /* NANORTC_FEATURE_VIDEO_REORDER */
#endif /* NANORTC_FEATURE_VIDEO */
    }
    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * Periodic RTCP Sender Report cadence (RFC 3550 §6.2). Called once per
 * timer tick from rtc_process_timers() in nano_rtc.c. Cadence-gates on
 * rtc->last_rtcp_send_ms internally; no-op when SRTP is not ready.
 * ---------------------------------------------------------------- */

void nano_rtc_media_emit_rtcp_sr_cadence(nanortc_t *rtc, uint32_t now_ms)
{
    if (!(rtc->srtp.ready && (now_ms - rtc->last_rtcp_send_ms) >= NANORTC_RTCP_INTERVAL_MS)) {
        return;
    }
    rtc->last_rtcp_send_ms = now_ms;

    /* NTP timestamp from monotonic now_ms (RFC 3550 §4):
     * No wall-clock available in Sans I/O; relative time is sufficient
     * for DLSR calculation at the receiver. */
    uint32_t ntp_sec = now_ms / 1000;
    uint32_t ntp_frac = (uint32_t)((uint64_t)(now_ms % 1000) * 4294967u);

    /* Emit at most ONE SR per cadence tick. The SR is built in the single
     * shared rtc->stun_buf and enqueued by pointer (out_queue stores only a
     * pointer per slot), so enqueuing two SRs in one call would alias stun_buf
     * and the receiver would see only the last track's SR — the exact aliasing
     * the turn_buf/stun_buf split was built to avoid. Round-robin the starting
     * track via rtc->sr_cursor so every sending track still emits periodic SRs;
     * each track's effective interval stretches by the number of sending tracks
     * (well within RFC 3550 §6.2 tolerance for the small counts supported). */
    if (rtc->media_count == 0) {
        return;
    }
    for (uint8_t scanned = 0; scanned < rtc->media_count; scanned++) {
        uint8_t ti = (uint8_t)((rtc->sr_cursor + scanned) % rtc->media_count);
        nanortc_track_t *m = &rtc->media[ti];
        if (!m->active)
            continue;
        /* Only send SR for tracks that are sending */
        if (m->direction == NANORTC_DIR_RECVONLY || m->direction == NANORTC_DIR_INACTIVE)
            continue;
        if (m->rtcp.packets_sent == 0)
            continue;

        /* RTP timestamp corresponding to NTP time */
        uint32_t clock_rate = (m->kind == NANORTC_TRACK_VIDEO) ? 90000 : m->sample_rate;
        uint32_t rtp_ts = (uint32_t)((uint64_t)now_ms * clock_rate / 1000);

        /* Generate SR + SRTCP protect into stun_buf (safe: ICE checks
         * only run when NOT connected, see guard above) */
        size_t sr_len = 0;
        int sr_rc = rtcp_generate_sr(&m->rtcp, ntp_sec, ntp_frac, rtp_ts, rtc->stun_buf,
                                     sizeof(rtc->stun_buf), &sr_len);
        if (sr_rc != NANORTC_OK)
            continue;

        size_t srtcp_len = 0;
        sr_rc = nano_srtp_protect_rtcp(&rtc->srtp, rtc->stun_buf, sr_len, &srtcp_len);
        if (sr_rc != NANORTC_OK)
            continue;

        nano_rtc_enqueue_transmit(rtc, rtc->stun_buf, srtcp_len, &rtc->remote_addr, false);
        /* Resume after this track next tick so SR coverage rotates fairly. */
        rtc->sr_cursor = (uint8_t)((ti + 1) % rtc->media_count);
        return;
    }
}

#else /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* CORE_ONLY / DATA-only profiles: keep the TU non-empty for strict ISO C. */
typedef int nano_rtc_media_unused;

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */
