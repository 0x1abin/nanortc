/*
 * nanortc — Base64 encoder (RFC 4648 §4 standard alphabet)
 * @internal Not part of the public API.
 *
 * Encoder-only: needed by the H.265 SDP path for sprop-vps / sprop-sps /
 * sprop-pps fmtp parameters (RFC 7798 §7.1). Decoder is not required by
 * any current code path.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_BASE64_H_
#define NANORTC_BASE64_H_

#include <stdint.h>
#include <stddef.h>

/**
 * Compute the number of bytes required (including a trailing NUL terminator)
 * to hold the base64-encoded form of @p src_len raw bytes. Useful for sizing
 * output buffers at compile time.
 *
 * @param src_len  Raw byte count.
 * @return Encoded length in bytes (ceil(src_len / 3) * 4 + 1).
 */
static inline size_t nano_base64_encoded_size(size_t src_len)
{
    return ((src_len + 2) / 3) * 4 + 1;
}

/**
 * Encode a byte buffer to base64 using the RFC 4648 §4 standard alphabet
 * (A–Z, a–z, 0–9, +, /). Output is zero-terminated and padded with '='
 * characters per §3.2.
 *
 * The output buffer must have at least nano_base64_encoded_size(src_len)
 * bytes of capacity. On success, @p out_len receives the number of encoded
 * bytes (excluding the trailing NUL).
 *
 * @param src      Input bytes (may be NULL iff src_len == 0).
 * @param src_len  Length of the input in bytes.
 * @param dst      Output buffer (must be non-NULL unless src_len == 0 and
 *                 dst_cap == 0 to obtain length-only information).
 * @param dst_cap  Capacity of @p dst in bytes.
 * @param out_len  [out] Encoded length, or 0 on error.
 * @return NANORTC_OK on success, negative error code otherwise.
 */
int nano_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_cap,
                       size_t *out_len);

#endif /* NANORTC_BASE64_H_ */
