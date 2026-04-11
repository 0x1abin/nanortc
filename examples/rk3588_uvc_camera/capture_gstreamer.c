/*
 * capture_gstreamer — GStreamer backend for capture.h
 *
 * Compile with RK3588_CAPTURE_GSTREAMER defined.
 * Default encoder: mpph264enc (RK3588 hardware).
 * Fallback: openh264enc (software).
 *
 * SPDX-License-Identifier: MIT
 */

#include "capture.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Module state (single-instance)
 * ---------------------------------------------------------------- */

static struct {
    GstElement *pipeline;
    GstAppSink *sink;
    GstElement *encoder;
    capture_encoder_cb callback;
    void *userdata;
} g_state;

/* ----------------------------------------------------------------
 * appsink callback (called from GStreamer streaming thread)
 * ---------------------------------------------------------------- */
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data)
{
    (void)user_data;
    static int sample_count = 0;

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    if (sample_count == 0 || (sample_count % 300) == 0) {
        fprintf(stderr, "[gst] encoded frame #%d (%zu bytes)\n", sample_count, map.size);
    }
    sample_count++;

    GstClockTime pts_ns = GST_BUFFER_PTS(buffer);
    uint32_t pts_ms = (pts_ns != GST_CLOCK_TIME_NONE)
                          ? (uint32_t)(pts_ns / GST_MSECOND)
                          : 0;

    bool is_keyframe = capture_annex_b_is_keyframe(map.data, map.size);

    if (g_state.callback) {
        g_state.callback(g_state.userdata, map.data, map.size, pts_ms, is_keyframe);
    }

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int capture_start(const capture_config_t *cfg)
{
    if (!cfg || !cfg->device || !cfg->callback) {
        fprintf(stderr, "[gst] invalid config\n");
        return -1;
    }

    if (g_state.pipeline) {
        fprintf(stderr, "[gst] already started\n");
        return -1;
    }

    static gboolean gst_initialized = FALSE;
    if (!gst_initialized) {
        gst_init(NULL, NULL);
        gst_initialized = TRUE;
    }

    int gop = cfg->fps * (cfg->keyframe_interval_s > 0 ? cfg->keyframe_interval_s : 2);

    /* Encoder selection: mpph264enc (RK3588 hardware, default) or
     * openh264enc (software fallback). The pipeline differs because
     * the property names diverge between the two elements. */
    const char *enc = (cfg->encoder && cfg->encoder[0]) ? cfg->encoder : "mpph264enc";
    int use_mpp = (strcmp(enc, "mpph264enc") == 0);

    /* Both encoders receive NV12 via videoconvert for correct chroma
     * downsampling (YUY2 4:2:2 → NV12 4:2:0).
     * mpph264enc: header-mode=each-idr ensures SPS/PPS in every IDR
     * so browsers can decode from any keyframe (mid-stream join, PLI).
     * openh264enc: needs I420 instead of NV12. */
    char pipeline_str[1024];
    int n;
    if (use_mpp) {
        n = snprintf(pipeline_str, sizeof(pipeline_str),
            "v4l2src device=%s do-timestamp=true "
            "! video/x-raw,format=YUY2,width=%d,height=%d,framerate=%d/1 "
            "! videoconvert "
            "! video/x-raw,format=NV12 "
            "! mpph264enc name=enc header-mode=each-idr "
              "rc-mode=cbr bps=%d bps-max=%d gop=%d "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! appsink name=sink sync=false max-buffers=2 drop=true",
            cfg->device, cfg->width, cfg->height, cfg->fps,
            cfg->bitrate_bps, cfg->bitrate_bps * 3 / 2, gop);
    } else {
        n = snprintf(pipeline_str, sizeof(pipeline_str),
            "v4l2src device=%s do-timestamp=true "
            "! video/x-raw,format=YUY2,width=%d,height=%d,framerate=%d/1 "
            "! videoconvert "
            "! video/x-raw,format=I420 "
            "! openh264enc name=enc bitrate=%d gop-size=%d complexity=low "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! appsink name=sink sync=false max-buffers=4 drop=true",
            cfg->device, cfg->width, cfg->height, cfg->fps,
            cfg->bitrate_bps, gop);
    }

    if (n < 0 || (size_t)n >= sizeof(pipeline_str)) {
        fprintf(stderr, "[gst] pipeline string overflow\n");
        return -1;
    }

    fprintf(stderr, "[gst] launching pipeline:\n  %s\n", pipeline_str);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_str, &err);
    if (!pipeline || err) {
        fprintf(stderr, "[gst] gst_parse_launch failed: %s\n",
                err ? err->message : "(unknown)");
        if (err) g_error_free(err);
        if (pipeline) gst_object_unref(pipeline);
        return -1;
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!sink) {
        fprintf(stderr, "[gst] cannot locate appsink\n");
        gst_object_unref(pipeline);
        return -1;
    }

    GstElement *encoder = gst_bin_get_by_name(GST_BIN(pipeline), "enc");

    g_state.pipeline = pipeline;
    g_state.sink = GST_APP_SINK(sink);
    g_state.encoder = encoder;
    g_state.callback = cfg->callback;
    g_state.userdata = cfg->userdata;

    /* Use structured callback instead of g_signal_connect.
     * Signals require a GLib main loop; the callback struct is invoked
     * directly from the streaming thread. */
    GstAppSinkCallbacks callbacks = {0};
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, NULL, NULL);
    g_object_set(sink, "emit-signals", FALSE, NULL);

    GstStateChangeReturn rc = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (rc == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[gst] failed to set pipeline PLAYING\n");
        capture_stop();
        return -1;
    }

    fprintf(stderr, "[gst] pipeline running (%dx%d@%dfps, %d bps, GOP %d)\n",
            cfg->width, cfg->height, cfg->fps, cfg->bitrate_bps, gop);
    return 0;
}

void capture_stop(void)
{
    if (g_state.pipeline) {
        gst_element_set_state(g_state.pipeline, GST_STATE_NULL);
    }
    if (g_state.encoder) {
        gst_object_unref(g_state.encoder);
        g_state.encoder = NULL;
    }
    if (g_state.sink) {
        gst_object_unref(g_state.sink);
        g_state.sink = NULL;
    }
    if (g_state.pipeline) {
        gst_object_unref(g_state.pipeline);
        g_state.pipeline = NULL;
    }
    g_state.callback = NULL;
    g_state.userdata = NULL;
}

void capture_force_keyframe(void)
{
    if (!g_state.encoder) {
        return;
    }

    /* Send upstream force-key-unit event via the encoder's src pad.
     * GStreamer routes it upstream to trigger an IDR on the next frame. */
    GstPad *src_pad = gst_element_get_static_pad(g_state.encoder, "src");
    if (!src_pad) {
        return;
    }

    GstEvent *evt = gst_video_event_new_upstream_force_key_unit(
        GST_CLOCK_TIME_NONE, TRUE, 0);

    if (evt) {
        gst_pad_send_event(src_pad, evt);
    }
    gst_object_unref(src_pad);
}
