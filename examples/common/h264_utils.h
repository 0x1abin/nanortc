/*
 * h264_utils.h — H.264 Annex-B bitstream utilities for examples
 *
 * Shared between browser_interop and linux_media_send examples.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef H264_UTILS_H_
#define H264_UTILS_H_

#include <stddef.h>
#include <stdint.h>

/**
 * Find next NAL unit in Annex-B bitstream.
 * Scans for 00 00 01 or 00 00 00 01 start codes.
 * *offset is updated past each NAL. Returns NULL when no more NALs.
 */
static inline const uint8_t *annex_b_find_nal(const uint8_t *data, size_t len, size_t *offset,
                                              size_t *nal_len)
{
    size_t i = *offset;

    /* Skip to start code */
    while (i + 2 < len) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                i += 3;
                break;
            }
            if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
                i += 4;
                break;
            }
        }
        i++;
    }

    if (i >= len) {
        return NULL;
    }

    const uint8_t *nal_start = data + i;

    /* Find next start code or end of buffer */
    size_t j = i;
    while (j + 2 < len) {
        if (data[j] == 0 && data[j + 1] == 0 &&
            (data[j + 2] == 1 || (j + 3 < len && data[j + 2] == 0 && data[j + 3] == 1))) {
            break;
        }
        j++;
    }
    if (j + 2 >= len) {
        j = len;
    }

    /* Strip trailing zero padding between NALs */
    size_t end = j;
    while (end > i && data[end - 1] == 0) {
        end--;
    }

    *nal_len = end - i;
    *offset = j;
    return nal_start;
}

#endif /* H264_UTILS_H_ */
