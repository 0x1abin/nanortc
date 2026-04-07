/*
 * Fuzz harness for SCTP parsers — nano_sctp.c
 *
 * Targets: nsctp_parse_header(), nsctp_parse_init(), nsctp_parse_data(),
 *          nsctp_parse_sack(), nsctp_verify_checksum()
 * Attack surface: Largest parser module (1184 lines), complex chunk parsing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sctp.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Parse SCTP header */
    nsctp_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    nsctp_parse_header(data, size, &hdr);

    /* Verify checksum */
    nsctp_verify_checksum(data, size);

    /* Try parsing the payload area as individual chunk types.
     * SCTP chunks start after the 12-byte common header. */
    if (size > 12) {
        const uint8_t *chunk = data + 12;
        size_t chunk_len = size - 12;

        nsctp_init_t init;
        memset(&init, 0, sizeof(init));
        nsctp_parse_init(chunk, chunk_len, &init);

        nsctp_data_t dat;
        memset(&dat, 0, sizeof(dat));
        nsctp_parse_data(chunk, chunk_len, &dat);

        nsctp_sack_t sack;
        memset(&sack, 0, sizeof(sack));
        nsctp_parse_sack(chunk, chunk_len, &sack);
    }

    return 0;
}
