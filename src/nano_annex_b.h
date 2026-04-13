/*
 * nanortc — Shared Annex-B bitstream scanner
 * @internal Not part of the public API.
 *
 * The Annex-B framing (start codes 00 00 01 / 00 00 00 01) is codec-agnostic
 * and is used by both H.264 (ISO/IEC 14496-10 Annex B) and H.265
 * (ISO/IEC 23008-2 Annex B) bitstreams. This helper centralizes the NAL
 * extraction logic so H.264 and H.265 paths can share it.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_ANNEX_B_H_
#define NANORTC_ANNEX_B_H_

#include <stdint.h>
#include <stddef.h>

/**
 * Find the next NAL unit in an Annex-B bitstream.
 *
 * Scans for the 3-byte (00 00 01) or 4-byte (00 00 00 01) start code and
 * returns a pointer to the NAL payload that follows, along with its length
 * up to (but excluding) the next start code or end of buffer. Trailing zero
 * bytes between NAL units are stripped from nal_len so callers receive the
 * canonical NAL unit byte sequence.
 *
 * @param data    Annex-B buffer.
 * @param len     Length of buffer in bytes.
 * @param offset  [in/out] Search cursor; advanced past the returned NAL so
 *                repeated calls can walk the entire stream.
 * @param nal_len [out] Length of the returned NAL unit (excluding start code).
 * @return Pointer to the NAL payload, or NULL if no more NAL units were found.
 */
const uint8_t *nano_annex_b_find_nal(const uint8_t *data, size_t len, size_t *offset,
                                     size_t *nal_len);

#endif /* NANORTC_ANNEX_B_H_ */
