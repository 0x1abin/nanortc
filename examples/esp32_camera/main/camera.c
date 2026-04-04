/*
 * nanortc ESP32-P4 Camera example — V4L2 capture via esp_video
 *
 * Uses the esp_video V4L2 layer to capture YUV420 frames from
 * a MIPI CSI camera sensor. The esp_video component handles
 * CSI controller, ISP, and sensor driver initialization internally.
 *
 * SPDX-License-Identifier: MIT
 */

#include "camera.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_log.h"

#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"
#include "esp_cam_sensor_xclk.h"

static const char *TAG = "camera";

#define BUFFER_COUNT 2

typedef struct {
    int fd;
    uint8_t *buffers[BUFFER_COUNT];
    uint32_t buffer_sizes[BUFFER_COUNT];
    int last_dequeued_index;
    uint16_t width;
    uint16_t height;
    esp_cam_sensor_xclk_handle_t xclk_handle;
} cam_state_t;

static cam_state_t s_cam;

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int camera_init(uint16_t width, uint16_t height, uint8_t fps)
{
    memset(&s_cam, 0, sizeof(s_cam));
    s_cam.fd = -1;
    s_cam.last_dequeued_index = -1;
    s_cam.width = width;
    s_cam.height = height;

    /* Phase 1: XCLK setup (esp_video does not handle XCLK for CSI devices) */
    esp_err_t ret = esp_cam_sensor_xclk_allocate(
        ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &s_cam.xclk_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XCLK allocate failed: %s", esp_err_to_name(ret));
        return -1;
    }
    esp_cam_sensor_xclk_config_t xclk_cfg = {
        .esp_clock_router_cfg = {
            .xclk_pin = CONFIG_EXAMPLE_CAM_XCLK_PIN,
            .xclk_freq_hz = 24 * 1000 * 1000,
        }
    };
    ret = esp_cam_sensor_xclk_start(s_cam.xclk_handle, &xclk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XCLK start failed: %s", esp_err_to_name(ret));
        return -1;
    }

    /* Phase 2: Initialize esp_video subsystem (handles CSI+ISP+sensor internally) */
    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = I2C_NUM_0,
                .scl_pin = CONFIG_EXAMPLE_CAM_SCCB_SCL_PIN,
                .sda_pin = CONFIG_EXAMPLE_CAM_SCCB_SDA_PIN,
            },
            .freq = 100000,
        },
        .reset_pin = CONFIG_EXAMPLE_CAM_RESET_PIN,
        .pwdn_pin = CONFIG_EXAMPLE_CAM_PWDN_PIN,
    };
    esp_video_init_config_t cam_config = {
        .csi = &csi_cfg,
    };
    ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return -1;
    }

    /* Phase 3: Open V4L2 device and configure capture */
    s_cam.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_cam.fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return -1;
    }

    /* Set capture format: YUV420 at requested resolution */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    if (ioctl(s_cam.fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed");
        close(s_cam.fd);
        s_cam.fd = -1;
        return -1;
    }
    ESP_LOGI(TAG, "Format set: %"PRIu32"x%"PRIu32" pixfmt=0x%"PRIx32,
             fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat);

    /* Set frame rate (non-fatal if unsupported) */
    struct v4l2_streamparm sparm;
    memset(&sparm, 0, sizeof(sparm));
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sparm.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    sparm.parm.capture.timeperframe.numerator = 1;
    sparm.parm.capture.timeperframe.denominator = fps;
    if (ioctl(s_cam.fd, VIDIOC_S_PARM, &sparm) != 0) {
        ESP_LOGW(TAG, "VIDIOC_S_PARM failed (fps=%d), using sensor default", fps);
    }

    /* Set DQBUF timeout to 2 seconds */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    ioctl(s_cam.fd, VIDIOC_S_DQBUF_TIMEOUT, &tv);

    /* Request buffers (MMAP mode) */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        close(s_cam.fd);
        s_cam.fd = -1;
        return -1;
    }

    /* Query, map, and enqueue each buffer */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed", i);
            close(s_cam.fd);
            s_cam.fd = -1;
            return -1;
        }

        s_cam.buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, s_cam.fd, buf.m.offset);
        if (s_cam.buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%d] failed", i);
            s_cam.buffers[i] = NULL;
            close(s_cam.fd);
            s_cam.fd = -1;
            return -1;
        }
        s_cam.buffer_sizes[i] = buf.length;

        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed", i);
            close(s_cam.fd);
            s_cam.fd = -1;
            return -1;
        }
    }

    ESP_LOGI(TAG, "Camera initialized: %dx%d V4L2 MMAP (%d bufs)", width, height, BUFFER_COUNT);
    return 0;
}

int camera_start_streaming(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        return -1;
    }
    ESP_LOGI(TAG, "V4L2 streaming started");
    return 0;
}

int camera_grab_frame(uint8_t **buf, size_t *len)
{
    if (s_cam.fd < 0)
        return -1;

    struct v4l2_buffer v4l2_buf;
    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_cam.fd, VIDIOC_DQBUF, &v4l2_buf) != 0)
        return -1;

    s_cam.last_dequeued_index = v4l2_buf.index;
    *buf = s_cam.buffers[v4l2_buf.index];
    *len = v4l2_buf.bytesused;
    return 0;
}

void camera_release_frame(void)
{
    if (s_cam.fd < 0 || s_cam.last_dequeued_index < 0)
        return;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = s_cam.last_dequeued_index;
    ioctl(s_cam.fd, VIDIOC_QBUF, &buf);

    s_cam.last_dequeued_index = -1;
}

void camera_deinit(void)
{
    if (s_cam.fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);

        for (int i = 0; i < BUFFER_COUNT; i++) {
            if (s_cam.buffers[i]) {
                munmap(s_cam.buffers[i], s_cam.buffer_sizes[i]);
                s_cam.buffers[i] = NULL;
            }
        }
        close(s_cam.fd);
        s_cam.fd = -1;
    }

    if (s_cam.xclk_handle) {
        esp_cam_sensor_xclk_stop(s_cam.xclk_handle);
        esp_cam_sensor_xclk_free(s_cam.xclk_handle);
        s_cam.xclk_handle = NULL;
    }

    esp_video_deinit();
    ESP_LOGI(TAG, "Camera deinitialized");
}
