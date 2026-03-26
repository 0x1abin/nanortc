/*
 * nanortc — SDP parser/generator internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_SDP_H_
#define NANO_SDP_H_

#include <stdint.h>
#include <stddef.h>

typedef struct nano_sdp {
    /* Parsed from remote SDP */
    char remote_ufrag[32];
    char remote_pwd[128];
    char remote_fingerprint[128];
    uint16_t remote_sctp_port;

    /* Local SDP fields */
    char local_ufrag[8];
    char local_pwd[32];
    char local_fingerprint[128];
    uint16_t local_sctp_port;
} nano_sdp_t;

int sdp_init(nano_sdp_t *sdp);
int sdp_parse(nano_sdp_t *sdp, const char *sdp_str, size_t len);
int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *out_len);

#endif /* NANO_SDP_H_ */
