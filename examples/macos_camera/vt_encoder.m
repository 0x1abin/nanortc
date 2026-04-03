/*
 * vt_encoder — macOS VideoToolbox H.264 hardware encoder
 *
 * Encodes CVPixelBuffer frames to H.264 Annex-B format suitable
 * for nanortc_send_video(). Handles AVCC → Annex-B conversion
 * and SPS/PPS extraction for keyframes.
 *
 * SPDX-License-Identifier: MIT
 */

#import <VideoToolbox/VideoToolbox.h>

#include "vt_encoder.h"

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Annex-B start code
 * ---------------------------------------------------------------- */

static const uint8_t ANNEX_B_START_CODE[] = {0x00, 0x00, 0x00, 0x01};
#define START_CODE_LEN 4

/* ----------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------- */

static VTCompressionSessionRef s_session;
static vt_encoder_cb s_callback;
static void *s_userdata;
static CMTime s_start_time;
static bool s_start_time_valid;
static volatile bool s_force_keyframe;

/* Scratch buffer for Annex-B assembly (max ~128KB for a keyframe) */
#define ANNEX_B_BUF_SIZE (128 * 1024)
static uint8_t s_annex_b_buf[ANNEX_B_BUF_SIZE];

/* ----------------------------------------------------------------
 * AVCC → Annex-B conversion
 * ---------------------------------------------------------------- */

/**
 * Append SPS and PPS from the format description to the output buffer.
 * Returns the number of bytes written, or -1 on error.
 */
static int append_parameter_sets(CMFormatDescriptionRef fmt, uint8_t *out, size_t out_size)
{
    size_t offset = 0;
    size_t param_count = 0;

    OSStatus st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, NULL, NULL,
                                                                     &param_count, NULL);
    if (st != noErr)
        return -1;

    for (size_t i = 0; i < param_count; i++) {
        const uint8_t *param = NULL;
        size_t param_len = 0;

        st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &param, &param_len,
                                                                 NULL, NULL);
        if (st != noErr)
            continue;

        if (offset + START_CODE_LEN + param_len > out_size)
            return -1;

        memcpy(out + offset, ANNEX_B_START_CODE, START_CODE_LEN);
        offset += START_CODE_LEN;
        memcpy(out + offset, param, param_len);
        offset += param_len;
    }

    return (int)offset;
}

/**
 * Convert AVCC-formatted NALs in a CMBlockBuffer to Annex-B format.
 * Returns the number of bytes written, or -1 on error.
 */
static int convert_avcc_to_annex_b(CMBlockBufferRef block, int nal_length_size, uint8_t *out,
                                   size_t out_offset, size_t out_size)
{
    size_t total_len = CMBlockBufferGetDataLength(block);
    size_t src_offset = 0;
    size_t dst_offset = out_offset;

    while (src_offset < total_len) {
        /* Read NAL length (big-endian, typically 4 bytes) */
        uint32_t nal_len = 0;
        uint8_t len_buf[4] = {0};

        OSStatus st = CMBlockBufferCopyDataBytes(block, src_offset, (size_t)nal_length_size, len_buf);
        if (st != noErr)
            return -1;

        for (int i = 0; i < nal_length_size; i++) {
            nal_len = (nal_len << 8) | len_buf[i];
        }
        src_offset += (size_t)nal_length_size;

        if (src_offset + nal_len > total_len)
            return -1;
        if (dst_offset + START_CODE_LEN + nal_len > out_size)
            return -1;

        /* Write Annex-B start code + NAL data */
        memcpy(out + dst_offset, ANNEX_B_START_CODE, START_CODE_LEN);
        dst_offset += START_CODE_LEN;

        st = CMBlockBufferCopyDataBytes(block, src_offset, nal_len, out + dst_offset);
        if (st != noErr)
            return -1;

        dst_offset += nal_len;
        src_offset += nal_len;
    }

    return (int)(dst_offset - out_offset);
}

/* ----------------------------------------------------------------
 * VideoToolbox compression callback
 * ---------------------------------------------------------------- */

static void compression_callback(void *outputCallbackRefCon, void *sourceFrameRefCon,
                                 OSStatus status, VTEncodeInfoFlags infoFlags,
                                 CMSampleBufferRef sampleBuffer)
{
    (void)outputCallbackRefCon;
    (void)sourceFrameRefCon;

    if (status != noErr || !sampleBuffer)
        return;

    /* Check if keyframe */
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    bool is_keyframe = false;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef notSync = CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync);
        is_keyframe = (notSync == NULL || !CFBooleanGetValue(notSync));
    }

    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!block)
        return;

    /* Get NAL length size from format description */
    int nal_length_size = 0;
    if (fmt) {
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, NULL, NULL, NULL,
                                                           &nal_length_size);
    }
    if (nal_length_size == 0)
        nal_length_size = 4;

    size_t offset = 0;

    /* For keyframes, prepend SPS/PPS */
    if (is_keyframe && fmt) {
        int ps_len = append_parameter_sets(fmt, s_annex_b_buf, ANNEX_B_BUF_SIZE);
        if (ps_len > 0)
            offset = (size_t)ps_len;
    }

    /* Convert AVCC NALs to Annex-B */
    int nal_len = convert_avcc_to_annex_b(block, nal_length_size, s_annex_b_buf, offset,
                                          ANNEX_B_BUF_SIZE);
    if (nal_len < 0)
        return;

    size_t total_len = offset + (size_t)nal_len;

    /* Compute PTS in milliseconds */
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    uint32_t pts_ms;
    if (!s_start_time_valid) {
        s_start_time = pts;
        s_start_time_valid = true;
        pts_ms = 0;
    } else {
        CMTime elapsed = CMTimeSubtract(pts, s_start_time);
        pts_ms = (uint32_t)(CMTimeGetSeconds(elapsed) * 1000.0);
    }

    if (s_callback) {
        s_callback(s_userdata, s_annex_b_buf, total_len, pts_ms, is_keyframe);
    }
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int vt_encoder_init(const vt_encoder_config_t *cfg)
{
    if (!cfg || !cfg->callback) {
        fprintf(stderr, "[vt_encoder] Invalid config\n");
        return -1;
    }

    int width = cfg->width > 0 ? cfg->width : 1280;
    int height = cfg->height > 0 ? cfg->height : 720;
    int fps = cfg->fps > 0 ? cfg->fps : 30;
    int bitrate = cfg->bitrate_kbps > 0 ? cfg->bitrate_kbps : 800;
    int kf_interval = cfg->keyframe_interval_s > 0 ? cfg->keyframe_interval_s : 2;

    s_callback = cfg->callback;
    s_userdata = cfg->userdata;
    s_start_time_valid = false;
    s_force_keyframe = false;

    /* Create compression session */
    NSDictionary *encSpec = @{
        (NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @YES
    };

    OSStatus st = VTCompressionSessionCreate(
        NULL,                    /* allocator */
        width, height,
        kCMVideoCodecType_H264,
        (__bridge CFDictionaryRef)encSpec,
        NULL,                    /* source pixel buffer attributes */
        NULL,                    /* compressed data allocator */
        compression_callback,
        NULL,                    /* callback refcon */
        &s_session);

    if (st != noErr) {
        fprintf(stderr, "[vt_encoder] VTCompressionSessionCreate failed: %d\n", (int)st);
        return -1;
    }

    /* Real-time encoding */
    VTSessionSetProperty(s_session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);

    /* Baseline profile (maximum browser compatibility) */
    VTSessionSetProperty(s_session, kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_H264_Baseline_AutoLevel);

    /* No B-frames (critical for low latency) */
    VTSessionSetProperty(s_session, kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);

    /* Bitrate */
    int bitrate_bps = bitrate * 1000;
    CFNumberRef bitrateRef = CFNumberCreate(NULL, kCFNumberIntType, &bitrate_bps);
    VTSessionSetProperty(s_session, kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    /* Max keyframe interval */
    int max_kf = fps * kf_interval;
    CFNumberRef kfRef = CFNumberCreate(NULL, kCFNumberIntType, &max_kf);
    VTSessionSetProperty(s_session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kfRef);
    CFRelease(kfRef);

    /* Expected frame rate */
    CFNumberRef fpsRef = CFNumberCreate(NULL, kCFNumberIntType, &fps);
    VTSessionSetProperty(s_session, kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
    CFRelease(fpsRef);

    VTCompressionSessionPrepareToEncodeFrames(s_session);

    fprintf(stderr, "[vt_encoder] H.264 encoder ready (%dx%d@%dfps, %dkbps, KF every %ds)\n",
            width, height, fps, bitrate, kf_interval);

    return 0;
}

void vt_encoder_encode(CVPixelBufferRef pixbuf, CMTime pts)
{
    if (!s_session || !pixbuf)
        return;

    NSDictionary *props = nil;
    if (s_force_keyframe) {
        s_force_keyframe = false;
        props = @{(NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES};
    }

    VTCompressionSessionEncodeFrame(s_session, pixbuf, pts, kCMTimeInvalid,
                                    (__bridge CFDictionaryRef)props, NULL, NULL);
}

void vt_encoder_force_keyframe(void)
{
    s_force_keyframe = true;
}

void vt_encoder_destroy(void)
{
    if (s_session) {
        VTCompressionSessionCompleteFrames(s_session, kCMTimeInvalid);
        VTCompressionSessionInvalidate(s_session);
        CFRelease(s_session);
        s_session = NULL;
        fprintf(stderr, "[vt_encoder] Encoder destroyed\n");
    }
    s_callback = NULL;
}
