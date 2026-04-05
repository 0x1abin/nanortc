/*
 * nanortc — CRC-32 (ISO HDLC) for STUN FINGERPRINT (RFC 8489 §14.7)
 * @internal Not part of the public API.
 *
 * NOT the same as CRC-32c (Castagnoli) used by SCTP.
 * Polynomial: 0xEDB88320 (reflected) / 0x04C11DB7 (normal)
 * Test vector: CRC-32("123456789") = 0xCBF43926
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_CRC32_H_
#define NANORTC_CRC32_H_

#include <stdint.h>
#include <stddef.h>

uint32_t nano_crc32(const uint8_t *data, size_t len);

#endif /* NANORTC_CRC32_H_ */
