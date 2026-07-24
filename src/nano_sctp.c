/*
 * nanortc — SCTP-Lite implementation (RFC 4960)
 *
 * Minimal SCTP for WebRTC DataChannel over DTLS.
 * Reference: str0m src/sctp/mod.rs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sctp.h"
#include "nanortc_crypto.h"
#include "nano_crc32c.h"
#include "nano_log.h"
#include "nano_time.h"
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
        return NANORTC_ERR_PARSE;
    }

    hdr->src_port = nanortc_read_u16be(data + 0);
    hdr->dst_port = nanortc_read_u16be(data + 2);
    hdr->vtag = nanortc_read_u32be(data + 4);
    return NANORTC_OK;
}

int nsctp_verify_checksum(const uint8_t *data, size_t len)
{
    if (!data || len < SCTP_HEADER_SIZE) {
        return NANORTC_ERR_PARSE;
    }

    /* Save original checksum as raw bytes (CRC-32c is stored opaquely,
     * NOT in network byte order — the reflected algorithm produces bytes
     * that match the wire format directly). */
    uint32_t stored;
    memcpy(&stored, data + 8, 4);

    /* Compute CRC with checksum field treated as zero using segmented API.
     * Avoids copying the entire packet to a scratch buffer (saves ~1200B
     * stack allocation + memcpy on every SCTP packet received). */
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    uint32_t crc = nano_crc32c_init();
    crc = nano_crc32c_update(crc, data, 8);             /* [0..8)  src+dst port, vtag */
    crc = nano_crc32c_update(crc, zeros, 4);            /* [8..12) checksum as zero  */
    crc = nano_crc32c_update(crc, data + 12, len - 12); /* [12..len) chunk data      */
    uint32_t computed = nano_crc32c_final(crc);

    if (computed != stored) {
        return NANORTC_ERR_PROTOCOL;
    }
    return NANORTC_OK;
}

int nsctp_parse_init(const uint8_t *chunk, size_t chunk_len, nsctp_init_t *out)
{
    /* chunk points to chunk header: type(1)+flags(1)+length(2)+body */
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_INIT_BODY_SIZE) {
        return NANORTC_ERR_PARSE;
    }

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->initiate_tag = nanortc_read_u32be(body + 0);
    out->a_rwnd = nanortc_read_u32be(body + 4);
    out->num_ostreams = nanortc_read_u16be(body + 8);
    out->num_istreams = nanortc_read_u16be(body + 10);
    out->initial_tsn = nanortc_read_u32be(body + 12);
    out->cookie = NULL;
    out->cookie_len = 0;
    out->forward_tsn_supported = false;

    /* Scan optional parameters for State Cookie (type=7) */
    uint16_t declared_len = nanortc_read_u16be(chunk + 2);
    size_t params_start = SCTP_CHUNK_HDR_SIZE + SCTP_INIT_BODY_SIZE;
    size_t pos = params_start;

    while (pos + 4 <= declared_len && pos + 4 <= chunk_len) {
        uint16_t ptype = nanortc_read_u16be(chunk + pos);
        uint16_t plen = nanortc_read_u16be(chunk + pos + 2);
        if (plen < 4) {
            break; /* malformed */
        }
        if (ptype == SCTP_PARAM_STATE_COOKIE) {
            uint16_t cookie_data_len = plen - 4;
            if (pos + plen <= chunk_len) {
                out->cookie = chunk + pos + 4;
                out->cookie_len = cookie_data_len;
            }
        } else if (ptype == SCTP_PARAM_FORWARD_TSN_SUPPORTED && plen == 4) {
            out->forward_tsn_supported = true;
        }
        pos += SCTP_PAD4(plen);
    }

    return NANORTC_OK;
}

int nsctp_parse_data(const uint8_t *chunk, size_t chunk_len, nsctp_data_t *out)
{
    /* Minimum: chunk_hdr(4) + tsn(4)+sid(2)+ssn(2)+ppid(4) = 16, 0 payload OK */
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE) {
        return NANORTC_ERR_PARSE;
    }

    out->flags = chunk[1];
    uint16_t clen = nanortc_read_u16be(chunk + 2);

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->tsn = nanortc_read_u32be(body + 0);
    out->stream_id = nanortc_read_u16be(body + 4);
    out->ssn = nanortc_read_u16be(body + 6);
    out->ppid = nanortc_read_u32be(body + 8);

    uint16_t hdr_total = SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE;
    if (clen > hdr_total && (size_t)clen <= chunk_len) {
        out->payload = chunk + hdr_total;
        out->payload_len = clen - hdr_total;
    } else {
        out->payload = NULL;
        out->payload_len = 0;
    }

    return NANORTC_OK;
}

int nsctp_parse_sack(const uint8_t *chunk, size_t chunk_len, nsctp_sack_t *out)
{
    if (!chunk || !out || chunk_len < SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE) {
        return NANORTC_ERR_PARSE;
    }

    const uint8_t *body = chunk + SCTP_CHUNK_HDR_SIZE;
    out->cumulative_tsn = nanortc_read_u32be(body + 0);
    out->a_rwnd = nanortc_read_u32be(body + 4);
    out->num_gap_blocks = nanortc_read_u16be(body + 8);
    out->num_dup_tsns = nanortc_read_u16be(body + 10);

    return NANORTC_OK;
}

/* ================================================================
 * Codec — Encoder
 * ================================================================ */

size_t nsctp_encode_header(uint8_t *buf, uint16_t src_port, uint16_t dst_port, uint32_t vtag)
{
    nanortc_write_u16be(buf + 0, src_port);
    nanortc_write_u16be(buf + 2, dst_port);
    nanortc_write_u32be(buf + 4, vtag);
    nanortc_write_u32be(buf + 8, 0); /* checksum placeholder */
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
    nanortc_write_u32be(buf + pos, initiate_tag);
    pos += 4;
    nanortc_write_u32be(buf + pos, a_rwnd);
    pos += 4;
    nanortc_write_u16be(buf + pos, num_ostreams);
    pos += 2;
    nanortc_write_u16be(buf + pos, num_istreams);
    pos += 2;
    nanortc_write_u32be(buf + pos, initial_tsn);
    pos += 4;

    /* Optional: State Cookie parameter (type=7) for INIT-ACK */
    if (cookie && cookie_len > 0) {
        nanortc_write_u16be(buf + pos, SCTP_PARAM_STATE_COOKIE);
        pos += 2;
        nanortc_write_u16be(buf + pos, 4 + cookie_len);
        pos += 2;
        memcpy(buf + pos, cookie, cookie_len);
        pos += cookie_len;
        /* Pad to 4 bytes */
        while (pos & 3) {
            buf[pos++] = 0;
        }
    }

    /* RFC 3758 §3.1: advertise PR-SCTP support in INIT and INIT-ACK. */
    nanortc_write_u16be(buf + pos, SCTP_PARAM_FORWARD_TSN_SUPPORTED);
    nanortc_write_u16be(buf + pos + 2, 4);
    pos += 4;

    /* Fill chunk length (unpadded) */
    uint16_t chunk_len = (uint16_t)pos;
    nanortc_write_u16be(buf + len_offset, chunk_len);

    return pos;
}

size_t nsctp_encode_cookie_echo(uint8_t *buf, const uint8_t *cookie, uint16_t cookie_len)
{
    buf[0] = SCTP_CHUNK_COOKIE_ECHO;
    buf[1] = 0;
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + cookie_len;
    nanortc_write_u16be(buf + 2, clen);
    memcpy(buf + SCTP_CHUNK_HDR_SIZE, cookie, cookie_len);

    size_t total = SCTP_PAD4(clen);
    /* Zero padding (at most 3 bytes: total - clen ∈ {0,1,2,3}) */
    if (total > clen) {
        memset(buf + clen, 0, total - clen);
    }
    return total;
}

size_t nsctp_encode_cookie_ack(uint8_t *buf)
{
    buf[0] = SCTP_CHUNK_COOKIE_ACK;
    buf[1] = 0;
    nanortc_write_u16be(buf + 2, SCTP_CHUNK_HDR_SIZE);
    return SCTP_CHUNK_HDR_SIZE;
}

size_t nsctp_encode_data(uint8_t *buf, uint32_t tsn, uint16_t stream_id, uint16_t ssn,
                         uint32_t ppid, uint8_t flags, const uint8_t *payload, uint16_t payload_len)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + SCTP_DATA_HDR_SIZE + payload_len;

    buf[0] = SCTP_CHUNK_DATA;
    buf[1] = flags;
    nanortc_write_u16be(buf + 2, clen);

    uint8_t *body = buf + SCTP_CHUNK_HDR_SIZE;
    nanortc_write_u32be(body + 0, tsn);
    nanortc_write_u16be(body + 4, stream_id);
    nanortc_write_u16be(body + 6, ssn);
    nanortc_write_u32be(body + 8, ppid);

    if (payload && payload_len > 0) {
        memcpy(body + SCTP_DATA_HDR_SIZE, payload, payload_len);
    }

    size_t total = SCTP_PAD4(clen);
    if (total > clen) {
        memset(buf + clen, 0, total - clen);
    }
    return total;
}

size_t nsctp_encode_sack(uint8_t *buf, uint32_t cumulative_tsn, uint32_t a_rwnd)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE;

    buf[0] = SCTP_CHUNK_SACK;
    buf[1] = 0;
    nanortc_write_u16be(buf + 2, clen);

    uint8_t *body = buf + SCTP_CHUNK_HDR_SIZE;
    nanortc_write_u32be(body + 0, cumulative_tsn);
    nanortc_write_u32be(body + 4, a_rwnd);
    nanortc_write_u16be(body + 8, 0);  /* no gap blocks */
    nanortc_write_u16be(body + 10, 0); /* no dup TSNs */

    return clen;
}

size_t nsctp_encode_sack_with_gaps(uint8_t *buf, uint32_t cumulative_tsn, uint32_t a_rwnd,
                                   const nano_sctp_t *sctp)
{
    /* Collect gap TSNs and sort them */
    uint32_t gap_tsns[NANORTC_SCTP_MAX_RECV_GAP];
    uint8_t ngaps = 0;
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
        if (sctp->recv_gap[i].valid) {
            gap_tsns[ngaps++] = sctp->recv_gap[i].tsn;
        }
    }

    if (ngaps == 0) {
        return nsctp_encode_sack(buf, cumulative_tsn, a_rwnd);
    }

    /* Simple insertion sort (max NANORTC_SCTP_MAX_RECV_GAP entries) */
    for (uint8_t i = 1; i < ngaps; i++) {
        uint32_t key = gap_tsns[i];
        int8_t j = (int8_t)i - 1;
        /* TSN comparison using signed diff to handle wrapping */
        while (j >= 0 && (int32_t)(gap_tsns[j] - key) > 0) {
            gap_tsns[j + 1] = gap_tsns[j];
            j--;
        }
        gap_tsns[j + 1] = key;
    }

    /* Build gap ack blocks: each block is (start_offset, end_offset) relative to
     * cumulative_tsn. Contiguous TSNs merge into a single block. (RFC 9260 §3.3.4) */
    uint16_t block_starts[NANORTC_SCTP_MAX_GAP_BLOCKS];
    uint16_t block_ends[NANORTC_SCTP_MAX_GAP_BLOCKS];
    uint8_t nblocks = 0;

    for (uint8_t i = 0; i < ngaps && nblocks < NANORTC_SCTP_MAX_GAP_BLOCKS; i++) {
        uint16_t offset = (uint16_t)(gap_tsns[i] - cumulative_tsn);
        if (nblocks > 0 && offset == block_ends[nblocks - 1] + 1) {
            /* Extend current block */
            block_ends[nblocks - 1] = offset;
        } else {
            /* Start new block */
            block_starts[nblocks] = offset;
            block_ends[nblocks] = offset;
            nblocks++;
        }
    }

    /* Encode SACK with gap blocks */
    uint16_t clen =
        SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE + (uint16_t)(nblocks * 4); /* 4 bytes per block */

    buf[0] = SCTP_CHUNK_SACK;
    buf[1] = 0;
    nanortc_write_u16be(buf + 2, clen);

    uint8_t *body = buf + SCTP_CHUNK_HDR_SIZE;
    nanortc_write_u32be(body + 0, cumulative_tsn);
    nanortc_write_u32be(body + 4, a_rwnd);
    nanortc_write_u16be(body + 8, nblocks);
    nanortc_write_u16be(body + 10, 0); /* no dup TSNs */

    /* Gap Ack Block: start(u16) + end(u16) */
    uint8_t *gap_ptr = body + SCTP_SACK_MIN_SIZE;
    for (uint8_t i = 0; i < nblocks; i++) {
        nanortc_write_u16be(gap_ptr + 0, block_starts[i]);
        nanortc_write_u16be(gap_ptr + 2, block_ends[i]);
        gap_ptr += 4;
    }

    return clen;
}

size_t nsctp_encode_heartbeat(uint8_t *buf, const uint8_t *info, uint16_t info_len)
{
    /* Heartbeat Info parameter: type=1, length=4+info_len */
    uint16_t param_len = 4 + info_len;
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + param_len;

    buf[0] = SCTP_CHUNK_HEARTBEAT;
    buf[1] = 0;
    nanortc_write_u16be(buf + 2, clen);

    /* Heartbeat Info TLV (RFC 4960 §3.3.5) — type=1 */
    uint8_t *p = buf + SCTP_CHUNK_HDR_SIZE;
    nanortc_write_u16be(p + 0, 1); /* Heartbeat Info type */
    nanortc_write_u16be(p + 2, param_len);
    if (info && info_len > 0) {
        memcpy(p + 4, info, info_len);
    }

    size_t total = SCTP_PAD4(clen);
    if (total > clen) {
        memset(buf + clen, 0, total - clen);
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
    nanortc_write_u16be(buf + 2, clen);
    nanortc_write_u32be(buf + SCTP_CHUNK_HDR_SIZE, new_cumulative_tsn);
    return clen;
}

size_t nsctp_encode_shutdown(uint8_t *buf, uint32_t cumulative_tsn)
{
    uint16_t clen = SCTP_CHUNK_HDR_SIZE + 4;
    buf[0] = SCTP_CHUNK_SHUTDOWN;
    buf[1] = 0;
    nanortc_write_u16be(buf + 2, clen);
    nanortc_write_u32be(buf + SCTP_CHUNK_HDR_SIZE, cumulative_tsn);
    return clen;
}

/* ================================================================
 * Internal helpers
 * ================================================================ */

/** Check if output queue is full. */
static bool nsctp_out_full(const nano_sctp_t *sctp)
{
    return (uint8_t)(sctp->out_tail - sctp->out_head) >= NANORTC_SCTP_OUT_QUEUE_SIZE;
}

/** Get write pointer to next output slot. */
static uint8_t *nsctp_out_write_buf(nano_sctp_t *sctp)
{
    return sctp->out_bufs[sctp->out_tail & (NANORTC_SCTP_OUT_QUEUE_SIZE - 1)];
}

/** Queue an outbound SCTP packet (header already written at current out slot). */
static void nsctp_queue_output(nano_sctp_t *sctp, size_t len)
{
    if (nsctp_out_full(sctp)) {
        return; /* drop if queue full */
    }
    size_t padded = SCTP_PAD4(len);
    if (padded > NANORTC_SCTP_MTU) {
        padded = NANORTC_SCTP_MTU;
    }
    uint8_t idx = sctp->out_tail & (NANORTC_SCTP_OUT_QUEUE_SIZE - 1);
    nsctp_finalize_checksum(sctp->out_bufs[idx], padded);
    sctp->out_lens[idx] = (uint16_t)padded;
    sctp->out_tail++;
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
    return sq_count(sctp) >= NANORTC_SCTP_MAX_SEND_QUEUE;
}

/* ================================================================
 * State machine
 * ================================================================ */

static int nsctp_generate_association_seed(const nanortc_crypto_provider_t *crypto,
                                           uint32_t *local_vtag, uint32_t *next_tsn,
                                           uint8_t cookie_secret[NSCTP_SECRET_SIZE])
{
    if (!crypto || !crypto->random_bytes || !local_vtag || !next_tsn || !cookie_secret) {
        return NANORTC_ERR_CRYPTO;
    }

    uint32_t vtag = 0;
    uint32_t tsn = 0;
    uint8_t secret[NSCTP_SECRET_SIZE];
    if (crypto->random_bytes((uint8_t *)&vtag, sizeof(vtag)) != 0 ||
        crypto->random_bytes((uint8_t *)&tsn, sizeof(tsn)) != 0 ||
        crypto->random_bytes(secret, sizeof(secret)) != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    *local_vtag = vtag == 0u ? 1u : vtag;
    *next_tsn = tsn == 0u ? 1u : tsn;
    memcpy(cookie_secret, secret, sizeof(secret));
    return NANORTC_OK;
}

int nsctp_init(nano_sctp_t *sctp)
{
    if (!sctp) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(sctp, 0, sizeof(*sctp));
    sctp->state = NANORTC_SCTP_STATE_CLOSED;
    sctp->local_port = 5000; /* WebRTC default SCTP port */
    sctp->remote_port = 5000;
#if NANORTC_FEATURE_DC_RELIABLE
    sctp->rto_ms = NANORTC_SCTP_RTO_INITIAL_MS;
#endif
    return NANORTC_OK;
}

/* ---- nsctp_start: client sends INIT ---- */

int nsctp_start(nano_sctp_t *sctp)
{
    if (!sctp || !sctp->crypto) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (sctp->state != NANORTC_SCTP_STATE_CLOSED) {
        return NANORTC_ERR_STATE;
    }

    /* Generate the complete association seed off-struct. A provider failure at
     * any call leaves CLOSED state and the output queue untouched. */
    uint32_t local_vtag = 0;
    uint32_t next_tsn = 0;
    uint8_t cookie_secret[NSCTP_SECRET_SIZE];
    int rc = nsctp_generate_association_seed(sctp->crypto, &local_vtag, &next_tsn, cookie_secret);
    if (rc != NANORTC_OK) {
        return rc;
    }

    sctp->local_vtag = local_vtag;
    sctp->next_tsn = next_tsn;
    memcpy(sctp->cookie_secret, cookie_secret, sizeof(cookie_secret));

    /* Build INIT packet (vtag=0 for INIT per RFC 4960 §8.5.1) */
    size_t pos = nsctp_begin_packet(sctp, 0);
    pos += nsctp_encode_init(nsctp_out_write_buf(sctp) + pos, SCTP_CHUNK_INIT, sctp->local_vtag,
                             NANORTC_SCTP_RECV_BUF_SIZE, 0xFFFF, 0xFFFF, sctp->next_tsn, NULL, 0);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANORTC_SCTP_STATE_COOKIE_WAIT;
    NANORTC_LOGI("SCTP", "INIT sent, -> COOKIE_WAIT");
    return NANORTC_OK;
}

/* ---- Chunk handlers ---- */

static int nsctp_handle_init(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen,
                             const nsctp_header_t *hdr)
{
    (void)hdr;
    nsctp_init_t init;
    if (nsctp_parse_init(chunk, clen, &init) != NANORTC_OK) {
        return NANORTC_ERR_PARSE;
    }

    /* Generate our own vtag + TSN before committing peer parameters. */
    uint32_t local_vtag = sctp->local_vtag;
    uint32_t next_tsn = sctp->next_tsn;
    uint8_t cookie_secret[NSCTP_SECRET_SIZE];
    memcpy(cookie_secret, sctp->cookie_secret, sizeof(cookie_secret));
    if (local_vtag == 0u) {
        int rc =
            nsctp_generate_association_seed(sctp->crypto, &local_vtag, &next_tsn, cookie_secret);
        if (rc != NANORTC_OK) {
            return rc;
        }
    }

    /* Commit both local randomness and parsed peer state together. */
    sctp->local_vtag = local_vtag;
    sctp->next_tsn = next_tsn;
    memcpy(sctp->cookie_secret, cookie_secret, sizeof(cookie_secret));
    sctp->remote_vtag = init.initiate_tag;
    sctp->peer_initial_tsn = init.initial_tsn;
    sctp->cumulative_tsn = init.initial_tsn - 1;
    sctp->peer_a_rwnd = init.a_rwnd;
    sctp->peer_num_istreams = init.num_istreams;
    sctp->peer_num_ostreams = init.num_ostreams;
    sctp->peer_supports_forward_tsn = init.forward_tsn_supported;

    /* Build INIT-ACK with a simple cookie.
     * Cookie = cookie_secret XOR'd with initiate_tag (simple, DTLS provides auth).
     * We keep this minimal. */
    uint8_t cookie[8];
    memcpy(cookie, sctp->cookie_secret, 8);
    /* Mix in peer's initiate tag for binding */
    uint32_t tag_be = nanortc_htonl(init.initiate_tag);
    cookie[0] ^= ((uint8_t *)&tag_be)[0];
    cookie[1] ^= ((uint8_t *)&tag_be)[1];
    cookie[2] ^= ((uint8_t *)&tag_be)[2];
    cookie[3] ^= ((uint8_t *)&tag_be)[3];

    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_init(nsctp_out_write_buf(sctp) + pos, SCTP_CHUNK_INIT_ACK, sctp->local_vtag,
                             NANORTC_SCTP_RECV_BUF_SIZE, 0xFFFF, 0xFFFF, sctp->next_tsn, cookie,
                             sizeof(cookie));
    nsctp_queue_output(sctp, pos);

    NANORTC_LOGI("SCTP", "INIT-ACK sent (server)");
    return NANORTC_OK;
}

static int nsctp_handle_init_ack(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    if (sctp->state != NANORTC_SCTP_STATE_COOKIE_WAIT) {
        return NANORTC_ERR_STATE;
    }

    nsctp_init_t init;
    if (nsctp_parse_init(chunk, clen, &init) != NANORTC_OK) {
        return NANORTC_ERR_PARSE;
    }

    /* Save peer parameters */
    sctp->remote_vtag = init.initiate_tag;
    sctp->peer_initial_tsn = init.initial_tsn;
    sctp->cumulative_tsn = init.initial_tsn - 1;
    sctp->peer_a_rwnd = init.a_rwnd;
    sctp->peer_supports_forward_tsn = init.forward_tsn_supported;

    /* Extract and store cookie */
    if (!init.cookie || init.cookie_len == 0 || init.cookie_len > NANORTC_SCTP_COOKIE_SIZE) {
        NANORTC_LOGE("SCTP", "INIT-ACK missing or oversized cookie");
        return NANORTC_ERR_PROTOCOL;
    }
    memcpy(sctp->cookie, init.cookie, init.cookie_len);
    sctp->cookie_len = init.cookie_len;

    /* Send COOKIE-ECHO */
    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos +=
        nsctp_encode_cookie_echo(nsctp_out_write_buf(sctp) + pos, sctp->cookie, sctp->cookie_len);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANORTC_SCTP_STATE_COOKIE_ECHOED;
    NANORTC_LOGI("SCTP", "COOKIE-ECHO sent, -> COOKIE_ECHOED");
    return NANORTC_OK;
}

static int nsctp_handle_cookie_echo(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    /* Validate cookie (simple: recompute expected cookie and compare) */
    uint16_t chunk_body_len = clen - SCTP_CHUNK_HDR_SIZE;
    const uint8_t *received_cookie = chunk + SCTP_CHUNK_HDR_SIZE;

    /* Recompute expected cookie */
    uint8_t expected[8];
    memcpy(expected, sctp->cookie_secret, 8);
    uint32_t tag_be = nanortc_htonl(sctp->remote_vtag);
    expected[0] ^= ((uint8_t *)&tag_be)[0];
    expected[1] ^= ((uint8_t *)&tag_be)[1];
    expected[2] ^= ((uint8_t *)&tag_be)[2];
    expected[3] ^= ((uint8_t *)&tag_be)[3];

    if (chunk_body_len < 8 || memcmp(received_cookie, expected, 8) != 0) {
        NANORTC_LOGW("SCTP", "invalid cookie in COOKIE-ECHO");
        return NANORTC_ERR_PROTOCOL;
    }

    /* Send COOKIE-ACK */
    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_cookie_ack(nsctp_out_write_buf(sctp) + pos);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANORTC_SCTP_STATE_ESTABLISHED;
    NANORTC_LOGI("SCTP", "COOKIE-ACK sent, -> ESTABLISHED (server)");
    return NANORTC_OK;
}

static int nsctp_handle_cookie_ack(nano_sctp_t *sctp)
{
    if (sctp->state != NANORTC_SCTP_STATE_COOKIE_ECHOED) {
        return NANORTC_ERR_STATE;
    }
    sctp->state = NANORTC_SCTP_STATE_ESTABLISHED;
    NANORTC_LOGI("SCTP", "-> ESTABLISHED (client)");
    return NANORTC_OK;
}

/* ---- Gap tracking helpers (RFC 9260 §6.2) ---- */

/** Enqueue a message into the delivery queue (for gap-fill batch delivery). */
static bool nsctp_enqueue_delivery(nano_sctp_t *sctp, uint32_t tsn, uint16_t data_offset,
                                    uint16_t data_len, uint16_t stream_id, uint16_t ssn,
                                    uint32_t ppid, uint8_t flags)
{
    uint8_t next = sctp->dq_tail + 1;
    if (next - sctp->dq_head >= NANORTC_SCTP_MAX_RECV_GAP) {
        return false;
    }
    uint8_t idx = sctp->dq_tail & (NANORTC_SCTP_MAX_RECV_GAP - 1);
    sctp->deliver_queue[idx].tsn = tsn;
    sctp->deliver_queue[idx].data_offset = data_offset;
    sctp->deliver_queue[idx].data_len = data_len;
    sctp->deliver_queue[idx].stream_id = stream_id;
    sctp->deliver_queue[idx].ssn = ssn;
    sctp->deliver_queue[idx].ppid = ppid;
    sctp->deliver_queue[idx].flags = flags;
    sctp->dq_tail++;
    return true;
}

static void nsctp_reset_reassembly(nano_sctp_t *sctp)
{
    sctp->recv_message_len = 0;
    sctp->recv_message_active = false;
}

static uint8_t *nsctp_reassembly_buf(nano_sctp_t *sctp)
{
    /* The tail of recv_gap_buf is reserved for one fragmented SCTP user
     * message. This keeps the embedded association footprint flat while the
     * lower portion remains an arena for out-of-order chunks. */
    return sctp->recv_gap_buf +
           (NANORTC_SCTP_RECV_GAP_BUF_SIZE - NANORTC_SCTP_REASSEMBLY_BUF_SIZE);
}

/** Consume one DATA chunk and expose only complete user messages.
 *
 * RFC 4960 section 6.9 permits a user message to span B/middle/E chunks.
 * In particular, Chromium can split a small unordered DataChannel message at
 * a 16-byte boundary. Delivering those chunks individually truncates JSON and
 * can turn a heartbeat/neutral update into a protocol fault.
 */
static bool nsctp_deliver_chunk(nano_sctp_t *sctp, uint32_t tsn, uint16_t stream_id,
                                uint16_t ssn, uint32_t ppid, uint8_t flags,
                                const uint8_t *payload, uint16_t payload_len)
{
    bool begin = (flags & SCTP_DATA_FLAG_BEGIN) != 0;
    bool end = (flags & SCTP_DATA_FLAG_END) != 0;
    bool unordered = (flags & SCTP_DATA_FLAG_UNORDERED) != 0;

    if (begin && end) {
        nsctp_reset_reassembly(sctp);
        sctp->delivered_data = payload;
        sctp->delivered_len = payload_len;
        sctp->delivered_stream = stream_id;
        sctp->delivered_ppid = ppid;
        sctp->has_delivered = true;
        return true;
    }

    if (begin) {
        nsctp_reset_reassembly(sctp);
        if (payload_len > NANORTC_SCTP_REASSEMBLY_BUF_SIZE) {
            NANORTC_LOGW("SCTP", "fragmented message exceeds receive buffer");
            return false;
        }
        if (payload_len > 0) {
            memcpy(nsctp_reassembly_buf(sctp), payload, payload_len);
        }
        sctp->recv_message_len = payload_len;
        sctp->recv_message_stream = stream_id;
        sctp->recv_message_ssn = ssn;
        sctp->recv_message_ppid = ppid;
        sctp->recv_message_next_tsn = tsn + 1;
        sctp->recv_message_unordered = unordered;
        sctp->recv_message_active = true;
        return false;
    }

    if (!sctp->recv_message_active || sctp->recv_message_stream != stream_id ||
        sctp->recv_message_ssn != ssn || sctp->recv_message_ppid != ppid ||
        sctp->recv_message_unordered != unordered || sctp->recv_message_next_tsn != tsn) {
        nsctp_reset_reassembly(sctp);
        NANORTC_LOGW("SCTP", "discarding orphaned or non-contiguous DATA fragment");
        return false;
    }
    if ((size_t)sctp->recv_message_len + payload_len > NANORTC_SCTP_REASSEMBLY_BUF_SIZE) {
        nsctp_reset_reassembly(sctp);
        NANORTC_LOGW("SCTP", "fragmented message exceeds receive buffer");
        return false;
    }
    if (payload_len > 0) {
        memcpy(nsctp_reassembly_buf(sctp) + sctp->recv_message_len, payload, payload_len);
    }
    sctp->recv_message_len += payload_len;
    sctp->recv_message_next_tsn++;
    if (!end) {
        return false;
    }

    sctp->delivered_data = nsctp_reassembly_buf(sctp);
    sctp->delivered_len = sctp->recv_message_len;
    sctp->delivered_stream = stream_id;
    sctp->delivered_ppid = ppid;
    sctp->has_delivered = true;
    sctp->recv_message_active = false;
    return true;
}

/** Buffer an out-of-order DATA chunk into the gap array.
 *  Returns 0 on success, negative on error (no space). */
static int nsctp_gap_insert(nano_sctp_t *sctp, const nsctp_data_t *data, bool delivered)
{
    /* Check for duplicates */
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
        if (sctp->recv_gap[i].valid && sctp->recv_gap[i].tsn == data->tsn) {
            return NANORTC_OK; /* duplicate, ignore */
        }
    }

    /* Find free slot */
    uint8_t slot = NANORTC_SCTP_MAX_RECV_GAP;
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
        if (!sctp->recv_gap[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot >= NANORTC_SCTP_MAX_RECV_GAP) {
        NANORTC_LOGW("SCTP", "gap buffer full, dropping out-of-order TSN");
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Check payload fits in recv_gap_buf */
    if (sctp->recv_gap_buf_used + data->payload_len >
        NANORTC_SCTP_RECV_GAP_BUF_SIZE - NANORTC_SCTP_REASSEMBLY_BUF_SIZE) {
        NANORTC_LOGW("SCTP", "gap buf storage full, dropping out-of-order TSN");
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy payload into recv_gap_buf */
    uint16_t offset = sctp->recv_gap_buf_used;
    if (data->payload && data->payload_len > 0) {
        memcpy(sctp->recv_gap_buf + offset, data->payload, data->payload_len);
    }
    sctp->recv_gap_buf_used += data->payload_len;

    sctp->recv_gap[slot].tsn = data->tsn;
    sctp->recv_gap[slot].data_offset = offset;
    sctp->recv_gap[slot].data_len = data->payload_len;
    sctp->recv_gap[slot].stream_id = data->stream_id;
    sctp->recv_gap[slot].ssn = data->ssn;
    sctp->recv_gap[slot].ppid = data->ppid;
    sctp->recv_gap[slot].flags = data->flags;
    sctp->recv_gap[slot].delivered = delivered;
    sctp->recv_gap[slot].valid = true;
    sctp->recv_gap_count++;

    NANORTC_LOGD("SCTP", "gap buffered out-of-order TSN");
    return NANORTC_OK;
}

/** After advancing cumulative_tsn, drain any contiguous gap entries.
 *  Enqueues their data into the delivery queue. */
static void nsctp_gap_drain(nano_sctp_t *sctp)
{
    bool progress = true;
    while (progress) {
        progress = false;
        for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
            if (!sctp->recv_gap[i].valid) {
                continue;
            }
            if (sctp->recv_gap[i].tsn == sctp->cumulative_tsn + 1) {
                /* This gap entry is now contiguous — deliver it */
                sctp->cumulative_tsn = sctp->recv_gap[i].tsn;

                if (!sctp->recv_gap[i].delivered) {
                    nsctp_enqueue_delivery(sctp, sctp->recv_gap[i].tsn,
                                           sctp->recv_gap[i].data_offset,
                                           sctp->recv_gap[i].data_len,
                                           sctp->recv_gap[i].stream_id,
                                           sctp->recv_gap[i].ssn,
                                           sctp->recv_gap[i].ppid,
                                           sctp->recv_gap[i].flags);
                }

                sctp->recv_gap[i].valid = false;
                sctp->recv_gap_count--;
                progress = true;
                /* Restart scan since we advanced cumulative_tsn */
                break;
            }
            /* Also discard entries at or below cumulative_tsn (stale) */
            int32_t diff = (int32_t)(sctp->recv_gap[i].tsn - sctp->cumulative_tsn);
            if (diff <= 0) {
                sctp->recv_gap[i].valid = false;
                sctp->recv_gap_count--;
                progress = true;
                break;
            }
        }
    }

}

int nsctp_poll_delivery(nano_sctp_t *sctp)
{
    if (!sctp || sctp->dq_head == sctp->dq_tail) {
        return NANORTC_ERR_WOULD_BLOCK;
    }

    while (sctp->dq_head != sctp->dq_tail) {
        uint8_t idx = sctp->dq_head & (NANORTC_SCTP_MAX_RECV_GAP - 1);
        sctp->dq_head++;
        if (nsctp_deliver_chunk(sctp, sctp->deliver_queue[idx].tsn,
                                sctp->deliver_queue[idx].stream_id,
                                sctp->deliver_queue[idx].ssn,
                                sctp->deliver_queue[idx].ppid,
                                sctp->deliver_queue[idx].flags,
                                sctp->recv_gap_buf + sctp->deliver_queue[idx].data_offset,
                                sctp->deliver_queue[idx].data_len)) {
            return NANORTC_OK;
        }
    }
    return NANORTC_ERR_WOULD_BLOCK;
}

static int nsctp_handle_data_chunk(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED) {
        return NANORTC_ERR_STATE;
    }

    nsctp_data_t data;
    if (nsctp_parse_data(chunk, clen, &data) != NANORTC_OK) {
        return NANORTC_ERR_PARSE;
    }

    sctp->sack_needed = true;

    /* Duplicate check: already received (RFC 9260 §6.2) */
    int32_t diff = (int32_t)(data.tsn - sctp->cumulative_tsn);
    if (diff <= 0) {
        return NANORTC_OK; /* duplicate or old TSN, ignore */
    }

    if (data.tsn == sctp->cumulative_tsn + 1) {
        /* In-order: advance cumulative TSN, deliver immediately */
        sctp->cumulative_tsn = data.tsn;

        nsctp_deliver_chunk(sctp, data.tsn, data.stream_id, data.ssn, data.ppid,
                            data.flags, data.payload, data.payload_len);

        /* Drain any gap entries that are now contiguous */
        nsctp_gap_drain(sctp);
    } else {
        /* An unordered DataChannel message is deliverable immediately even
         * while a lower TSN is missing. Keep a gap record for SACK/cumulative
         * tracking, but mark it delivered so gap fill cannot replay it. */
        bool unordered = (data.flags & SCTP_DATA_FLAG_UNORDERED) != 0;
        if (nsctp_gap_insert(sctp, &data, false) == NANORTC_OK && unordered) {
            for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
                if (sctp->recv_gap[i].valid && sctp->recv_gap[i].tsn == data.tsn) {
                    sctp->recv_gap[i].delivered =
                        nsctp_enqueue_delivery(sctp, sctp->recv_gap[i].tsn,
                                                sctp->recv_gap[i].data_offset,
                                                sctp->recv_gap[i].data_len,
                                                sctp->recv_gap[i].stream_id, data.ssn,
                                                sctp->recv_gap[i].ppid,
                                                sctp->recv_gap[i].flags);
                    break;
                }
            }
        }
    }

    return NANORTC_OK;
}

static void nsctp_advance_send_head(nano_sctp_t *sctp)
{
    bool abandoned_prefix = false;
    uint32_t forward_tsn = 0;
    while (sctp->sq_head != sctp->sq_tail) {
        nsctp_send_entry_t *e =
            &sctp->send_queue[sctp->sq_head & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->acked && !e->abandoned)
            break;
        if (e->abandoned) {
            abandoned_prefix = true;
            forward_tsn = e->tsn;
        }
        sctp->sq_head++;
    }
    if (abandoned_prefix) {
        sctp->pending_forward_tsn = forward_tsn;
        sctp->forward_tsn_pending = true;
    }
    if (sctp->sq_head == sctp->sq_tail)
        sctp->send_buf_used = 0;
}

static int nsctp_handle_sack_chunk(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    nsctp_sack_t sack;
    if (nsctp_parse_sack(chunk, clen, &sack) != NANORTC_OK) {
        return NANORTC_ERR_PARSE;
    }
    size_t gap_bytes = (size_t)sack.num_gap_blocks * 4u;
    if ((size_t)SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE + gap_bytes > clen)
        return NANORTC_ERR_PARSE;

    /* Mark cumulative and selective gap acknowledgements. */
    uint32_t highest_acked_tsn = sack.cumulative_tsn;
    uint8_t idx = sctp->sq_head;
    while (idx != sctp->sq_tail) {
        nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        int32_t diff = (int32_t)(e->tsn - sack.cumulative_tsn);
        bool acked = diff <= 0;
        if (!acked && (uint32_t)diff <= UINT16_MAX) {
            const uint8_t *gap = chunk + SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE;
            for (uint16_t block = 0; block < sack.num_gap_blocks; block++, gap += 4) {
                uint16_t start = nanortc_read_u16be(gap);
                uint16_t end = nanortc_read_u16be(gap + 2);
                if ((uint32_t)diff >= start && (uint32_t)diff <= end) {
                    acked = true;
                    break;
                }
            }
        }
        if (acked) {
            e->acked = true;
            if ((int32_t)(e->tsn - highest_acked_tsn) > 0)
                highest_acked_tsn = e->tsn;
        }
        idx++;
    }

#if NANORTC_FEATURE_DC_RELIABLE
    /* A later selectively acknowledged TSN proves an earlier zero-retransmit
     * message was lost. Abandon it immediately instead of waiting for the
     * one-second RTO and filling the bounded send ring. */
    for (idx = sctp->sq_head; idx != sctp->sq_tail; idx++) {
        nsctp_send_entry_t *e =
            &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->acked && !e->abandoned && e->partial_reliability &&
            e->max_retransmits == 0 && sctp->peer_supports_forward_tsn &&
            (int32_t)(highest_acked_tsn - e->tsn) > 0) {
            e->in_flight = false;
            e->abandoned = true;
        }
    }
#endif
    nsctp_advance_send_head(sctp);

    return NANORTC_OK;
}

static int nsctp_handle_heartbeat(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    /* Echo back as HEARTBEAT-ACK with same Heartbeat Info TLV */
    uint16_t info_offset = SCTP_CHUNK_HDR_SIZE + 4; /* skip TLV type+length */
    if (clen < info_offset) {
        return NANORTC_ERR_PARSE;
    }

    /* Extract the full Heartbeat Info parameter (including TLV header) */
    const uint8_t *info = chunk + SCTP_CHUNK_HDR_SIZE + 4;
    uint16_t param_len = nanortc_read_u16be(chunk + SCTP_CHUNK_HDR_SIZE + 2);
    uint16_t info_len = (param_len >= 4) ? (param_len - 4) : 0;

    if ((size_t)(SCTP_CHUNK_HDR_SIZE + 4 + info_len) > clen) {
        return NANORTC_ERR_PARSE;
    }

    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_heartbeat_ack(nsctp_out_write_buf(sctp) + pos, info, info_len);
    nsctp_queue_output(sctp, pos);

    NANORTC_LOGT("SCTP", "HEARTBEAT-ACK sent");
    return NANORTC_OK;
}

static int nsctp_handle_heartbeat_ack(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    (void)chunk;
    (void)clen;
    /* Clear pending heartbeat */
    sctp->heartbeat_pending = false;
    return NANORTC_OK;
}

static int nsctp_handle_forward_tsn(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    if (clen < SCTP_CHUNK_HDR_SIZE + 4) {
        return NANORTC_ERR_PARSE;
    }

    uint32_t new_tsn = nanortc_read_u32be(chunk + SCTP_CHUNK_HDR_SIZE);

    /* Only advance forward */
    int32_t diff = (int32_t)(new_tsn - sctp->cumulative_tsn);
    if (diff > 0) {
        if (sctp->recv_message_active &&
            (int32_t)(new_tsn - sctp->recv_message_next_tsn) >= 0) {
            nsctp_reset_reassembly(sctp);
        }
        sctp->cumulative_tsn = new_tsn;
        sctp->sack_needed = true;
        nsctp_gap_drain(sctp);
    }

    NANORTC_LOGD("SCTP", "cumulative TSN advanced by FORWARD-TSN");
    return NANORTC_OK;
}

/* ---- Main dispatch ---- */

int nsctp_handle_data(nano_sctp_t *sctp, const uint8_t *data, size_t len)
{
    if (!sctp || !data || len < SCTP_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Verify CRC-32c checksum */
    int rc = nsctp_verify_checksum(data, len);
    if (rc != NANORTC_OK) {
        NANORTC_LOGW("SCTP", "checksum verification failed");
        return rc;
    }

    /* Parse common header */
    nsctp_header_t hdr;
    nsctp_parse_header(data, len, &hdr);

    /* Clear delivered state from previous call. Once the caller consumed the
     * final queued message it is safe to reclaim the shared gap payload arena;
     * reclaiming it in nsctp_poll_delivery() invalidated delivered_data before
     * the DataChannel event callback ran. */
    sctp->has_delivered = false;
    if (sctp->recv_gap_count == 0 && sctp->dq_head == sctp->dq_tail) {
        sctp->recv_gap_buf_used = 0;
    }

    /* Iterate chunks */
    size_t pos = SCTP_HEADER_SIZE;
    while (pos + SCTP_CHUNK_HDR_SIZE <= len) {
        uint8_t ctype = data[pos];
        uint16_t clen = nanortc_read_u16be(data + pos + 2);

        if (clen < SCTP_CHUNK_HDR_SIZE || pos + clen > len) {
            break;
        }

        switch (ctype) {
        case SCTP_CHUNK_INIT:
            NANORTC_LOGD("SCTP", "INIT received");
            rc = nsctp_handle_init(sctp, data + pos, clen, &hdr);
            if (rc != NANORTC_OK) {
                return rc;
            }
            break;

        case SCTP_CHUNK_INIT_ACK:
            NANORTC_LOGD("SCTP", "INIT-ACK received");
            nsctp_handle_init_ack(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_COOKIE_ECHO:
            NANORTC_LOGD("SCTP", "COOKIE-ECHO received");
            nsctp_handle_cookie_echo(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_COOKIE_ACK:
            NANORTC_LOGD("SCTP", "COOKIE-ACK received");
            nsctp_handle_cookie_ack(sctp);
            break;

        case SCTP_CHUNK_DATA:
            nsctp_handle_data_chunk(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_SACK:
            nsctp_handle_sack_chunk(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_HEARTBEAT:
            NANORTC_LOGT("SCTP", "HEARTBEAT received");
            nsctp_handle_heartbeat(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_HEARTBEAT_ACK:
            NANORTC_LOGT("SCTP", "HEARTBEAT-ACK received");
            nsctp_handle_heartbeat_ack(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_FORWARD_TSN:
            nsctp_handle_forward_tsn(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_ABORT:
            NANORTC_LOGW("SCTP", "ABORT received");
            sctp->state = NANORTC_SCTP_STATE_CLOSED;
            break;

        case SCTP_CHUNK_SHUTDOWN:
            NANORTC_LOGI("SCTP", "SHUTDOWN received");
            break;

        default:
            break;
        }

        pos += SCTP_PAD4(clen);
    }

    /* If DATA was received and we need to send SACK, queue it */
    if (sctp->sack_needed && !nsctp_out_full(sctp)) {
        size_t spos = nsctp_begin_packet(sctp, sctp->remote_vtag);
        if (sctp->recv_gap_count > 0) {
            spos +=
                nsctp_encode_sack_with_gaps(nsctp_out_write_buf(sctp) + spos, sctp->cumulative_tsn,
                                            NANORTC_SCTP_RECV_BUF_SIZE, sctp);
        } else {
            spos += nsctp_encode_sack(nsctp_out_write_buf(sctp) + spos, sctp->cumulative_tsn,
                                      NANORTC_SCTP_RECV_BUF_SIZE);
        }
        nsctp_queue_output(sctp, spos);
        sctp->sack_needed = false;
    }

    return NANORTC_OK;
}

/* ---- Poll output ---- */

int nsctp_poll_output(nano_sctp_t *sctp, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!sctp || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* First: drain any queued response (handshake, SACK, HEARTBEAT-ACK) */
    if (sctp->out_head != sctp->out_tail) {
        uint8_t ridx = sctp->out_head & (NANORTC_SCTP_OUT_QUEUE_SIZE - 1);
        uint16_t pkt_len = sctp->out_lens[ridx];
        if (buf_len < pkt_len) {
            return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(buf, sctp->out_bufs[ridx], pkt_len);
        *out_len = pkt_len;
        sctp->out_head++;
        return NANORTC_OK;
    }

    /* Second: tell the peer which retransmit-limited TSNs were abandoned. */
    if (sctp->state == NANORTC_SCTP_STATE_ESTABLISHED && sctp->forward_tsn_pending) {
        size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
        pos += nsctp_encode_forward_tsn(nsctp_out_write_buf(sctp) + pos,
                                        sctp->pending_forward_tsn);
        nsctp_queue_output(sctp, pos);
        sctp->forward_tsn_pending = false;
        return nsctp_poll_output(sctp, buf, buf_len, out_len);
    }

    /* Third: encode pending DATA from send queue */
    if (sctp->state == NANORTC_SCTP_STATE_ESTABLISHED) {
        uint8_t idx = sctp->sq_head;
        while (idx != sctp->sq_tail) {
            nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
            if (!e->in_flight && !e->acked && !e->abandoned) {
                /* Build DATA packet directly into output buffer */
                size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
                pos += nsctp_encode_data(nsctp_out_write_buf(sctp) + pos, e->tsn, e->stream_id,
                                         e->ssn, e->ppid, e->flags, sctp->send_buf + e->data_offset,
                                         e->data_len);

                nsctp_queue_output(sctp, pos);
                e->in_flight = true;
#if NANORTC_FEATURE_DC_RELIABLE
                e->sent_at_ms = sctp->last_send_ms;
#endif

                /* Immediately dequeue the packet we just queued */
                uint8_t ridx = (uint8_t)((sctp->out_head) & (NANORTC_SCTP_OUT_QUEUE_SIZE - 1));
                uint16_t pkt_len = sctp->out_lens[ridx];
                memcpy(buf, sctp->out_bufs[ridx], pkt_len);
                *out_len = pkt_len;
                sctp->out_head++;
                return NANORTC_OK;
            }
            idx++;
        }
    }

    *out_len = 0;
    return NANORTC_ERR_NO_DATA;
}

bool nsctp_has_pending_output(const nano_sctp_t *sctp)
{
    if (!sctp) {
        return false;
    }
    if (sctp->out_head != sctp->out_tail) {
        return true;
    }
    if (sctp->forward_tsn_pending) {
        return true;
    }
    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED) {
        return false;
    }
    for (uint8_t idx = sctp->sq_head; idx != sctp->sq_tail; idx++) {
        const nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->in_flight && !e->acked && !e->abandoned) {
            return true;
        }
    }
    return false;
}

/* ---- nsctp_send: enqueue application data ---- */

int nsctp_send_ex(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid, const uint8_t *data,
                  size_t len, bool unordered, bool partial_reliability,
                  uint32_t max_retransmits)
{
    if (!sctp) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED) {
        return NANORTC_ERR_STATE;
    }
    if (sq_full(sctp)) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    if (len > 0 && !data) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Check send buffer space */
    if (sctp->send_buf_used + len > NANORTC_SCTP_SEND_BUF_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Copy payload into send buffer */
    uint16_t offset = sctp->send_buf_used;
    if (len > 0) {
        memcpy(sctp->send_buf + offset, data, len);
        sctp->send_buf_used += (uint16_t)len;
    }

    /* Create send queue entry */
    nsctp_send_entry_t *e = &sctp->send_queue[sctp->sq_tail & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
    memset(e, 0, sizeof(*e));
    e->tsn = sctp->next_tsn++;
    e->stream_id = stream_id;
#if NANORTC_FEATURE_DC_ORDERED
    e->ssn = unordered ? 0 : sctp->next_ssn[stream_id % NANORTC_MAX_DATACHANNELS]++;
    e->flags = SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END;
    if (unordered) {
        e->flags |= SCTP_DATA_FLAG_UNORDERED;
    }
#else
    (void)unordered;
    e->ssn = 0;
    e->flags = SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END | SCTP_DATA_FLAG_UNORDERED;
#endif
    e->ppid = ppid;
    e->data_offset = offset;
    e->data_len = (uint16_t)len;
    e->acked = false;
    e->in_flight = false;
#if NANORTC_FEATURE_DC_RELIABLE
    e->partial_reliability = partial_reliability;
    e->max_retransmits = max_retransmits;
    e->retransmit_count = 0;
#else
    (void)partial_reliability;
    (void)max_retransmits;
#endif

    sctp->sq_tail++;
    NANORTC_LOGD("SCTP", "DATA enqueued");
    return NANORTC_OK;
}

int nsctp_send(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid, const uint8_t *data,
               size_t len)
{
    return nsctp_send_ex(sctp, stream_id, ppid, data, len, false, false, 0);
}

/* ---- Timeout handling ---- */

int nsctp_handle_timeout(nano_sctp_t *sctp, uint32_t now_ms)
{
    if (!sctp) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED) {
        return NANORTC_OK;
    }
    sctp->last_send_ms = now_ms;

#if NANORTC_FEATURE_DC_RELIABLE
    /* Retransmission: check send queue for timed-out entries */
    uint8_t idx = sctp->sq_head;
    while (idx != sctp->sq_tail) {
        nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (e->in_flight && !e->acked && !e->abandoned) {
            uint32_t elapsed = nano_time_elapsed(now_ms, e->sent_at_ms);
            if (elapsed >= sctp->rto_ms) {
                if (e->partial_reliability && sctp->peer_supports_forward_tsn &&
                    e->retransmit_count >= e->max_retransmits) {
                    e->in_flight = false;
                    e->abandoned = true;
                    NANORTC_LOGD("SCTP", "PR-SCTP DATA abandoned");
                    idx++;
                    continue;
                }
                if (e->retransmit_count >= NANORTC_SCTP_MAX_RETRANSMITS) {
                    NANORTC_LOGE("SCTP", "max retransmits exceeded");
                    sctp->state = NANORTC_SCTP_STATE_CLOSED;
                    sctp->closed_due_to_failure = true;
                    return NANORTC_ERR_PROTOCOL;
                }
                /* Mark for retransmission */
                e->in_flight = false;
                e->retransmit_count++;
                e->sent_at_ms = now_ms;

                /* Exponential backoff */
                sctp->rto_ms *= 2;
                if (sctp->rto_ms > NANORTC_SCTP_RTO_MAX_MS) {
                    sctp->rto_ms = NANORTC_SCTP_RTO_MAX_MS;
                }
                NANORTC_LOGD("SCTP", "DATA retransmit scheduled");
            }
        }
        idx++;
    }

    nsctp_advance_send_head(sctp);
#endif /* NANORTC_FEATURE_DC_RELIABLE */

    /* Heartbeat */
    if (!sctp->heartbeat_pending && sctp->crypto) {
        uint32_t hb_elapsed = nano_time_elapsed(now_ms, sctp->last_heartbeat_ms);
        if (hb_elapsed >= NANORTC_SCTP_HEARTBEAT_INTERVAL_MS && !nsctp_out_full(sctp)) {
            uint8_t heartbeat_nonce[NSCTP_NONCE_SIZE];
            if (!sctp->crypto->random_bytes ||
                sctp->crypto->random_bytes(heartbeat_nonce, sizeof(heartbeat_nonce)) != 0) {
                return NANORTC_ERR_CRYPTO;
            }
            size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
            pos += nsctp_encode_heartbeat(nsctp_out_write_buf(sctp) + pos, heartbeat_nonce,
                                          sizeof(heartbeat_nonce));
            nsctp_queue_output(sctp, pos);
            memcpy(sctp->heartbeat_nonce, heartbeat_nonce, sizeof(heartbeat_nonce));
            sctp->heartbeat_pending = true;
            sctp->last_heartbeat_ms = now_ms;
            NANORTC_LOGT("SCTP", "HEARTBEAT sent");
        }
    }

    return NANORTC_OK;
}

uint32_t nsctp_next_timeout_ms(const nano_sctp_t *sctp, uint32_t now_ms)
{
    if (!sctp || sctp->state != NANORTC_SCTP_STATE_ESTABLISHED) {
        return UINT32_MAX;
    }

    uint32_t best = UINT32_MAX;

#if NANORTC_FEATURE_DC_RELIABLE
    /* Earliest retransmit deadline across the in-flight send queue. */
    for (uint8_t idx = sctp->sq_head; idx != sctp->sq_tail; idx++) {
        const nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->in_flight || e->acked || e->abandoned) {
            continue;
        }
        uint32_t elapsed = nano_time_elapsed(now_ms, e->sent_at_ms);
        uint32_t left = (elapsed >= sctp->rto_ms) ? 0u : (sctp->rto_ms - elapsed);
        if (left < best) {
            best = left;
        }
    }
#endif

    /* Heartbeat deadline (skipped while one is already in flight — the
     * timeout block above only re-arms after an ACK clears the pending
     * flag). */
    if (!sctp->heartbeat_pending) {
        uint32_t hb_elapsed = nano_time_elapsed(now_ms, sctp->last_heartbeat_ms);
        uint32_t left = (hb_elapsed >= NANORTC_SCTP_HEARTBEAT_INTERVAL_MS)
                            ? 0u
                            : (NANORTC_SCTP_HEARTBEAT_INTERVAL_MS - hb_elapsed);
        if (left < best) {
            best = left;
        }
    }

    return best;
}
