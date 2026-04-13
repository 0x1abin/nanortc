/*
 * nanortc — Shared Annex-B bitstream scanner
 *
 * Migrated from h264_annex_b_find_nal() because the start-code scan logic
 * is codec-agnostic: H.264 and H.265 both use the ISO/IEC Annex-B framing
 * with 00 00 01 / 00 00 00 01 start codes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_annex_b.h"

const uint8_t *nano_annex_b_find_nal(const uint8_t *data, size_t len, size_t *offset,
                                     size_t *nal_len)
{
    if (!data || !offset || !nal_len) {
        return NULL;
    }

    size_t i = *offset;

    /* Scan for start code: 00 00 01 or 00 00 00 01 */
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
