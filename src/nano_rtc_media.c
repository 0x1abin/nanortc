/*
 * nanortc — Media send paths
 *
 * Owns audio/video packetization, the video pkt_ring scratch ring, the
 * track add / direction APIs, the video keyframe-request (PLI) feedback
 * path, and the BWE runtime knobs / per-track stats accessors.
 *
 * Lifted out of nano_rtc.c by Phase 10 PR-4 slice 2: the trailing media
 * region of nano_rtc.c was already feature-guarded under
 * NANORTC_HAVE_MEDIA_TRANSPORT and only touched media-owned state on
 * nanortc_t (`media[]`, `pkt_ring[]`, `pkt_ring_meta[]`, `bwe`, `srtp`).
 * Moving it here shrinks nano_rtc.c without changing the public API, the
 * module dependency graph, or the output-queue lifetime contract on
 * `nanortc_output_t` (see include/nanortc.h).
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
#if NANORTC_FEATURE_H265
#include "nano_h265.h"
#include "nano_base64.h"
#include "nano_annex_b.h"
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

    ctx->last_rc = nano_rtc_enqueue_transmit(rtc, pkt_buf, srtp_len, &rtc->remote_addr, false);
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

        rc = nano_rtc_enqueue_transmit(rtc, pkt_buf, srtp_len, &rtc->remote_addr, false);
        if (rc != NANORTC_OK) {
            return rc;
        }
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

    /* One call = one encoded frame for fps accounting. Callers that split
     * a frame across multiple send calls will over-report, which we accept
     * because the API contract is "caller passes one complete frame". */
    rate_window_on_frame(&m->rate_window, rtc->now_ms);

    uint32_t ts = pts_ms_to_rtp(pts_ms, 90000);
    const uint8_t *buf = (const uint8_t *)data;

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

    /* Generate PLI (RFC 4585 §6.3.1) */
    uint8_t pli_buf[RTCP_PLI_SIZE + NANORTC_SRTP_AUTH_TAG_SIZE + 4];
    size_t pli_len = 0;
    int rc =
        rtcp_generate_pli(m->rtcp.ssrc, m->rtcp.remote_ssrc, pli_buf, sizeof(pli_buf), &pli_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* SRTCP protect (RFC 3711 §3.4) */
    size_t srtcp_len = 0;
    int prc = nano_srtp_protect_rtcp(&rtc->srtp, pli_buf, pli_len, &srtcp_len);
    if (prc != NANORTC_OK) {
        return prc;
    }

    return nano_rtc_enqueue_transmit(rtc, pli_buf, srtcp_len, &rtc->remote_addr, false);
}

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

#else /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* CORE_ONLY / DATA-only profiles: keep the TU non-empty for strict ISO C. */
typedef int nano_rtc_media_unused;

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */
