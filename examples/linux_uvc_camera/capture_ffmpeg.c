/*
 * capture_ffmpeg — FFmpeg backend for capture.h
 *
 * Pipeline:
 *   V4L2 capture (MJPEG or YUYV) → decode → swscale (→ NV12)
 *     → ffmpeg encoder → Annex-B callback
 *
 * The encoder name is passed through capture_config_t.encoder and
 * resolved by ffmpeg at runtime, so one backend covers every libav
 * encoder. Known-good names:
 *   - libx264     software H.264 (universal default, no GPU needed)
 *   - hevc_nvenc  NVIDIA GPU H.265   (build the library with H265=ON)
 *   - h264_nvenc  NVIDIA GPU H.264
 *   - h264_rkmpp  Rockchip MPP hardware H.264 (RK3399/RK356x/RK3576/RK3588…)
 * Unknown names fall back to libx264.
 *
 * Many USB3 4K UVC cameras emit corrupt frames in raw YUYV at >=720p;
 * capture MJPEG ("-i mjpeg", the default) and let ffmpeg decode it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "capture.h"

/* nanortc-internal Annex-B / H.265 helpers for parameter-set extraction.
 * The example adds ${CMAKE_SOURCE_DIR}/src to its include path and links
 * libnanortc, where nano_annex_b_find_nal() is a non-static symbol. */
#include "nano_annex_b.h"
#include "nano_h265.h"

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Module state (single-instance)
 * ---------------------------------------------------------------- */

static struct {
    /* V4L2 capture + input decode */
    AVFormatContext *ifmt_ctx;
    int video_stream_idx;
    AVCodecContext *dec_ctx; /* MJPEG or rawvideo decoder */

    /* Pixel format conversion (created lazily on first decoded frame) */
    struct SwsContext *sws;
    int sws_src_w, sws_src_h, sws_src_fmt;
    AVFrame *enc_frame; /* NV12 frame fed to encoder */

    /* Encoder */
    AVCodecContext *enc_ctx;
    bool is_h265;

    /* Cached parameter sets (Annex-B) for prepending to IDR frames */
    uint8_t *sps_pps;
    int sps_pps_size;

    /* H.265 VPS/SPS/PPS parsed as raw NALs (point into sps_pps or psets_buf) */
    const uint8_t *vps;
    size_t vps_len;
    const uint8_t *sps;
    size_t sps_len;
    const uint8_t *pps;
    size_t pps_len;
    uint8_t *psets_buf; /* fallback storage if parsed from first keyframe */
    volatile int psets_ready;

    /* Callback */
    capture_encoder_cb callback;
    void *userdata;

    /* Thread control */
    pthread_t thread;
    volatile int running;
    volatile int force_idr;
    volatile int pending_bps; /* staged BWE target, applied on encode thread */

    int enc_w, enc_h, fps;
} g_ff;

/* ----------------------------------------------------------------
 * H.265 parameter-set parsing
 * ---------------------------------------------------------------- */

/* Walk an Annex-B buffer and record raw VPS(32)/SPS(33)/PPS(34) NAL
 * pointers (excluding start codes). Returns true if all three found. */
static bool h265_split_param_sets(const uint8_t *buf, size_t len, const uint8_t **vps,
                                  size_t *vps_len, const uint8_t **sps, size_t *sps_len,
                                  const uint8_t **pps, size_t *pps_len)
{
    *vps = *sps = *pps = NULL;
    *vps_len = *sps_len = *pps_len = 0;
    int mask = 0;
    size_t off = 0;
    while (off < len) {
        size_t nal_len = 0;
        const uint8_t *nal = nano_annex_b_find_nal(buf, len, &off, &nal_len);
        if (!nal || nal_len < H265_NAL_HEADER_SIZE)
            break;
        uint8_t type = H265_NAL_TYPE(nal);
        if (type == H265_NAL_VPS_NUT && !(mask & 0x1)) {
            *vps = nal;
            *vps_len = nal_len;
            mask |= 0x1;
        } else if (type == H265_NAL_SPS_NUT && !(mask & 0x2)) {
            *sps = nal;
            *sps_len = nal_len;
            mask |= 0x2;
        } else if (type == H265_NAL_PPS_NUT && !(mask & 0x4)) {
            *pps = nal;
            *pps_len = nal_len;
            mask |= 0x4;
        }
    }
    return mask == 0x7;
}

/* Primary path: parse VPS/SPS/PPS from the encoder extradata (Annex-B,
 * populated because we keep AV_CODEC_FLAG_GLOBAL_HEADER for nvenc). */
static void cache_h265_param_sets_from_extradata(void)
{
    if (!g_ff.is_h265 || !g_ff.sps_pps || g_ff.sps_pps_size <= 0)
        return;
    if (g_ff.sps_pps[0] == 0x01) {
        /* hvcC (length-prefixed) rather than Annex-B — the start-code
         * walker would find nothing. Fall back to first-keyframe parse. */
        fprintf(stderr, "[ffcap] H265 extradata is hvcC; deferring to keyframe parse\n");
        return;
    }
    if (h265_split_param_sets(g_ff.sps_pps, (size_t)g_ff.sps_pps_size, &g_ff.vps, &g_ff.vps_len,
                              &g_ff.sps, &g_ff.sps_len, &g_ff.pps, &g_ff.pps_len)) {
        g_ff.psets_ready = 1;
        fprintf(stderr, "[ffcap] H265 parameter sets: VPS=%zu SPS=%zu PPS=%zu (from extradata)\n",
                g_ff.vps_len, g_ff.sps_len, g_ff.pps_len);
    }
}

/* Fallback path: parse VPS/SPS/PPS from the first keyframe access unit and
 * copy them into module-owned storage (the keyframe buffer is transient). */
static void cache_h265_param_sets_from_keyframe(const uint8_t *au, size_t len)
{
    const uint8_t *v, *s, *p;
    size_t vl, sl, pl;
    if (!h265_split_param_sets(au, len, &v, &vl, &s, &sl, &p, &pl))
        return;
    uint8_t *buf = malloc(vl + sl + pl);
    if (!buf)
        return;
    memcpy(buf, v, vl);
    memcpy(buf + vl, s, sl);
    memcpy(buf + vl + sl, p, pl);
    g_ff.psets_buf = buf;
    g_ff.vps = buf;
    g_ff.vps_len = vl;
    g_ff.sps = buf + vl;
    g_ff.sps_len = sl;
    g_ff.pps = buf + vl + sl;
    g_ff.pps_len = pl;
    g_ff.psets_ready = 1;
    fprintf(stderr, "[ffcap] H265 parameter sets: VPS=%zu SPS=%zu PPS=%zu (from keyframe)\n", vl,
            sl, pl);
}

/* ----------------------------------------------------------------
 * Capture + decode + encode thread
 * ---------------------------------------------------------------- */

static void emit_packet(AVPacket *opkt, int frame_count)
{
    bool is_key = (opkt->flags & AV_PKT_FLAG_KEY) != 0;

    int64_t pts_ms = 0;
    if (opkt->pts != AV_NOPTS_VALUE) {
        AVRational tb = g_ff.enc_ctx->time_base;
        pts_ms = av_rescale_q(opkt->pts, tb, (AVRational){1, 1000});
    }

    if (is_key && g_ff.sps_pps && g_ff.sps_pps_size > 0) {
        /* Prepend cached parameter sets before the IDR (in-band delivery,
         * like header-mode=each-idr). Build [VPS/SPS/PPS][IDR]. */
        size_t total = (size_t)g_ff.sps_pps_size + opkt->size;
        uint8_t *combined = malloc(total);
        if (combined) {
            memcpy(combined, g_ff.sps_pps, g_ff.sps_pps_size);
            memcpy(combined + g_ff.sps_pps_size, opkt->data, opkt->size);
            if (g_ff.is_h265 && !g_ff.psets_ready)
                cache_h265_param_sets_from_keyframe(combined, total);
            if (g_ff.callback)
                g_ff.callback(g_ff.userdata, combined, total, (uint32_t)pts_ms, true);
            free(combined);
        }
    } else {
        bool is_keyframe =
            is_key || (g_ff.is_h265 ? capture_annex_b_is_keyframe_h265(opkt->data, opkt->size)
                                    : capture_annex_b_is_keyframe_h264(opkt->data, opkt->size));
        if (g_ff.is_h265 && is_keyframe && !g_ff.psets_ready)
            cache_h265_param_sets_from_keyframe(opkt->data, opkt->size);
        if (g_ff.callback)
            g_ff.callback(g_ff.userdata, opkt->data, opkt->size, (uint32_t)pts_ms, is_keyframe);
    }

    if (frame_count == 1 || (frame_count % 300) == 0)
        fprintf(stderr, "[ffcap] frame #%d (%d bytes%s)\n", frame_count - 1, opkt->size,
                is_key ? " KEY" : "");
}

static int encode_frame(AVFrame *nv12, int *frame_count, AVPacket *opkt)
{
    /* Apply any BWE-staged bitrate change in-place before encoding. nvenc's
     * reconfig_encoder() picks up changed bit_rate/rc_max_rate on send. */
    int pend = g_ff.pending_bps;
    if (pend > 0) {
        g_ff.pending_bps = 0;
        g_ff.enc_ctx->bit_rate = pend;
        g_ff.enc_ctx->rc_max_rate = pend; /* CBR: ceiling == average */
        g_ff.enc_ctx->rc_buffer_size = pend;
    }

    nv12->pts = (*frame_count)++;
    if (g_ff.force_idr) {
        nv12->pict_type = AV_PICTURE_TYPE_I;
        g_ff.force_idr = 0;
    } else {
        nv12->pict_type = AV_PICTURE_TYPE_NONE;
    }

    int ret = avcodec_send_frame(g_ff.enc_ctx, nv12);
    if (ret < 0) {
        fprintf(stderr, "[ffcap] avcodec_send_frame: %d\n", ret);
        return ret;
    }
    while ((ret = avcodec_receive_packet(g_ff.enc_ctx, opkt)) == 0) {
        emit_packet(opkt, *frame_count);
        av_packet_unref(opkt);
    }
    return 0;
}

/* Lazily (re)create the swscale context once we know the decoded frame
 * geometry/format. Converts whatever the decoder produces into NV12. */
static int ensure_sws(const AVFrame *src)
{
    if (g_ff.sws && g_ff.sws_src_w == src->width && g_ff.sws_src_h == src->height &&
        g_ff.sws_src_fmt == src->format)
        return 0;
    if (g_ff.sws) {
        sws_freeContext(g_ff.sws);
        g_ff.sws = NULL;
    }
    g_ff.sws = sws_getContext(src->width, src->height, (enum AVPixelFormat)src->format, g_ff.enc_w,
                              g_ff.enc_h, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_ff.sws) {
        fprintf(stderr, "[ffcap] sws_getContext failed\n");
        return -1;
    }
    /* MJPEG decodes to full-range (yuvj*) — tell swscale so the NV12 output
     * is not washed out. */
    {
        int *inv_table, *table, srcRange, dstRange, brightness, contrast, saturation;
        if (sws_getColorspaceDetails(g_ff.sws, &inv_table, &srcRange, &table, &dstRange,
                                     &brightness, &contrast, &saturation) >= 0) {
            const int *coefs = sws_getCoefficients(SWS_CS_ITU709);
            sws_setColorspaceDetails(g_ff.sws, coefs, 1 /* src full range */, coefs,
                                     0 /* dst limited range */, brightness, contrast, saturation);
        }
    }
    g_ff.sws_src_w = src->width;
    g_ff.sws_src_h = src->height;
    g_ff.sws_src_fmt = src->format;
    return 0;
}

static void *capture_thread(void *arg)
{
    (void)arg;
    AVPacket *ipkt = av_packet_alloc();
    AVPacket *opkt = av_packet_alloc();
    AVFrame *dec_frame = av_frame_alloc();
    int frame_count = 0;

    if (!ipkt || !opkt || !dec_frame) {
        fprintf(stderr, "[ffcap] alloc failed\n");
        goto out;
    }

    while (g_ff.running) {
        int ret = av_read_frame(g_ff.ifmt_ctx, ipkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                continue;
            fprintf(stderr, "[ffcap] av_read_frame: %d\n", ret);
            break;
        }
        if (ipkt->stream_index != g_ff.video_stream_idx) {
            av_packet_unref(ipkt);
            continue;
        }
        /* v4l2 intermittently dequeues empty/corrupt buffers (logged by the
         * demuxer as "corrupted data (0 bytes)"). Skip them rather than feed
         * the decoder a 0-byte packet, which fails with EINVAL. */
        if (ipkt->size == 0) {
            av_packet_unref(ipkt);
            continue;
        }

        /* Decode the captured packet (MJPEG → YUVJ*, or rawvideo passthrough). */
        ret = avcodec_send_packet(g_ff.dec_ctx, ipkt);
        av_packet_unref(ipkt);
        if (ret < 0) {
            fprintf(stderr, "[ffcap] decode send_packet: %d\n", ret);
            continue;
        }
        while ((ret = avcodec_receive_frame(g_ff.dec_ctx, dec_frame)) == 0) {
            if (ensure_sws(dec_frame) == 0) {
                sws_scale(g_ff.sws, (const uint8_t *const *)dec_frame->data, dec_frame->linesize, 0,
                          dec_frame->height, g_ff.enc_frame->data, g_ff.enc_frame->linesize);
                encode_frame(g_ff.enc_frame, &frame_count, opkt);
            }
            av_frame_unref(dec_frame);
        }
    }

out:
    av_packet_free(&ipkt);
    av_packet_free(&opkt);
    av_frame_free(&dec_frame);
    return NULL;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

static int open_input(const capture_config_t *cfg)
{
    avdevice_register_all();

    const AVInputFormat *v4l2fmt = av_find_input_format("v4l2");
    if (!v4l2fmt) {
        fprintf(stderr, "[ffcap] v4l2 input format not found\n");
        return -1;
    }

    const char *infmt = (cfg->input_format && cfg->input_format[0]) ? cfg->input_format : "mjpeg";

    AVDictionary *opts = NULL;
    char vsize[32], vfps[16];
    snprintf(vsize, sizeof(vsize), "%dx%d", cfg->width, cfg->height);
    snprintf(vfps, sizeof(vfps), "%d", cfg->fps);
    av_dict_set(&opts, "video_size", vsize, 0);
    av_dict_set(&opts, "framerate", vfps, 0);
    av_dict_set(&opts, "input_format", infmt, 0);

    int ret = avformat_open_input(&g_ff.ifmt_ctx, cfg->device, v4l2fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffcap] cannot open %s (%s): %s\n", cfg->device, infmt, errbuf);
        return -1;
    }

    if (avformat_find_stream_info(g_ff.ifmt_ctx, NULL) < 0) {
        fprintf(stderr, "[ffcap] find_stream_info failed\n");
        return -1;
    }

    g_ff.video_stream_idx = -1;
    for (unsigned i = 0; i < g_ff.ifmt_ctx->nb_streams; i++) {
        if (g_ff.ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_ff.video_stream_idx = (int)i;
            break;
        }
    }
    if (g_ff.video_stream_idx < 0) {
        fprintf(stderr, "[ffcap] no video stream found\n");
        return -1;
    }

    AVStream *st = g_ff.ifmt_ctx->streams[g_ff.video_stream_idx];
    AVCodecParameters *par = st->codecpar;
    fprintf(stderr, "[ffcap] capture: %dx%d fmt/codec=%d (%s)\n", par->width, par->height,
            par->codec_id, infmt);

    /* Decoder for the captured stream (mjpeg or rawvideo). */
    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        fprintf(stderr, "[ffcap] no decoder for codec_id %d\n", par->codec_id);
        return -1;
    }
    g_ff.dec_ctx = avcodec_alloc_context3(dec);
    if (!g_ff.dec_ctx || avcodec_parameters_to_context(g_ff.dec_ctx, par) < 0) {
        fprintf(stderr, "[ffcap] decoder context init failed\n");
        return -1;
    }
    g_ff.dec_ctx->thread_count = 1;
    if (avcodec_open2(g_ff.dec_ctx, dec, NULL) < 0) {
        fprintf(stderr, "[ffcap] decoder open failed\n");
        return -1;
    }
    return 0;
}

static int open_encoder(const capture_config_t *cfg, int width, int height)
{
    const char *enc_name = (cfg->encoder && cfg->encoder[0]) ? cfg->encoder : "hevc_nvenc";
    const AVCodec *encoder = avcodec_find_encoder_by_name(enc_name);
    if (!encoder) {
        fprintf(stderr, "[ffcap] encoder '%s' not found, trying libx264\n", enc_name);
        encoder = avcodec_find_encoder_by_name("libx264");
    }
    if (!encoder) {
        fprintf(stderr, "[ffcap] no usable encoder available\n");
        return -1;
    }
    fprintf(stderr, "[ffcap] encoder: %s\n", encoder->name);

    g_ff.is_h265 = (strstr(encoder->name, "hevc") != NULL) ||
                   (strstr(encoder->name, "h265") != NULL) ||
                   (strstr(encoder->name, "265") != NULL);

    g_ff.enc_ctx = avcodec_alloc_context3(encoder);
    g_ff.enc_ctx->width = width;
    g_ff.enc_ctx->height = height;
    g_ff.enc_ctx->pix_fmt = AV_PIX_FMT_NV12; /* nvenc auto-uploads sw NV12 */
    g_ff.enc_ctx->time_base = (AVRational){1, cfg->fps};
    g_ff.enc_ctx->framerate = (AVRational){cfg->fps, 1};
    g_ff.enc_ctx->bit_rate = cfg->bitrate_bps;
    g_ff.enc_ctx->rc_max_rate = cfg->bitrate_bps; /* CBR */
    g_ff.enc_ctx->rc_buffer_size = cfg->bitrate_bps;

    int gop = cfg->fps * (cfg->keyframe_interval_s > 0 ? cfg->keyframe_interval_s : 2);
    g_ff.enc_ctx->gop_size = gop;
    g_ff.enc_ctx->max_b_frames = 0; /* low latency: no B-frames */
    g_ff.enc_ctx->thread_count = 1;

    int is_nvenc = (strstr(encoder->name, "nvenc") != NULL);
    if (is_nvenc) {
        /* nvenc carries parameter sets in extradata when GLOBAL_HEADER is
         * set — we use that for both SDP sprop-* and per-IDR in-band prepend. */
        g_ff.enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *eopts = NULL; /* opened via avcodec_open2 dict below */
        (void)eopts;
        av_opt_set(g_ff.enc_ctx->priv_data, "preset", "p4", 0); /* p1(fast)..p7 */
        av_opt_set(g_ff.enc_ctx->priv_data, "tune", "ll", 0);   /* low latency */
        av_opt_set(g_ff.enc_ctx->priv_data, "rc", "cbr", 0);
        av_opt_set(g_ff.enc_ctx->priv_data, "profile", g_ff.is_h265 ? "main" : "high", 0);
        av_opt_set_int(g_ff.enc_ctx->priv_data, "zerolatency", 1, 0);
        av_opt_set_int(g_ff.enc_ctx->priv_data, "forced-idr", 1, 0); /* PLI → true IDR */
        av_opt_set_int(g_ff.enc_ctx->priv_data, "gpu", 0, 0);        /* pin to GPU 0 */
        /* Real-time anti-stutter: replace the periodic full IDR with a rolling
         * intra-refresh wave spread over the GOP. A 4K IDR is ~230KB (~190 RTP
         * fragments); on a constrained uplink the send pacer cannot drain it
         * within its latency cap and burst-flushes the tail every GOP (~2s) →
         * periodic stutter. Intra-refresh keeps every frame ~uniform in size so
         * there is no big keyframe to burst. forced-idr above still emits a true
         * IDR on join / PLI so a new viewer or loss recovery starts clean. */
        int ir_rc = av_opt_set_int(g_ff.enc_ctx->priv_data, "intra-refresh", 1, 0);
        fprintf(stderr, "[ffcap] intra-refresh %s\n", ir_rc == 0 ? "enabled" : "unavailable");
    } else {
        /* Software / non-nvenc: inline Annex-B headers, prepend cached SPS/PPS. */
        g_ff.enc_ctx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER;
        if (strcmp(encoder->name, "libx264") == 0) {
            av_opt_set(g_ff.enc_ctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(g_ff.enc_ctx->priv_data, "tune", "zerolatency", 0);
        } else if (strcmp(encoder->name, "h264_rkmpp") == 0) {
            /* Rockchip MPP hardware H.264 (RK3399/RK356x/RK3576/RK3588…). */
            av_opt_set(g_ff.enc_ctx->priv_data, "rc_mode", "CBR", 0);
            av_opt_set(g_ff.enc_ctx->priv_data, "profile", "baseline", 0);
            av_opt_set(g_ff.enc_ctx->priv_data, "level", "51", 0);
        }
    }

    int ret = avcodec_open2(g_ff.enc_ctx, encoder, NULL);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffcap] avcodec_open2 failed: %s\n", errbuf);
        return -1;
    }

    if (g_ff.enc_ctx->extradata && g_ff.enc_ctx->extradata_size > 0) {
        g_ff.sps_pps_size = g_ff.enc_ctx->extradata_size;
        g_ff.sps_pps = malloc(g_ff.sps_pps_size);
        if (g_ff.sps_pps)
            memcpy(g_ff.sps_pps, g_ff.enc_ctx->extradata, g_ff.sps_pps_size);
        cache_h265_param_sets_from_extradata();
    }

    fprintf(stderr, "[ffcap] pipeline: %s %dx%d@%dfps %d bps GOP %d codec=%s\n", encoder->name,
            width, height, cfg->fps, cfg->bitrate_bps, gop, g_ff.is_h265 ? "H265" : "H264");
    return 0;
}

int capture_start(const capture_config_t *cfg)
{
    if (!cfg || !cfg->device || !cfg->callback) {
        fprintf(stderr, "[ffcap] invalid config\n");
        return -1;
    }
    if (g_ff.running) {
        fprintf(stderr, "[ffcap] already running\n");
        return -1;
    }

    memset(&g_ff, 0, sizeof(g_ff));
    g_ff.callback = cfg->callback;
    g_ff.userdata = cfg->userdata;
    g_ff.video_stream_idx = -1;
    g_ff.fps = cfg->fps;

    if (open_input(cfg) < 0) {
        capture_stop();
        return -1;
    }

    AVCodecParameters *par = g_ff.ifmt_ctx->streams[g_ff.video_stream_idx]->codecpar;
    g_ff.enc_w = par->width;
    g_ff.enc_h = par->height;

    /* Encoder input frame (NV12) at the capture resolution. */
    g_ff.enc_frame = av_frame_alloc();
    g_ff.enc_frame->format = AV_PIX_FMT_NV12;
    g_ff.enc_frame->width = g_ff.enc_w;
    g_ff.enc_frame->height = g_ff.enc_h;
    if (av_frame_get_buffer(g_ff.enc_frame, 32) < 0) {
        fprintf(stderr, "[ffcap] av_frame_get_buffer failed\n");
        capture_stop();
        return -1;
    }

    if (open_encoder(cfg, g_ff.enc_w, g_ff.enc_h) < 0) {
        capture_stop();
        return -1;
    }

    g_ff.running = 1;
    if (pthread_create(&g_ff.thread, NULL, capture_thread, NULL) != 0) {
        fprintf(stderr, "[ffcap] pthread_create failed\n");
        g_ff.running = 0;
        capture_stop();
        return -1;
    }
    return 0;
}

void capture_stop(void)
{
    if (g_ff.running) {
        g_ff.running = 0;
        pthread_join(g_ff.thread, NULL);
    }
    if (g_ff.enc_ctx)
        avcodec_free_context(&g_ff.enc_ctx);
    if (g_ff.dec_ctx)
        avcodec_free_context(&g_ff.dec_ctx);
    if (g_ff.enc_frame)
        av_frame_free(&g_ff.enc_frame);
    if (g_ff.sws) {
        sws_freeContext(g_ff.sws);
        g_ff.sws = NULL;
    }
    if (g_ff.ifmt_ctx)
        avformat_close_input(&g_ff.ifmt_ctx);
    free(g_ff.sps_pps);
    g_ff.sps_pps = NULL;
    g_ff.sps_pps_size = 0;
    free(g_ff.psets_buf);
    g_ff.psets_buf = NULL;
    g_ff.psets_ready = 0;
    g_ff.callback = NULL;
    g_ff.userdata = NULL;
}

void capture_force_keyframe(void)
{
    g_ff.force_idr = 1;
}

int capture_set_bitrate(int bps)
{
    if (!g_ff.enc_ctx || bps <= 0)
        return -1;
    /* Stage the new target; the capture thread applies it in-place just
     * before the next avcodec_send_frame() so the encoder context is never
     * mutated concurrently with an encode. */
    g_ff.pending_bps = bps;
    return 0;
}

int capture_get_h265_parameter_sets(const uint8_t **vps, size_t *vps_len, const uint8_t **sps,
                                    size_t *sps_len, const uint8_t **pps, size_t *pps_len)
{
    if (!g_ff.is_h265 || !g_ff.psets_ready)
        return -1;
    *vps = g_ff.vps;
    *vps_len = g_ff.vps_len;
    *sps = g_ff.sps;
    *sps_len = g_ff.sps_len;
    *pps = g_ff.pps;
    *pps_len = g_ff.pps_len;
    return 0;
}
