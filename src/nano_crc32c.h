/*
 * nanortc — CRC-32c for SCTP checksums
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_CRC32C_H_
#define NANO_CRC32C_H_

#include <stdint.h>
#include <stddef.h>

uint32_t nano_crc32c(const uint8_t *data, size_t len);

#endif /* NANO_CRC32C_H_ */
