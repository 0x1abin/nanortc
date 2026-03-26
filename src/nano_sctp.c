/*
 * nanortc — SCTP-Lite implementation (RFC 4960)
 *
 * Minimal SCTP for WebRTC DataChannel over DTLS.
 * Reference: libpeer src/sctp.c, str0m src/sctp/mod.rs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sctp.h"
#include "nano_crc32c.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

/* ================================================================
 * Constants
 * ================================================================ */

#define SCTP_HEADER_SIZE      12 /* common header: src+dst port, vtag, checksum */
#define SCTP_CHUNK_HDR_SIZE   4  /* chunk: type(1) + flags(1) + length(2) */
#define SCTP_INIT_BODY_SIZE   16 /* init body: tag(4)+rwnd(4)+ostreams(2)+istreams(2)+tsn(4) */
#define SCTP_DATA_HDR_SIZE    12 /* data header after chunk hdr: tsn+sid+ssn+ppid */
#define SCTP_SACK_MIN_SIZE    12 /* sack body: cum_tsn(4)+rwnd(4)+ngap(2)+ndup(2) */

/* 4-byte pad length: round up to next multiple of 4 */
#define SCTP_PAD4(x) (((x) + 3u) & ~3u)

/* ================================================================
 * Codec — Parser
 * ================================================================ */

int sctp_parse_header(const uint8_t *data, size_t len, sctp_header_t *hdr)
{
    if (!data || !hdr || len < SCTP_HEADER_SIZE) {
        return NANO_ERR_PARSE;
    }

    hdr->src_port = nano_ntohs(*(const uint16_t *)(data + 0));
    hdr->dst_port = nano_ntohs(*(const uint16_t *)(data + 2));
    hdr->vtag     = nano_ntohl(*(const uint32_t *)(data + 4));
    hdr->checksum = nano_ntohl(*(const uint32_t *)(data + 8));
    return NANO_OK;
}

int sctp_verify_checksum(const uint8_t *data, size_t len)
{
    if (!data || len < SCTP_HEADER_SIZE) {
        return NANO_ERR_PARSE;
    }

    /* Save original checksum, zero the field, compute CRC-32c over copy */
    uint32_t stored = nano_ntohl(*(const uint32_t *)(data + 8));

    /* We need to compute CRC with checksum field zeroed.
     * Copy the packet to a scratch buffer so we don't mutate input. */
    uint8_t scratch[NANO_SCTP_MTU];
    if (len > sizeof(scratch)) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(scratch, data, len);
    *(uint32_t *)(scratch + 8) = 0;

    uint32_t computed = nano_crc32c(scratch, len);
    if (computed != stored) {
        return NANO_ERR_PROTOCOL;
    }
    return NANO_OK;
}

int sctp_parse_init(const uint8_t *chunk, size_t chunk_len, sctp_init_t *out)
{
    /* chunk points to chunk header: type(1)+flags(1)+length(2)+body */
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_INIT_BODY_SIZE) {
        return NANO_ERR_PARSE;
    }

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->initiate_tag = nano_ntohl(*(const uint32_t *)(body + 0));
    out->a_rwnd       = nano_ntohl(*(const uint32_t *)(body + 4));
    out->num_ostreams = nano_ntohs(*(const uint16_t *)(body + 8));
    out->num_istreams = nano_ntohs(*(const uint16_t *)(body + 10));
    out->initial_tsn  = nano_ntohl(*(const uint32_t *)(body + 12));
    out->cookie       = NULL;
    out->cookie_len   = 0;

    /* Scan optional parameters for State Cookie (type=7) */
    uint16_t declared_len = nano_ntohs(*(const uint16_t *)(chunk + 2));
    size_t params_start = SCTP_CHUNK_HDR_SIZE + SCTP_INIT_BODY_SIZE;
    size_t pos = params_start;

    while (pos + 4 <= declared_len && pos + 4 <= chunk_len) {
        uint16_t ptype = nano_ntohs(*(const uint16_t *)(chunk + pos));
        uint16_t plen  = nano_ntohs(*(const uint16_t *)(chunk + pos + 2));
        if (plen < 4) {
            break; /* malformed */
        }
        if (ptype == SCTP_PARAM_STATE_COOKIE) {
            uint16_t cookie_data_len = plen - 4;
            if (pos + plen <= chunk_len) {
                out->cookie = chunk + pos + 4;
                out->cookie_len = cookie_data_len;
            }
        }
        pos += SCTP_PAD4(plen);
    }

    return NANO_OK;
}

int sctp_parse_data(const uint8_t *chunk, size_t chunk_len, sctp_data_t *out)
{
    /* Minimum: chunk_hdr(4) + tsn(4)+sid(2)+ssn(2)+ppid(4) = 16, 0 payload OK */
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE) {
        return NANO_ERR_PARSE;
    }

    out->flags      = chunk[1];
    uint16_t clen   = nano_ntohs(*(const uint16_t *)(chunk + 2));

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->tsn        = nano_ntohl(*(const uint32_t *)(body + 0));
    out->stream_id  = nano_ntohs(*(const uint16_t *)(body + 4));
    out->ssn        = nano_ntohs(*(const uint16_t *)(body + 6));
    out->ppid       = nano_ntohl(*(const uint32_t *)(body + 8));

    uint16_t hdr_total = SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE;
    if (clen > hdr_total && (size_t)clen <= chunk_len) {
        out->payload     = chunk + hdr_total;
        out->payload_len = clen - hdr_total;
    } else {
        out->payload     = NULL;
        out->payload_len = 0;
    }

    return NANO_OK;
}

int sctp_parse_sack(const uint8_t *chunk, size_t chunk_len, sctp_sack_t *out)
{
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE) {
        return NANO_ERR_PARSE;
    }

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->cumulative_tsn = nano_ntohl(*(const uint32_t *)(body + 0));
    out->a_rwnd         = nano_ntohl(*(const uint32_t *)(body + 4));
    out->num_gap_blocks = nano_ntohs(*(const uint16_t *)(body + 8));
    out->num_dup_tsns   = nano_ntohs(*(const uint16_t *)(body + 10));

    return NANO_OK;
}

/* ================================================================
 * Codec — Encoder
 * ================================================================ */

size_t sctp_encode_header(uint8_t *buf, uint16_t src_port, uint16_t dst_port,
                          uint32_t vtag)
{
    *(uint16_t *)(buf + 0) = nano_htons(src_port);
    *(uint16_t *)(buf + 2) = nano_htons(dst_port);
    *(uint32_t *)(buf + 4) = nano_htonl(vtag);
    *(uint32_t *)(buf + 8) = 0; /* checksum placeholder */
    return SCTP_HEADER_SIZE;
}

void sctp_finalize_checksum(uint8_t *packet, size_t len)
{
    /* Zero checksum field, compute CRC-32c, store back */
    *(uint32_t *)(packet + 8) = 0;
    uint32_t crc = nano_crc32c(packet, len);
    *(uint32_t *)(packet + 8) = nano_htonl(crc);
}

size_t sctp_encode_init(uint8_t *buf, uint8_t type, uint32_t initiate_tag,
                        uint32_t a_rwnd, uint16_t num_ostreams,
                        uint16_t num_istreams, uint32_t initial_tsn,
                        const uint8_t *cookie, uint16_t cookie_len)
{
    size_t pos = 0;

    /* Chunk header */
    buf[pos++] = type;
    buf[pos++] = 0; /* flags */

    /* Length placeholder — filled after body + params */
    size_t len_offset = pos;
    pos += 2;

    /* INIT body (16 bytes) */
    *(uint32_t *)(buf + pos) = nano_htonl(initiate_tag);  pos += 4;
    *(uint32_t *)(buf + pos) = nano_htonl(a_rwnd);        pos += 4;
    *(uint16_t *)(buf + pos) = nano_htons(num_ostreams);  pos += 2;
    *(uint16_t *)(buf + pos) = nano_htons(num_istreams);  pos += 2;
    *(uint32_t *)(buf + pos) = nano_htonl(initial_tsn);   pos += 4;

    /* Optional: State Cookie parameter (type=7) for INIT-ACK */
    if (cookie && cookie_len > 0) {
        *(uint16_t *)(buf + pos) = nano_htons(SCTP_PARAM_STATE_COOKIE); pos += 2;
        *(uint16_t *)(buf + pos) = nano_htons(4 + cookie_len);          pos += 2;
        memcpy(buf + pos, cookie, cookie_len);
        pos += cookie_len;
        /* Pad to 4 bytes */
        while (pos & 3) {
            buf[pos++] = 0;
        }
    }

    /* Fill chunk length (unpadded) */
    uint16_t chunk_len = (uint16_t)pos;
    *(uint16_t *)(buf + len_offset) = nano_htons(chunk_len);

    return pos;
}

size_t sctp_encode_cookie_echo(uint8_t *buf, const uint8_t *cookie,
                               uint16_t cookie_len)
{
    buf[0] = SCTP_CHUNK_COOKIE_ECHO;
    buf[1] = 0;
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + cookie_len;
    *(uint16_t *)(buf + 2) = nano_htons(clen);
    memcpy(buf + SCTP_CHUNK_HDR_SIZE, cookie, cookie_len);

    size_t total = SCTP_PAD4(clen);
    /* Zero padding */
    for (size_t i = clen; i < total; i++) {
        buf[i] = 0;
    }
    return total;
}

size_t sctp_encode_cookie_ack(uint8_t *buf)
{
    buf[0] = SCTP_CHUNK_COOKIE_ACK;
    buf[1] = 0;
    *(uint16_t *)(buf + 2) = nano_htons(SCTP_CHUNK_HDR_SIZE);
    return SCTP_CHUNK_HDR_SIZE;
}

size_t sctp_encode_data(uint8_t *buf, uint32_t tsn, uint16_t stream_id,
                        uint16_t ssn, uint32_t ppid, uint8_t flags,
                        const uint8_t *payload, uint16_t payload_len)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE + payload_len;

    buf[0] = SCTP_CHUNK_DATA;
    buf[1] = flags;
    *(uint16_t *)(buf + 2) = nano_htons(clen);

    uint8_t *body = buf + SCTP_CHUNK_HDR_SIZE;
    *(uint32_t *)(body + 0)  = nano_htonl(tsn);
    *(uint16_t *)(body + 4)  = nano_htons(stream_id);
    *(uint16_t *)(body + 6)  = nano_htons(ssn);
    *(uint32_t *)(body + 8)  = nano_htonl(ppid);

    if (payload && payload_len > 0) {
        memcpy(body + SCTP_DATA_HDR_SIZE, payload, payload_len);
    }

    size_t total = SCTP_PAD4(clen);
    for (size_t i = clen; i < total; i++) {
        buf[i] = 0;
    }
    return total;
}

size_t sctp_encode_sack(uint8_t *buf, uint32_t cumulative_tsn, uint32_t a_rwnd)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE;

    buf[0] = SCTP_CHUNK_SACK;
    buf[1] = 0;
    *(uint16_t *)(buf + 2) = nano_htons(clen);

    uint8_t *body = buf + SCTP_CHUNK_HDR_SIZE;
    *(uint32_t *)(body + 0) = nano_htonl(cumulative_tsn);
    *(uint32_t *)(body + 4) = nano_htonl(a_rwnd);
    *(uint16_t *)(body + 8) = 0; /* no gap blocks */
    *(uint16_t *)(body + 10) = 0; /* no dup TSNs */

    return clen;
}

size_t sctp_encode_heartbeat(uint8_t *buf, const uint8_t *info,
                             uint16_t info_len)
{
    /* Heartbeat Info parameter: type=1, length=4+info_len */
    uint16_t param_len = 4 + info_len;
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + param_len;

    buf[0] = SCTP_CHUNK_HEARTBEAT;
    buf[1] = 0;
    *(uint16_t *)(buf + 2) = nano_htons(clen);

    /* Heartbeat Info TLV (RFC 4960 §3.3.5) — type=1 */
    uint8_t *p = buf + SCTP_CHUNK_HDR_SIZE;
    *(uint16_t *)(p + 0) = nano_htons(1); /* Heartbeat Info type */
    *(uint16_t *)(p + 2) = nano_htons(param_len);
    if (info && info_len > 0) {
        memcpy(p + 4, info, info_len);
    }

    size_t total = SCTP_PAD4(clen);
    for (size_t i = clen; i < total; i++) {
        buf[i] = 0;
    }
    return total;
}

size_t sctp_encode_heartbeat_ack(uint8_t *buf, const uint8_t *info,
                                 uint16_t info_len)
{
    /* Same format as HEARTBEAT, just different chunk type */
    size_t n = sctp_encode_heartbeat(buf, info, info_len);
    buf[0] = SCTP_CHUNK_HEARTBEAT_ACK;
    return n;
}

size_t sctp_encode_forward_tsn(uint8_t *buf, uint32_t new_cumulative_tsn)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + 4;
    buf[0] = SCTP_CHUNK_FORWARD_TSN;
    buf[1] = 0;
    *(uint16_t *)(buf + 2) = nano_htons(clen);
    *(uint32_t *)(buf + SCTP_CHUNK_HDR_SIZE) = nano_htonl(new_cumulative_tsn);
    return clen;
}

size_t sctp_encode_shutdown(uint8_t *buf, uint32_t cumulative_tsn)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + 4;
    buf[0] = SCTP_CHUNK_SHUTDOWN;
    buf[1] = 0;
    *(uint16_t *)(buf + 2) = nano_htons(clen);
    *(uint32_t *)(buf + SCTP_CHUNK_HDR_SIZE) = nano_htonl(cumulative_tsn);
    return clen;
}

/* ================================================================
 * State machine — Init / Stubs
 *
 * Full FSM (handshake, DATA/SACK, retransmit) in Session 2.
 * ================================================================ */

int sctp_init(nano_sctp_t *sctp)
{
    if (!sctp) {
        return NANO_ERR_INVALID_PARAM;
    }
    memset(sctp, 0, sizeof(*sctp));
    sctp->state = NANO_SCTP_STATE_CLOSED;
    sctp->local_port = 5000;  /* WebRTC default SCTP port */
    sctp->remote_port = 5000;
    sctp->rto_ms = NANO_SCTP_RTO_INITIAL_MS;
    return NANO_OK;
}

int sctp_handle_data(nano_sctp_t *sctp, const uint8_t *data, size_t len)
{
    if (!sctp || !data || len < SCTP_HEADER_SIZE) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* Verify CRC-32c checksum */
    int rc = sctp_verify_checksum(data, len);
    if (rc != NANO_OK) {
        NANO_LOGW("SCTP", "checksum verification failed");
        return rc;
    }

    /* Parse common header */
    sctp_header_t hdr;
    sctp_parse_header(data, len, &hdr);

    /* Verify verification tag (RFC 4960 §8.5) */
    /* INIT must have vtag=0; others must match local_vtag */
    /* (detailed validation in Session 2 FSM) */

    /* Iterate chunks (libpeer pattern: pos starts after header) */
    size_t pos = SCTP_HEADER_SIZE;
    while (pos + SCTP_CHUNK_HDR_SIZE <= len) {
        uint8_t ctype = data[pos];
        uint16_t clen = nano_ntohs(*(const uint16_t *)(data + pos + 2));

        if (clen < SCTP_CHUNK_HDR_SIZE || pos + clen > len) {
            break; /* malformed or truncated */
        }

        NANO_LOGT("SCTP", "chunk received");

        switch (ctype) {
        case SCTP_CHUNK_INIT:
            NANO_LOGD("SCTP", "INIT received");
            /* Session 2: handle INIT — respond with INIT-ACK */
            break;

        case SCTP_CHUNK_INIT_ACK:
            NANO_LOGD("SCTP", "INIT-ACK received");
            /* Session 2: extract cookie, send COOKIE-ECHO */
            break;

        case SCTP_CHUNK_COOKIE_ECHO:
            NANO_LOGD("SCTP", "COOKIE-ECHO received");
            /* Session 2: validate cookie, send COOKIE-ACK, → ESTABLISHED */
            break;

        case SCTP_CHUNK_COOKIE_ACK:
            NANO_LOGD("SCTP", "COOKIE-ACK received");
            /* Session 2: → ESTABLISHED */
            break;

        case SCTP_CHUNK_DATA: {
            NANO_LOGD("SCTP", "DATA received");
            /* Session 2: process DATA, set sack_needed, deliver */
            break;
        }

        case SCTP_CHUNK_SACK:
            NANO_LOGT("SCTP", "SACK received");
            /* Session 2: process SACK, free send queue */
            break;

        case SCTP_CHUNK_HEARTBEAT:
            NANO_LOGT("SCTP", "HEARTBEAT received");
            /* Session 2: echo as HEARTBEAT-ACK */
            break;

        case SCTP_CHUNK_HEARTBEAT_ACK:
            NANO_LOGT("SCTP", "HEARTBEAT-ACK received");
            break;

        case SCTP_CHUNK_FORWARD_TSN:
            NANO_LOGD("SCTP", "FORWARD-TSN received");
            /* Session 2: advance cumulative TSN */
            break;

        case SCTP_CHUNK_ABORT:
            NANO_LOGW("SCTP", "ABORT received");
            sctp->state = NANO_SCTP_STATE_CLOSED;
            break;

        case SCTP_CHUNK_SHUTDOWN:
            NANO_LOGI("SCTP", "SHUTDOWN received");
            break;

        default:
            NANO_LOGD("SCTP", "unknown chunk type");
            break;
        }

        /* Advance to next chunk, padded to 4 bytes */
        pos += SCTP_PAD4(clen);
    }

    return NANO_OK;
}

int sctp_poll_output(nano_sctp_t *sctp, uint8_t *buf, size_t buf_len,
                     size_t *out_len)
{
    if (!sctp || !buf || !out_len) {
        return NANO_ERR_INVALID_PARAM;
    }

    if (!sctp->has_output || sctp->out_len == 0) {
        *out_len = 0;
        return NANO_ERR_NO_DATA;
    }

    if (buf_len < sctp->out_len) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(buf, sctp->out_buf, sctp->out_len);
    *out_len = sctp->out_len;
    sctp->has_output = false;
    sctp->out_len = 0;
    return NANO_OK;
}

int sctp_send(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid,
              const uint8_t *data, size_t len)
{
    (void)sctp;
    (void)stream_id;
    (void)ppid;
    (void)data;
    (void)len;
    /* Session 2: enqueue to send queue */
    return NANO_ERR_NOT_IMPLEMENTED;
}

int sctp_start(nano_sctp_t *sctp)
{
    (void)sctp;
    /* Session 2: initiate INIT for client role */
    return NANO_ERR_NOT_IMPLEMENTED;
}

int sctp_handle_timeout(nano_sctp_t *sctp, uint32_t now_ms)
{
    (void)sctp;
    (void)now_ms;
    /* Session 2: retransmission + heartbeat */
    return NANO_OK;
}
