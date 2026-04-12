/*
 * capture_ffmpeg — FFmpeg backend for capture.h
 *
 * Compile with RK3588_CAPTURE_FFMPEG defined.
 * Default encoder: h264_rkmpp (RK3588 hardware via rockchip MPP).
 * Fallback: libx264 (software).
 *
 * Pipeline:
 *   v4l2 capture (YUYV) → swscale (→ NV12) → h264_rkmpp encode → Annex-B callback
 *
 * SPDX-License-Identifier: MIT
 */

#include "capture.h"

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Module state (single-instance)
 * ---------------------------------------------------------------- */

static struct {
    /* V4L2 capture */
    AVFormatContext *ifmt_ctx;
    int video_stream_idx;

    /* Pixel format conversion */
    struct SwsContext *sws;
    AVFrame *enc_frame; /* NV12 frame fed to encoder */

    /* H.264 encoder */
    AVCodecContext *enc_ctx;
    uint8_t *sps_pps;     /* cached extradata (SPS/PPS in Annex-B) */
    int sps_pps_size;

    /* Callback */
    capture_encoder_cb callback;
    void *userdata;

    /* Thread control */
    pthread_t thread;
    volatile int running;
    volatile int force_idr;
} g_ff;

/* ----------------------------------------------------------------
 * Capture + encode thread
 * ---------------------------------------------------------------- */
static void *capture_thread(void *arg)
{
    (void)arg;
    AVPacket *ipkt = av_packet_alloc();
    AVPacket *opkt = av_packet_alloc();
    AVFrame *raw_frame = av_frame_alloc();
    int frame_count = 0;

    if (!ipkt || !opkt || !raw_frame) {
        fprintf(stderr, "[ffcap] alloc failed\n");
        goto out;
    }

    while (g_ff.running) {
        /* 1. Read raw frame from V4L2 */
        int ret = av_read_frame(g_ff.ifmt_ctx, ipkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) continue;
            fprintf(stderr, "[ffcap] av_read_frame: %d\n", ret);
            break;
        }
        if (ipkt->stream_index != g_ff.video_stream_idx) {
            av_packet_unref(ipkt);
            continue;
        }

        /* 2. Decode V4L2 raw frame (YUYV is raw, no actual decoding needed —
         *    but we must wrap the packet data into an AVFrame for swscale). */
        AVStream *st = g_ff.ifmt_ctx->streams[g_ff.video_stream_idx];
        int w = st->codecpar->width;
        int h = st->codecpar->height;

        raw_frame->format = st->codecpar->format;
        raw_frame->width = w;
        raw_frame->height = h;
        av_image_fill_arrays(raw_frame->data, raw_frame->linesize,
                             ipkt->data, raw_frame->format, w, h, 1);

        /* 3. Convert YUYV → NV12 for encoder */
        sws_scale(g_ff.sws,
                  (const uint8_t *const *)raw_frame->data, raw_frame->linesize,
                  0, h,
                  g_ff.enc_frame->data, g_ff.enc_frame->linesize);

        /* PTS in encoder timebase (1/fps) */
        g_ff.enc_frame->pts = frame_count++;

        /* Force IDR if requested (PLI response) */
        if (g_ff.force_idr) {
            g_ff.enc_frame->pict_type = AV_PICTURE_TYPE_I;
            g_ff.force_idr = 0;
        } else {
            g_ff.enc_frame->pict_type = AV_PICTURE_TYPE_NONE;
        }

        /* 4. Encode */
        ret = avcodec_send_frame(g_ff.enc_ctx, g_ff.enc_frame);
        if (ret < 0) {
            fprintf(stderr, "[ffcap] avcodec_send_frame: %d\n", ret);
            av_packet_unref(ipkt);
            continue;
        }

        while ((ret = avcodec_receive_packet(g_ff.enc_ctx, opkt)) == 0) {
            bool is_key = (opkt->flags & AV_PKT_FLAG_KEY) != 0;

            /* PTS → milliseconds */
            int64_t pts_ms = 0;
            if (opkt->pts != AV_NOPTS_VALUE) {
                AVRational tb = g_ff.enc_ctx->time_base;
                pts_ms = av_rescale_q(opkt->pts, tb, (AVRational){1, 1000});
            }

            if (is_key && g_ff.sps_pps && g_ff.sps_pps_size > 0) {
                /* Prepend SPS/PPS before IDR (like header-mode=each-idr).
                 * Build a contiguous buffer: [SPS/PPS][IDR packet]. */
                size_t total = (size_t)g_ff.sps_pps_size + opkt->size;
                uint8_t *combined = malloc(total);
                if (combined) {
                    memcpy(combined, g_ff.sps_pps, g_ff.sps_pps_size);
                    memcpy(combined + g_ff.sps_pps_size, opkt->data, opkt->size);
                    if (g_ff.callback)
                        g_ff.callback(g_ff.userdata, combined, total,
                                      (uint32_t)pts_ms, true);
                    free(combined);
                }
            } else {
                /* P-frame or keyframe without separate SPS/PPS */
                bool is_keyframe = is_key || capture_annex_b_is_keyframe(opkt->data, opkt->size);
                if (g_ff.callback)
                    g_ff.callback(g_ff.userdata, opkt->data, opkt->size,
                                  (uint32_t)pts_ms, is_keyframe);
            }

            if (frame_count == 1 || (frame_count % 300) == 0)
                fprintf(stderr, "[ffcap] frame #%d (%d bytes%s)\n",
                        frame_count - 1, opkt->size, is_key ? " KEY" : "");

            av_packet_unref(opkt);
        }

        av_packet_unref(ipkt);
    }

out:
    av_packet_free(&ipkt);
    av_packet_free(&opkt);
    av_frame_free(&raw_frame);
    return NULL;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

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

    avdevice_register_all();

    /* --- Open V4L2 device --- */
    const AVInputFormat *v4l2fmt = av_find_input_format("v4l2");
    if (!v4l2fmt) {
        fprintf(stderr, "[ffcap] v4l2 input format not found\n");
        return -1;
    }

    AVDictionary *opts = NULL;
    char vsize[32], vfps[16];
    snprintf(vsize, sizeof(vsize), "%dx%d", cfg->width, cfg->height);
    snprintf(vfps, sizeof(vfps), "%d", cfg->fps);
    av_dict_set(&opts, "video_size", vsize, 0);
    av_dict_set(&opts, "framerate", vfps, 0);
    av_dict_set(&opts, "input_format", "yuyv422", 0);

    int ret = avformat_open_input(&g_ff.ifmt_ctx, cfg->device, v4l2fmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffcap] cannot open %s: %s\n", cfg->device, errbuf);
        return -1;
    }

    ret = avformat_find_stream_info(g_ff.ifmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "[ffcap] find_stream_info failed\n");
        capture_stop();
        return -1;
    }

    /* Find video stream */
    for (unsigned i = 0; i < g_ff.ifmt_ctx->nb_streams; i++) {
        if (g_ff.ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            g_ff.video_stream_idx = (int)i;
            break;
        }
    }
    if (g_ff.video_stream_idx < 0) {
        fprintf(stderr, "[ffcap] no video stream found\n");
        capture_stop();
        return -1;
    }

    AVCodecParameters *par = g_ff.ifmt_ctx->streams[g_ff.video_stream_idx]->codecpar;
    fprintf(stderr, "[ffcap] capture: %dx%d fmt=%d\n", par->width, par->height, par->format);

    /* --- Setup swscale: YUYV → NV12 --- */
    g_ff.sws = sws_getContext(par->width, par->height, par->format,
                              par->width, par->height, AV_PIX_FMT_NV12,
                              SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!g_ff.sws) {
        fprintf(stderr, "[ffcap] sws_getContext failed\n");
        capture_stop();
        return -1;
    }

    /* Allocate encoder input frame (NV12) */
    g_ff.enc_frame = av_frame_alloc();
    g_ff.enc_frame->format = AV_PIX_FMT_NV12;
    g_ff.enc_frame->width = par->width;
    g_ff.enc_frame->height = par->height;
    if (av_frame_get_buffer(g_ff.enc_frame, 32) < 0) {
        fprintf(stderr, "[ffcap] av_frame_get_buffer failed\n");
        capture_stop();
        return -1;
    }

    /* --- Setup H.264 encoder --- */
    const char *enc_name = (cfg->encoder && cfg->encoder[0]) ? cfg->encoder : "h264_rkmpp";
    const AVCodec *encoder = avcodec_find_encoder_by_name(enc_name);
    if (!encoder) {
        fprintf(stderr, "[ffcap] encoder '%s' not found, trying libx264\n", enc_name);
        encoder = avcodec_find_encoder_by_name("libx264");
    }
    if (!encoder) {
        fprintf(stderr, "[ffcap] no H.264 encoder available\n");
        capture_stop();
        return -1;
    }
    fprintf(stderr, "[ffcap] encoder: %s\n", encoder->name);

    g_ff.enc_ctx = avcodec_alloc_context3(encoder);
    g_ff.enc_ctx->width = par->width;
    g_ff.enc_ctx->height = par->height;
    g_ff.enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    g_ff.enc_ctx->time_base = (AVRational){1, cfg->fps};
    g_ff.enc_ctx->framerate = (AVRational){cfg->fps, 1};
    g_ff.enc_ctx->bit_rate = cfg->bitrate_bps;
    g_ff.enc_ctx->rc_max_rate = cfg->bitrate_bps * 3 / 2;
    g_ff.enc_ctx->rc_buffer_size = cfg->bitrate_bps;

    int gop = cfg->fps * (cfg->keyframe_interval_s > 0 ? cfg->keyframe_interval_s : 2);
    g_ff.enc_ctx->gop_size = gop;
    g_ff.enc_ctx->max_b_frames = 0;  /* No B-frames for low latency */
    g_ff.enc_ctx->flags &= ~AV_CODEC_FLAG_GLOBAL_HEADER; /* Annex-B inline headers */
    g_ff.enc_ctx->thread_count = 1;

    /* Encoder-specific options */
    if (strcmp(encoder->name, "h264_rkmpp") == 0) {
        av_opt_set(g_ff.enc_ctx->priv_data, "rc_mode", "CBR", 0);
        av_opt_set(g_ff.enc_ctx->priv_data, "profile", "baseline", 0);
        av_opt_set(g_ff.enc_ctx->priv_data, "level", "51", 0);
    } else if (strcmp(encoder->name, "libx264") == 0) {
        av_opt_set(g_ff.enc_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(g_ff.enc_ctx->priv_data, "tune", "zerolatency", 0);
        g_ff.enc_ctx->profile = FF_PROFILE_H264_BASELINE;
    }

    ret = avcodec_open2(g_ff.enc_ctx, encoder, NULL);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffcap] avcodec_open2 failed: %s\n", errbuf);
        capture_stop();
        return -1;
    }

    /* Cache SPS/PPS from extradata for prepending to IDR frames */
    if (g_ff.enc_ctx->extradata && g_ff.enc_ctx->extradata_size > 0) {
        g_ff.sps_pps_size = g_ff.enc_ctx->extradata_size;
        g_ff.sps_pps = malloc(g_ff.sps_pps_size);
        if (g_ff.sps_pps)
            memcpy(g_ff.sps_pps, g_ff.enc_ctx->extradata, g_ff.sps_pps_size);
    }

    fprintf(stderr, "[ffcap] pipeline: %s %dx%d@%dfps %d bps GOP %d\n",
            encoder->name, par->width, par->height, cfg->fps, cfg->bitrate_bps, gop);

    /* --- Start capture thread --- */
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
    if (g_ff.enc_ctx) {
        avcodec_free_context(&g_ff.enc_ctx);
    }
    if (g_ff.enc_frame) {
        av_frame_free(&g_ff.enc_frame);
    }
    if (g_ff.sws) {
        sws_freeContext(g_ff.sws);
        g_ff.sws = NULL;
    }
    if (g_ff.ifmt_ctx) {
        avformat_close_input(&g_ff.ifmt_ctx);
    }
    free(g_ff.sps_pps);
    g_ff.sps_pps = NULL;
    g_ff.sps_pps_size = 0;
    g_ff.callback = NULL;
    g_ff.userdata = NULL;
}

void capture_force_keyframe(void)
{
    g_ff.force_idr = 1;
}
