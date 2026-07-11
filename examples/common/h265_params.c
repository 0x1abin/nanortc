/*
 * h265_params.c — shared H.265/HEVC parameter-set extraction (see h265_params.h)
 *
 * SPDX-License-Identifier: MIT
 */
#include "h265_params.h"

#if NANORTC_FEATURE_H265

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t nano_get_millis(void); /* from examples/common/run_loop_linux.c */

/* Walk an Annex-B byte stream: return a pointer to the next NAL payload (start
 * code skipped) and its length, advancing *offset past it. NULL at end. */
static const uint8_t *h265_annex_b_next_nal(const uint8_t *buf, size_t len, size_t *offset,
                                            size_t *nal_len)
{
    size_t i = *offset;
    while (i + 3 <= len) {
        if (buf[i] == 0 && buf[i + 1] == 0) {
            size_t sc_len = 0;
            if (buf[i + 2] == 1) {
                sc_len = 3;
            } else if (i + 4 <= len && buf[i + 2] == 0 && buf[i + 3] == 1) {
                sc_len = 4;
            } else {
                i++;
                continue;
            }
            size_t nal_start = i + sc_len;
            size_t j = nal_start;
            while (j + 3 <= len) {
                if (buf[j] == 0 && buf[j + 1] == 0 &&
                    (buf[j + 2] == 1 || (j + 4 <= len && buf[j + 2] == 0 && buf[j + 3] == 1)))
                    break;
                j++;
            }
            if (j + 3 > len)
                j = len;
            *nal_len = j - nal_start;
            *offset = j;
            return buf + nal_start;
        }
        i++;
    }
    *offset = len;
    return NULL;
}

int h265_params_extract(h265_params_t *ps, const uint8_t *annex_b, size_t len)
{
    ps->ready = false;
    ps->vps_len = ps->sps_len = ps->pps_len = 0;
    size_t write_off = 0;
    size_t offset = 0;
    while (offset < len) {
        size_t nal_len = 0;
        const uint8_t *nal = h265_annex_b_next_nal(annex_b, len, &offset, &nal_len);
        if (!nal || nal_len < 2)
            continue;
        uint8_t nut = (uint8_t)((nal[0] >> 1) & 0x3F);
        size_t *dst_off = NULL;
        size_t *dst_len = NULL;
        if (nut == 32 && ps->vps_len == 0) {
            dst_off = &ps->vps_off;
            dst_len = &ps->vps_len;
        } else if (nut == 33 && ps->sps_len == 0) {
            dst_off = &ps->sps_off;
            dst_len = &ps->sps_len;
        } else if (nut == 34 && ps->pps_len == 0) {
            dst_off = &ps->pps_off;
            dst_len = &ps->pps_len;
        }
        if (!dst_off)
            continue;
        if (write_off + nal_len > sizeof(ps->bytes))
            return -1;
        memcpy(&ps->bytes[write_off], nal, nal_len);
        *dst_off = write_off;
        *dst_len = nal_len;
        write_off += nal_len;
        if (ps->vps_len && ps->sps_len && ps->pps_len) {
            ps->ready = true;
            return 0;
        }
    }
    return -1;
}

int h265_params_extract_from_queue(h265_params_t *ps, media_queue_t *q, int max_retry,
                                   int budget_ms, const volatile sig_atomic_t *quit,
                                   void (*force_keyframe)(void), const char *log_tag)
{
    for (int attempt = 1; attempt <= max_retry; attempt++) {
        if (quit && *quit)
            break;
        memset(ps, 0, sizeof(*ps));
        /* Force an IDR so the next encoded frame carries VPS/SPS/PPS — with
         * an effectively-infinite GOP only the on-demand IDR has them. */
        if (force_keyframe)
            force_keyframe();
        uint32_t t0 = nano_get_millis();
        while ((!quit || !*quit) && (nano_get_millis() - t0) < (uint32_t)budget_ms) {
            media_frame_t f = {0};
            if (media_queue_pop(q, &f) == 0) {
                int r = h265_params_extract(ps, f.data, f.len);
                free(f.data);
                if (r == 0) {
                    fprintf(stderr, "%s level=INFO msg=\"H.265 params\" vps=%zu sps=%zu pps=%zu\n",
                            log_tag, ps->vps_len, ps->sps_len, ps->pps_len);
                    return 0;
                }
            } else {
                /* Queue empty: nap briefly. We deliberately do NOT drain the
                 * media-queue wake pipe here — it is a blocking pipe, so a
                 * read on an empty pipe could stall past the budget; the few
                 * wake bytes that accumulate are drained by the main loop. */
                struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000L};
                nanosleep(&ts, NULL);
            }
        }
        fprintf(
            stderr,
            "%s level=WARN msg=\"H.265 PS extract attempt failed\" attempt=%d/%d budget_ms=%d\n",
            log_tag, attempt, max_retry, budget_ms);
    }
    fprintf(stderr, "%s level=ERROR msg=\"H.265 PS extraction failed after retries\"\n", log_tag);
    return -1;
}

#endif /* NANORTC_FEATURE_H265 */
