/*
 * Fuzz harness for RTP/RTCP parsers — nano_rtp.c, nano_rtcp.c
 *
 * Targets: rtp_unpack(), rtcp_parse()
 * Attack surface: Binary protocol parsers for real-time media packets.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_rtp.h"
#include "nano_rtcp.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Fuzz RTP unpacker */
    uint8_t pt = 0;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    rtp_unpack(data, size, &pt, &seq, &ts, &ssrc, &payload, &payload_len);

    /* Fuzz RTCP parser with the same data */
    nano_rtcp_info_t info;
    memset(&info, 0, sizeof(info));
    rtcp_parse(data, size, &info);

    return 0;
}
