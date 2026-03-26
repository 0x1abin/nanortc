/*
 * nanortc — RTP packing/unpacking internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_RTP_H_
#define NANO_RTP_H_

#include <stdint.h>
#include <stddef.h>

#define RTP_HEADER_SIZE 12
#define RTP_VERSION     2

typedef struct nano_rtp {
    uint16_t seq;
    uint32_t ssrc;
    uint8_t payload_type;
} nano_rtp_t;

int rtp_init(nano_rtp_t *rtp, uint32_t ssrc, uint8_t pt);
int rtp_pack(nano_rtp_t *rtp, uint32_t timestamp,
             const uint8_t *payload, size_t payload_len,
             uint8_t *buf, size_t buf_len, size_t *out_len);
int rtp_unpack(const uint8_t *data, size_t len,
               uint8_t *pt, uint16_t *seq, uint32_t *ts, uint32_t *ssrc,
               const uint8_t **payload, size_t *payload_len);

#endif /* NANO_RTP_H_ */
