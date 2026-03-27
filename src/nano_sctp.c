/*
 * nanortc — SCTP-Lite implementation (RFC 4960)
 *
 * Minimal SCTP for WebRTC DataChannel over DTLS.
 * Reference: str0m src/sctp/mod.rs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sctp.h"
#include "nano_crypto.h"
#include "nano_crc32c.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

/* ================================================================
 * Constants
 * ================================================================ */

#define SCTP_HEADER_SIZE    12 /* common header: src+dst port, vtag, checksum */
#define SCTP_CHUNK_HDR_SIZE 4  /* chunk: type(1) + flags(1) + length(2) */
#define SCTP_INIT_BODY_SIZE 16 /* init body: tag(4)+rwnd(4)+ostreams(2)+istreams(2)+tsn(4) */
#define SCTP_DATA_HDR_SIZE  12 /* data header after chunk hdr: tsn+sid+ssn+ppid */
#define SCTP_SACK_MIN_SIZE  12 /* sack body: cum_tsn(4)+rwnd(4)+ngap(2)+ndup(2) */

/* 4-byte pad length: round up to next multiple of 4 */
#define SCTP_PAD4(x) (((x) + 3u) & ~3u)

/* ================================================================
 * Codec — Parser
 * ================================================================ */

int nsctp_parse_header(const uint8_t *data, size_t len, nsctp_header_t *hdr)
{
    if (!data || !hdr || len < SCTP_HEADER_SIZE) {
        return NANO_ERR_PARSE;
    }

    hdr->src_port = nano_read_u16be(data + 0);
    hdr->dst_port = nano_read_u16be(data + 2);
    hdr->vtag = nano_read_u32be(data + 4);
    hdr->checksum = nano_read_u32be(data + 8);
    return NANO_OK;
}

int nsctp_verify_checksum(const uint8_t *data, size_t len)
{
    if (!data || len < SCTP_HEADER_SIZE) {
        return NANO_ERR_PARSE;
    }

    /* Save original checksum as raw bytes (CRC-32c is stored opaquely,
     * NOT in network byte order — the reflected algorithm produces bytes
     * that match the wire format directly). */
    uint32_t stored;
    memcpy(&stored, data + 8, 4);

    /* We need to compute CRC with checksum field zeroed.
     * Copy the packet to a scratch buffer so we don't mutate input. */
    uint8_t scratch[NANO_SCTP_MTU];
    if (len > sizeof(scratch)) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(scratch, data, len);
    memset(scratch + 8, 0, 4);

    uint32_t computed = nano_crc32c(scratch, len);
    if (computed != stored) {
        return NANO_ERR_PROTOCOL;
    }
    return NANO_OK;
}

int nsctp_parse_init(const uint8_t *chunk, size_t chunk_len, nsctp_init_t *out)
{
    /* chunk points to chunk header: type(1)+flags(1)+length(2)+body */
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_INIT_BODY_SIZE) {
        return NANO_ERR_PARSE;
    }

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->initiate_tag = nano_read_u32be(body + 0);
    out->a_rwnd = nano_read_u32be(body + 4);
    out->num_ostreams = nano_read_u16be(body + 8);
    out->num_istreams = nano_read_u16be(body + 10);
    out->initial_tsn = nano_read_u32be(body + 12);
    out->cookie = NULL;
    out->cookie_len = 0;

    /* Scan optional parameters for State Cookie (type=7) */
    uint16_t declared_len = nano_read_u16be(chunk + 2);
    size_t params_start = SCTP_CHUNK_HDR_SIZE + SCTP_INIT_BODY_SIZE;
    size_t pos = params_start;

    while (pos + 4 <= declared_len && pos + 4 <= chunk_len) {
        uint16_t ptype = nano_read_u16be(chunk + pos);
        uint16_t plen = nano_read_u16be(chunk + pos + 2);
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

int nsctp_parse_data(const uint8_t *chunk, size_t chunk_len, nsctp_data_t *out)
{
    /* Minimum: chunk_hdr(4) + tsn(4)+sid(2)+ssn(2)+ppid(4) = 16, 0 payload OK */
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE) {
        return NANO_ERR_PARSE;
    }

    out->flags = chunk[1];
    uint16_t clen = nano_read_u16be(chunk + 2);

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->tsn = nano_read_u32be(body + 0);
    out->stream_id = nano_read_u16be(body + 4);
    out->ssn = nano_read_u16be(body + 6);
    out->ppid = nano_read_u32be(body + 8);

    uint16_t hdr_total = SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE;
    if (clen > hdr_total && (size_t)clen <= chunk_len) {
        out->payload = chunk + hdr_total;
        out->payload_len = clen - hdr_total;
    } else {
        out->payload = NULL;
        out->payload_len = 0;
    }

    return NANO_OK;
}

int nsctp_parse_sack(const uint8_t *chunk, size_t chunk_len, nsctp_sack_t *out)
{
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE) {
        return NANO_ERR_PARSE;
    }

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->cumulative_tsn = nano_read_u32be(body + 0);
    out->a_rwnd = nano_read_u32be(body + 4);
    out->num_gap_blocks = nano_read_u16be(body + 8);
    out->num_dup_tsns = nano_read_u16be(body + 10);

    return NANO_OK;
}

/* ================================================================
 * Codec — Encoder
 * ================================================================ */

size_t nsctp_encode_header(uint8_t *buf, uint16_t src_port, uint16_t dst_port, uint32_t vtag)
{
    nano_write_u16be(buf + 0, src_port);
    nano_write_u16be(buf + 2, dst_port);
    nano_write_u32be(buf + 4, vtag);
    nano_write_u32be(buf + 8, 0); /* checksum placeholder */
    return SCTP_HEADER_SIZE;
}

void nsctp_finalize_checksum(uint8_t *packet, size_t len)
{
    /* Zero checksum field, compute CRC-32c, store back as raw bytes.
     * The reflected CRC-32c algorithm produces bytes matching wire order. */
    memset(packet + 8, 0, 4);
    uint32_t crc = nano_crc32c(packet, len);
    memcpy(packet + 8, &crc, 4);
}

size_t nsctp_encode_init(uint8_t *buf, uint8_t type, uint32_t initiate_tag, uint32_t a_rwnd,
                         uint16_t num_ostreams, uint16_t num_istreams, uint32_t initial_tsn,
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
    nano_write_u32be(buf + pos, initiate_tag);
    pos += 4;
    nano_write_u32be(buf + pos, a_rwnd);
    pos += 4;
    nano_write_u16be(buf + pos, num_ostreams);
    pos += 2;
    nano_write_u16be(buf + pos, num_istreams);
    pos += 2;
    nano_write_u32be(buf + pos, initial_tsn);
    pos += 4;

    /* Optional: State Cookie parameter (type=7) for INIT-ACK */
    if (cookie && cookie_len > 0) {
        nano_write_u16be(buf + pos, SCTP_PARAM_STATE_COOKIE);
        pos += 2;
        nano_write_u16be(buf + pos, 4 + cookie_len);
        pos += 2;
        memcpy(buf + pos, cookie, cookie_len);
        pos += cookie_len;
        /* Pad to 4 bytes */
        while (pos & 3) {
            buf[pos++] = 0;
        }
    }

    /* Fill chunk length (unpadded) */
    uint16_t chunk_len = (uint16_t)pos;
    nano_write_u16be(buf + len_offset, chunk_len);

    return pos;
}

size_t nsctp_encode_cookie_echo(uint8_t *buf, const uint8_t *cookie, uint16_t cookie_len)
{
    buf[0] = SCTP_CHUNK_COOKIE_ECHO;
    buf[1] = 0;
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + cookie_len;
    nano_write_u16be(buf + 2, clen);
    memcpy(buf + SCTP_CHUNK_HDR_SIZE, cookie, cookie_len);

    size_t total = SCTP_PAD4(clen);
    /* Zero padding */
    for (size_t i = clen; i < total; i++) {
        buf[i] = 0;
    }
    return total;
}

size_t nsctp_encode_cookie_ack(uint8_t *buf)
{
    buf[0] = SCTP_CHUNK_COOKIE_ACK;
    buf[1] = 0;
    nano_write_u16be(buf + 2, SCTP_CHUNK_HDR_SIZE);
    return SCTP_CHUNK_HDR_SIZE;
}

size_t nsctp_encode_data(uint8_t *buf, uint32_t tsn, uint16_t stream_id, uint16_t ssn,
                         uint32_t ppid, uint8_t flags, const uint8_t *payload, uint16_t payload_len)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE + payload_len;

    buf[0] = SCTP_CHUNK_DATA;
    buf[1] = flags;
    nano_write_u16be(buf + 2, clen);

    uint8_t *body = buf + SCTP_CHUNK_HDR_SIZE;
    nano_write_u32be(body + 0, tsn);
    nano_write_u16be(body + 4, stream_id);
    nano_write_u16be(body + 6, ssn);
    nano_write_u32be(body + 8, ppid);

    if (payload && payload_len > 0) {
        memcpy(body + SCTP_DATA_HDR_SIZE, payload, payload_len);
    }

    size_t total = SCTP_PAD4(clen);
    for (size_t i = clen; i < total; i++) {
        buf[i] = 0;
    }
    return total;
}

size_t nsctp_encode_sack(uint8_t *buf, uint32_t cumulative_tsn, uint32_t a_rwnd)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE;

    buf[0] = SCTP_CHUNK_SACK;
    buf[1] = 0;
    nano_write_u16be(buf + 2, clen);

    uint8_t *body = buf + SCTP_CHUNK_HDR_SIZE;
    nano_write_u32be(body + 0, cumulative_tsn);
    nano_write_u32be(body + 4, a_rwnd);
    nano_write_u16be(body + 8, 0);  /* no gap blocks */
    nano_write_u16be(body + 10, 0); /* no dup TSNs */

    return clen;
}

size_t nsctp_encode_heartbeat(uint8_t *buf, const uint8_t *info, uint16_t info_len)
{
    /* Heartbeat Info parameter: type=1, length=4+info_len */
    uint16_t param_len = 4 + info_len;
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + param_len;

    buf[0] = SCTP_CHUNK_HEARTBEAT;
    buf[1] = 0;
    nano_write_u16be(buf + 2, clen);

    /* Heartbeat Info TLV (RFC 4960 §3.3.5) — type=1 */
    uint8_t *p = buf + SCTP_CHUNK_HDR_SIZE;
    nano_write_u16be(p + 0, 1); /* Heartbeat Info type */
    nano_write_u16be(p + 2, param_len);
    if (info && info_len > 0) {
        memcpy(p + 4, info, info_len);
    }

    size_t total = SCTP_PAD4(clen);
    for (size_t i = clen; i < total; i++) {
        buf[i] = 0;
    }
    return total;
}

size_t nsctp_encode_heartbeat_ack(uint8_t *buf, const uint8_t *info, uint16_t info_len)
{
    /* Same format as HEARTBEAT, just different chunk type */
    size_t n = nsctp_encode_heartbeat(buf, info, info_len);
    buf[0] = SCTP_CHUNK_HEARTBEAT_ACK;
    return n;
}

size_t nsctp_encode_forward_tsn(uint8_t *buf, uint32_t new_cumulative_tsn)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + 4;
    buf[0] = SCTP_CHUNK_FORWARD_TSN;
    buf[1] = 0;
    nano_write_u16be(buf + 2, clen);
    nano_write_u32be(buf + SCTP_CHUNK_HDR_SIZE, new_cumulative_tsn);
    return clen;
}

size_t nsctp_encode_shutdown(uint8_t *buf, uint32_t cumulative_tsn)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + 4;
    buf[0] = SCTP_CHUNK_SHUTDOWN;
    buf[1] = 0;
    nano_write_u16be(buf + 2, clen);
    nano_write_u32be(buf + SCTP_CHUNK_HDR_SIZE, cumulative_tsn);
    return clen;
}

/* ================================================================
 * Internal helpers
 * ================================================================ */

/** Check if output queue is full. */
static bool nsctp_out_full(const nano_sctp_t *sctp)
{
    return (uint8_t)(sctp->out_tail - sctp->out_head) >= NANO_SCTP_OUT_QUEUE_SIZE;
}

/** Get write pointer to next output slot. */
static uint8_t *nsctp_out_write_buf(nano_sctp_t *sctp)
{
    return sctp->out_bufs[sctp->out_tail & (NANO_SCTP_OUT_QUEUE_SIZE - 1)];
}

/** Queue an outbound SCTP packet (header already written at current out slot). */
static void nsctp_queue_output(nano_sctp_t *sctp, size_t len)
{
    if (nsctp_out_full(sctp)) {
        return; /* drop if queue full */
    }
    size_t padded = SCTP_PAD4(len);
    if (padded > NANO_SCTP_MTU) {
        padded = NANO_SCTP_MTU;
    }
    uint8_t idx = sctp->out_tail & (NANO_SCTP_OUT_QUEUE_SIZE - 1);
    nsctp_finalize_checksum(sctp->out_bufs[idx], padded);
    sctp->out_lens[idx] = (uint16_t)padded;
    sctp->out_tail++;
    sctp->has_output = (sctp->out_head != sctp->out_tail);
}

/** Begin building an outbound packet in the next output slot. Returns header size (12). */
static size_t nsctp_begin_packet(nano_sctp_t *sctp, uint32_t vtag)
{
    return nsctp_encode_header(nsctp_out_write_buf(sctp), sctp->local_port, sctp->remote_port,
                               vtag);
}

/** Send queue helpers */
static uint8_t sq_count(const nano_sctp_t *sctp)
{
    return sctp->sq_tail - sctp->sq_head;
}

static bool sq_full(const nano_sctp_t *sctp)
{
    return sq_count(sctp) >= NANO_SCTP_MAX_SEND_QUEUE;
}

/* ================================================================
 * State machine
 * ================================================================ */

int nsctp_init(nano_sctp_t *sctp)
{
    if (!sctp) {
        return NANO_ERR_INVALID_PARAM;
    }
    memset(sctp, 0, sizeof(*sctp));
    sctp->state = NANO_SCTP_STATE_CLOSED;
    sctp->local_port = 5000; /* WebRTC default SCTP port */
    sctp->remote_port = 5000;
#if NANO_FEATURE_DC_RELIABLE
    sctp->rto_ms = NANO_SCTP_RTO_INITIAL_MS;
#endif
    return NANO_OK;
}

/* ---- nsctp_start: client sends INIT ---- */

int nsctp_start(nano_sctp_t *sctp)
{
    if (!sctp || !sctp->crypto) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (sctp->state != NANO_SCTP_STATE_CLOSED) {
        return NANO_ERR_STATE;
    }

    /* Generate random local vtag and initial TSN */
    sctp->crypto->random_bytes((uint8_t *)&sctp->local_vtag, 4);
    sctp->crypto->random_bytes((uint8_t *)&sctp->next_tsn, 4);
    if (sctp->local_vtag == 0)
        sctp->local_vtag = 1;
    if (sctp->next_tsn == 0)
        sctp->next_tsn = 1;

    /* Generate cookie secret for future use */
    sctp->crypto->random_bytes(sctp->cookie_secret, sizeof(sctp->cookie_secret));

    /* Build INIT packet (vtag=0 for INIT per RFC 4960 §8.5.1) */
    size_t pos = nsctp_begin_packet(sctp, 0);
    pos += nsctp_encode_init(nsctp_out_write_buf(sctp) + pos, SCTP_CHUNK_INIT, sctp->local_vtag,
                             NANO_SCTP_RECV_BUF_SIZE, 0xFFFF, 0xFFFF, sctp->next_tsn, NULL, 0);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANO_SCTP_STATE_COOKIE_WAIT;
    NANO_LOGI("SCTP", "INIT sent, -> COOKIE_WAIT");
    return NANO_OK;
}

/* ---- Chunk handlers ---- */

static int nsctp_handle_init(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen,
                             const nsctp_header_t *hdr)
{
    (void)hdr;
    nsctp_init_t init;
    if (nsctp_parse_init(chunk, clen, &init) != NANO_OK) {
        return NANO_ERR_PARSE;
    }

    /* Save peer parameters */
    sctp->remote_vtag = init.initiate_tag;
    sctp->peer_initial_tsn = init.initial_tsn;
    sctp->cumulative_tsn = init.initial_tsn - 1;
    sctp->peer_a_rwnd = init.a_rwnd;
    sctp->peer_num_istreams = init.num_istreams;
    sctp->peer_num_ostreams = init.num_ostreams;

    /* Generate our own vtag + TSN if not yet done */
    if (sctp->local_vtag == 0 && sctp->crypto) {
        sctp->crypto->random_bytes((uint8_t *)&sctp->local_vtag, 4);
        sctp->crypto->random_bytes((uint8_t *)&sctp->next_tsn, 4);
        if (sctp->local_vtag == 0)
            sctp->local_vtag = 1;
        if (sctp->next_tsn == 0)
            sctp->next_tsn = 1;
        sctp->crypto->random_bytes(sctp->cookie_secret, sizeof(sctp->cookie_secret));
    }

    /* Build INIT-ACK with a simple cookie.
     * Cookie = cookie_secret XOR'd with initiate_tag (simple, DTLS provides auth).
     * We keep this minimal. */
    uint8_t cookie[8];
    memcpy(cookie, sctp->cookie_secret, 8);
    /* Mix in peer's initiate tag for binding */
    uint32_t tag_be = nano_htonl(init.initiate_tag);
    cookie[0] ^= ((uint8_t *)&tag_be)[0];
    cookie[1] ^= ((uint8_t *)&tag_be)[1];
    cookie[2] ^= ((uint8_t *)&tag_be)[2];
    cookie[3] ^= ((uint8_t *)&tag_be)[3];

    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_init(nsctp_out_write_buf(sctp) + pos, SCTP_CHUNK_INIT_ACK, sctp->local_vtag,
                             NANO_SCTP_RECV_BUF_SIZE, 0xFFFF, 0xFFFF, sctp->next_tsn, cookie,
                             sizeof(cookie));
    nsctp_queue_output(sctp, pos);

    NANO_LOGI("SCTP", "INIT-ACK sent (server)");
    return NANO_OK;
}

static int nsctp_handle_init_ack(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    if (sctp->state != NANO_SCTP_STATE_COOKIE_WAIT) {
        return NANO_ERR_STATE;
    }

    nsctp_init_t init;
    if (nsctp_parse_init(chunk, clen, &init) != NANO_OK) {
        return NANO_ERR_PARSE;
    }

    /* Save peer parameters */
    sctp->remote_vtag = init.initiate_tag;
    sctp->peer_initial_tsn = init.initial_tsn;
    sctp->cumulative_tsn = init.initial_tsn - 1;
    sctp->peer_a_rwnd = init.a_rwnd;

    /* Extract and store cookie */
    if (!init.cookie || init.cookie_len == 0 || init.cookie_len > NANO_SCTP_COOKIE_SIZE) {
        NANO_LOGE("SCTP", "INIT-ACK missing or oversized cookie");
        return NANO_ERR_PROTOCOL;
    }
    memcpy(sctp->cookie, init.cookie, init.cookie_len);
    sctp->cookie_len = init.cookie_len;

    /* Send COOKIE-ECHO */
    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos +=
        nsctp_encode_cookie_echo(nsctp_out_write_buf(sctp) + pos, sctp->cookie, sctp->cookie_len);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANO_SCTP_STATE_COOKIE_ECHOED;
    NANO_LOGI("SCTP", "COOKIE-ECHO sent, -> COOKIE_ECHOED");
    return NANO_OK;
}

static int nsctp_handle_cookie_echo(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    /* Validate cookie (simple: recompute expected cookie and compare) */
    uint16_t chunk_body_len = clen - SCTP_CHUNK_HDR_SIZE;
    const uint8_t *received_cookie = chunk + SCTP_CHUNK_HDR_SIZE;

    /* Recompute expected cookie */
    uint8_t expected[8];
    memcpy(expected, sctp->cookie_secret, 8);
    uint32_t tag_be = nano_htonl(sctp->remote_vtag);
    expected[0] ^= ((uint8_t *)&tag_be)[0];
    expected[1] ^= ((uint8_t *)&tag_be)[1];
    expected[2] ^= ((uint8_t *)&tag_be)[2];
    expected[3] ^= ((uint8_t *)&tag_be)[3];

    if (chunk_body_len < 8 || memcmp(received_cookie, expected, 8) != 0) {
        NANO_LOGW("SCTP", "invalid cookie in COOKIE-ECHO");
        return NANO_ERR_PROTOCOL;
    }

    /* Send COOKIE-ACK */
    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_cookie_ack(nsctp_out_write_buf(sctp) + pos);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANO_SCTP_STATE_ESTABLISHED;
    NANO_LOGI("SCTP", "COOKIE-ACK sent, -> ESTABLISHED (server)");
    return NANO_OK;
}

static int nsctp_handle_cookie_ack(nano_sctp_t *sctp)
{
    if (sctp->state != NANO_SCTP_STATE_COOKIE_ECHOED) {
        return NANO_ERR_STATE;
    }
    sctp->state = NANO_SCTP_STATE_ESTABLISHED;
    NANO_LOGI("SCTP", "-> ESTABLISHED (client)");
    return NANO_OK;
}

static int nsctp_handle_data_chunk(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    if (sctp->state != NANO_SCTP_STATE_ESTABLISHED) {
        return NANO_ERR_STATE;
    }

    nsctp_data_t data;
    if (nsctp_parse_data(chunk, clen, &data) != NANO_OK) {
        return NANO_ERR_PARSE;
    }

    /* Update cumulative TSN (simple: advance if contiguous) */
    if (data.tsn == sctp->cumulative_tsn + 1) {
        sctp->cumulative_tsn = data.tsn;
    }
    /* TODO: handle out-of-order (gap tracking) */

    sctp->sack_needed = true;

    /* Deliver payload to caller */
    sctp->delivered_data = data.payload;
    sctp->delivered_len = data.payload_len;
    sctp->delivered_stream = data.stream_id;
    sctp->delivered_ppid = data.ppid;
    sctp->has_delivered = true;

    return NANO_OK;
}

static int nsctp_handle_sack_chunk(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    nsctp_sack_t sack;
    if (nsctp_parse_sack(chunk, clen, &sack) != NANO_OK) {
        return NANO_ERR_PARSE;
    }

    /* Mark acked entries in send queue */
    uint8_t idx = sctp->sq_head;
    while (idx != sctp->sq_tail) {
        nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANO_SCTP_MAX_SEND_QUEUE - 1)];
        /* TSN comparison: tsn <= cumulative_tsn_ack (handling wrap) */
        int32_t diff = (int32_t)(e->tsn - sack.cumulative_tsn);
        if (diff <= 0 && !e->acked) {
            e->acked = true;
        }
        idx++;
    }

    /* Advance sq_head past acked entries to free space */
    while (sctp->sq_head != sctp->sq_tail) {
        nsctp_send_entry_t *e = &sctp->send_queue[sctp->sq_head & (NANO_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->acked)
            break;
        sctp->sq_head++;
    }

    /* If send queue fully drained, reclaim send_buf */
    if (sctp->sq_head == sctp->sq_tail) {
        sctp->send_buf_used = 0;
    }

    return NANO_OK;
}

static int nsctp_handle_heartbeat(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    /* Echo back as HEARTBEAT-ACK with same Heartbeat Info TLV */
    uint16_t info_offset = SCTP_CHUNK_HDR_SIZE + 4; /* skip TLV type+length */
    if (clen < info_offset) {
        return NANO_ERR_PARSE;
    }

    /* Extract the full Heartbeat Info parameter (including TLV header) */
    const uint8_t *info = chunk + SCTP_CHUNK_HDR_SIZE + 4;
    uint16_t param_len = nano_read_u16be(chunk + SCTP_CHUNK_HDR_SIZE + 2);
    uint16_t info_len = (param_len >= 4) ? (param_len - 4) : 0;

    if ((size_t)(SCTP_CHUNK_HDR_SIZE + 4 + info_len) > clen) {
        return NANO_ERR_PARSE;
    }

    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_heartbeat_ack(nsctp_out_write_buf(sctp) + pos, info, info_len);
    nsctp_queue_output(sctp, pos);

    NANO_LOGT("SCTP", "HEARTBEAT-ACK sent");
    return NANO_OK;
}

static int nsctp_handle_heartbeat_ack(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    (void)chunk;
    (void)clen;
    /* Clear pending heartbeat */
    sctp->heartbeat_pending = false;
    return NANO_OK;
}

static int nsctp_handle_forward_tsn(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    if (clen < SCTP_CHUNK_HDR_SIZE + 4) {
        return NANO_ERR_PARSE;
    }

    uint32_t new_tsn = nano_read_u32be(chunk + SCTP_CHUNK_HDR_SIZE);

    /* Only advance forward */
    int32_t diff = (int32_t)(new_tsn - sctp->cumulative_tsn);
    if (diff > 0) {
        sctp->cumulative_tsn = new_tsn;
        sctp->sack_needed = true;
    }

    NANO_LOGD("SCTP", "cumulative TSN advanced by FORWARD-TSN");
    return NANO_OK;
}

/* ---- Main dispatch ---- */

int nsctp_handle_data(nano_sctp_t *sctp, const uint8_t *data, size_t len)
{
    if (!sctp || !data || len < SCTP_HEADER_SIZE) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* Verify CRC-32c checksum */
    int rc = nsctp_verify_checksum(data, len);
    if (rc != NANO_OK) {
        NANO_LOGW("SCTP", "checksum verification failed");
        return rc;
    }

    /* Parse common header */
    nsctp_header_t hdr;
    nsctp_parse_header(data, len, &hdr);

    /* Clear delivered state from previous call */
    sctp->has_delivered = false;

    /* Iterate chunks */
    size_t pos = SCTP_HEADER_SIZE;
    while (pos + SCTP_CHUNK_HDR_SIZE <= len) {
        uint8_t ctype = data[pos];
        uint16_t clen = nano_read_u16be(data + pos + 2);

        if (clen < SCTP_CHUNK_HDR_SIZE || pos + clen > len) {
            break;
        }

        switch (ctype) {
        case SCTP_CHUNK_INIT:
            NANO_LOGD("SCTP", "INIT received");
            nsctp_handle_init(sctp, data + pos, clen, &hdr);
            break;

        case SCTP_CHUNK_INIT_ACK:
            NANO_LOGD("SCTP", "INIT-ACK received");
            nsctp_handle_init_ack(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_COOKIE_ECHO:
            NANO_LOGD("SCTP", "COOKIE-ECHO received");
            nsctp_handle_cookie_echo(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_COOKIE_ACK:
            NANO_LOGD("SCTP", "COOKIE-ACK received");
            nsctp_handle_cookie_ack(sctp);
            break;

        case SCTP_CHUNK_DATA:
            nsctp_handle_data_chunk(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_SACK:
            nsctp_handle_sack_chunk(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_HEARTBEAT:
            NANO_LOGT("SCTP", "HEARTBEAT received");
            nsctp_handle_heartbeat(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_HEARTBEAT_ACK:
            NANO_LOGT("SCTP", "HEARTBEAT-ACK received");
            nsctp_handle_heartbeat_ack(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_FORWARD_TSN:
            nsctp_handle_forward_tsn(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_ABORT:
            NANO_LOGW("SCTP", "ABORT received");
            sctp->state = NANO_SCTP_STATE_CLOSED;
            break;

        case SCTP_CHUNK_SHUTDOWN:
            NANO_LOGI("SCTP", "SHUTDOWN received");
            break;

        default:
            break;
        }

        pos += SCTP_PAD4(clen);
    }

    /* If DATA was received and we need to send SACK, queue it */
    if (sctp->sack_needed && !nsctp_out_full(sctp)) {
        size_t spos = nsctp_begin_packet(sctp, sctp->remote_vtag);
        spos += nsctp_encode_sack(nsctp_out_write_buf(sctp) + spos, sctp->cumulative_tsn,
                                  NANO_SCTP_RECV_BUF_SIZE);
        nsctp_queue_output(sctp, spos);
        sctp->sack_needed = false;
    }

    return NANO_OK;
}

/* ---- Poll output ---- */

int nsctp_poll_output(nano_sctp_t *sctp, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!sctp || !buf || !out_len) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* First: drain any queued response (handshake, SACK, HEARTBEAT-ACK) */
    if (sctp->out_head != sctp->out_tail) {
        uint8_t ridx = sctp->out_head & (NANO_SCTP_OUT_QUEUE_SIZE - 1);
        uint16_t pkt_len = sctp->out_lens[ridx];
        if (buf_len < pkt_len) {
            return NANO_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(buf, sctp->out_bufs[ridx], pkt_len);
        *out_len = pkt_len;
        sctp->out_head++;
        sctp->has_output = (sctp->out_head != sctp->out_tail);
        return NANO_OK;
    }

    /* Second: encode pending DATA from send queue */
    if (sctp->state == NANO_SCTP_STATE_ESTABLISHED) {
        uint8_t idx = sctp->sq_head;
        while (idx != sctp->sq_tail) {
            nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANO_SCTP_MAX_SEND_QUEUE - 1)];
            if (!e->in_flight && !e->acked) {
                /* Build DATA packet directly into output buffer */
                size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
                pos += nsctp_encode_data(nsctp_out_write_buf(sctp) + pos, e->tsn, e->stream_id,
                                         e->ssn, e->ppid, e->flags, sctp->send_buf + e->data_offset,
                                         e->data_len);

                nsctp_queue_output(sctp, pos);
                e->in_flight = true;

                /* Immediately dequeue the packet we just queued */
                uint8_t ridx = (uint8_t)((sctp->out_head) & (NANO_SCTP_OUT_QUEUE_SIZE - 1));
                uint16_t pkt_len = sctp->out_lens[ridx];
                memcpy(buf, sctp->out_bufs[ridx], pkt_len);
                *out_len = pkt_len;
                sctp->out_head++;
                sctp->has_output = (sctp->out_head != sctp->out_tail);
                return NANO_OK;
            }
            idx++;
        }
    }

    *out_len = 0;
    return NANO_ERR_NO_DATA;
}

/* ---- nsctp_send: enqueue application data ---- */

int nsctp_send(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid, const uint8_t *data,
               size_t len)
{
    if (!sctp) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (sctp->state != NANO_SCTP_STATE_ESTABLISHED) {
        return NANO_ERR_STATE;
    }
    if (sq_full(sctp)) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }
    if (len > 0 && !data) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* Check send buffer space */
    if (sctp->send_buf_used + len > NANO_SCTP_SEND_BUF_SIZE) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy payload into send buffer */
    uint16_t offset = sctp->send_buf_used;
    if (len > 0) {
        memcpy(sctp->send_buf + offset, data, len);
        sctp->send_buf_used += (uint16_t)len;
    }

    /* Create send queue entry */
    nsctp_send_entry_t *e = &sctp->send_queue[sctp->sq_tail & (NANO_SCTP_MAX_SEND_QUEUE - 1)];
    memset(e, 0, sizeof(*e));
    e->tsn = sctp->next_tsn++;
    e->stream_id = stream_id;
#if NANO_FEATURE_DC_ORDERED
    e->ssn = sctp->next_ssn[stream_id % NANO_MAX_DATACHANNELS]++;
    e->flags = SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END; /* single-chunk msg */
#else
    e->ssn = 0;
    e->flags = SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END | SCTP_DATA_FLAG_UNORDERED;
#endif
    e->ppid = ppid;
    e->data_offset = offset;
    e->data_len = (uint16_t)len;
    e->acked = false;
    e->in_flight = false;
#if NANO_FEATURE_DC_RELIABLE
    e->retransmit_count = 0;
#endif

    sctp->sq_tail++;
    NANO_LOGD("SCTP", "DATA enqueued");
    return NANO_OK;
}

/* ---- Timeout handling ---- */

int nsctp_handle_timeout(nano_sctp_t *sctp, uint32_t now_ms)
{
    if (!sctp) {
        return NANO_ERR_INVALID_PARAM;
    }

    if (sctp->state != NANO_SCTP_STATE_ESTABLISHED) {
        return NANO_OK;
    }

#if NANO_FEATURE_DC_RELIABLE
    /* Retransmission: check send queue for timed-out entries */
    uint8_t idx = sctp->sq_head;
    while (idx != sctp->sq_tail) {
        nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANO_SCTP_MAX_SEND_QUEUE - 1)];
        if (e->in_flight && !e->acked) {
            uint32_t elapsed = now_ms - e->sent_at_ms;
            if (elapsed >= sctp->rto_ms) {
                if (e->retransmit_count >= NANO_SCTP_MAX_RETRANSMITS) {
                    NANO_LOGE("SCTP", "max retransmits exceeded");
                    sctp->state = NANO_SCTP_STATE_CLOSED;
                    return NANO_ERR_PROTOCOL;
                }
                /* Mark for retransmission */
                e->in_flight = false;
                e->retransmit_count++;
                e->sent_at_ms = now_ms;

                /* Exponential backoff */
                sctp->rto_ms *= 2;
                if (sctp->rto_ms > NANO_SCTP_RTO_MAX_MS) {
                    sctp->rto_ms = NANO_SCTP_RTO_MAX_MS;
                }
                NANO_LOGD("SCTP", "DATA retransmit scheduled");
            }
        }
        idx++;
    }
#endif /* NANO_FEATURE_DC_RELIABLE */

    /* Heartbeat */
    if (!sctp->heartbeat_pending && sctp->crypto) {
        uint32_t hb_elapsed = now_ms - sctp->last_heartbeat_ms;
        if (hb_elapsed >= NANO_SCTP_HEARTBEAT_INTERVAL_MS) {
            sctp->crypto->random_bytes(sctp->heartbeat_nonce, sizeof(sctp->heartbeat_nonce));
            if (!nsctp_out_full(sctp)) {
                size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
                pos += nsctp_encode_heartbeat(nsctp_out_write_buf(sctp) + pos,
                                              sctp->heartbeat_nonce, sizeof(sctp->heartbeat_nonce));
                nsctp_queue_output(sctp, pos);
                sctp->heartbeat_pending = true;
                sctp->last_heartbeat_ms = now_ms;
                NANO_LOGT("SCTP", "HEARTBEAT sent");
            }
        }
    }

    return NANO_OK;
}
