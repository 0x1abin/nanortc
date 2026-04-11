/*
 * nanortc — CRC-32c for SCTP checksums
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_CRC32C_H_
#define NANORTC_CRC32C_H_

#include <stdint.h>
#include <stddef.h>

/** Compute CRC-32c over a contiguous buffer (convenience wrapper). */
uint32_t nano_crc32c(const uint8_t *data, size_t len);

/** Incremental CRC-32c API — compute CRC over non-contiguous segments. */
uint32_t nano_crc32c_init(void);
uint32_t nano_crc32c_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t nano_crc32c_final(uint32_t crc);

#endif /* NANORTC_CRC32C_H_ */
