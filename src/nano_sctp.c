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
        if (ptype == SCTP_PARAM_FORWARD_TSN && plen == 4)
            out->forward_tsn_supported = true;
        if (ptype == SCTP_PARAM_STATE_COOKIE) {
            uint16_t cookie_data_len = plen - 4;
            if (pos + plen <= chunk_len) {
                out->cookie = chunk + pos + 4;
                out->cookie_len = cookie_data_len;
            }
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

    if (clen < 16u || clen > chunk_len)
        return NANORTC_ERR_PARSE;
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

size_t nsctp_encode_init(uint8_t *buf, size_t buf_len, uint8_t type, uint32_t initiate_tag,
                         uint32_t a_rwnd, uint16_t num_ostreams, uint16_t num_istreams,
                         uint32_t initial_tsn, const uint8_t *cookie, uint16_t cookie_len)
{
    size_t need = 24u + ((cookie && cookie_len) ? SCTP_PAD4(4u + (size_t)cookie_len) : 0u);
    if (!buf || (cookie_len && !cookie) || need > buf_len || need > UINT16_MAX)
        return 0;

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

    /* RFC 3758 §3.3: negotiate Forward TSN in both INIT and INIT-ACK. */
    nanortc_write_u16be(buf + pos, SCTP_PARAM_FORWARD_TSN);
    nanortc_write_u16be(buf + pos + 2, 4);
    pos += 4;

    /* Fill chunk length (unpadded) */
    uint16_t chunk_len = (uint16_t)pos;
    nanortc_write_u16be(buf + len_offset, chunk_len);

    return pos;
}

size_t nsctp_encode_cookie_echo(uint8_t *buf, size_t buf_len, const uint8_t *cookie,
                                uint16_t cookie_len)
{
    if (!buf || (cookie_len && !cookie) || cookie_len > UINT16_MAX - 4u ||
        SCTP_PAD4(4u + (size_t)cookie_len) > buf_len)
        return 0;

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

size_t nsctp_encode_data(uint8_t *buf, size_t buf_len, uint32_t tsn, uint16_t stream_id,
                         uint16_t ssn, uint32_t ppid, uint8_t flags, const uint8_t *payload,
                         uint16_t payload_len)
{
    if (!buf || (payload_len && !payload) || payload_len > UINT16_MAX - 16u ||
        SCTP_PAD4(16u + (size_t)payload_len) > buf_len)
        return 0;

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

size_t nsctp_encode_sack_with_gaps(uint8_t *buf, size_t buf_len, uint32_t cumulative_tsn,
                                   uint32_t a_rwnd, const nano_sctp_t *sctp)
{
    if (!buf || !sctp || buf_len < 16u)
        return 0;

    /* Collect gap TSNs and sort them */
    uint32_t gap_tsns[NANORTC_SCTP_MAX_RECV_GAP];
    uint8_t ngaps = 0;
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
        if (sctp->recv_gap[i].valid && (int32_t)(sctp->recv_gap[i].tsn - cumulative_tsn) > 0) {
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

    if (clen > buf_len)
        return 0;
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

size_t nsctp_encode_heartbeat(uint8_t *buf, size_t buf_len, const uint8_t *info, uint16_t info_len)
{
    if (!buf || (info_len && !info) || info_len > UINT16_MAX - 8u ||
        SCTP_PAD4(8u + (size_t)info_len) > buf_len)
        return 0;

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

size_t nsctp_encode_heartbeat_ack(uint8_t *buf, size_t buf_len, const uint8_t *info,
                                  uint16_t info_len)
{
    /* Same format as HEARTBEAT, just different chunk type */
    size_t n = nsctp_encode_heartbeat(buf, buf_len, info, info_len);
    if (n == 0)
        return 0;
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
    if (padded > NANORTC_SCTP_MTU || len < SCTP_HEADER_SIZE) {
        return;
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
    return (uint8_t)(sctp->sq_tail - sctp->sq_head);
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

    if (nsctp_out_full(sctp))
        return NANORTC_ERR_WOULD_BLOCK;

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
    pos += nsctp_encode_init(nsctp_out_write_buf(sctp) + pos, NANORTC_SCTP_MTU - pos,
                             SCTP_CHUNK_INIT, sctp->local_vtag, NANORTC_SCTP_RECV_BUF_SIZE, 0xFFFF,
                             0xFFFF, sctp->next_tsn, NULL, 0);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANORTC_SCTP_STATE_COOKIE_WAIT;
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
    sctp->peer_forward_tsn = init.forward_tsn_supported;
    sctp->peer_num_istreams = init.num_istreams;
    sctp->peer_num_ostreams = init.num_ostreams;

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
    pos += nsctp_encode_init(nsctp_out_write_buf(sctp) + pos, NANORTC_SCTP_MTU - pos,
                             SCTP_CHUNK_INIT_ACK, sctp->local_vtag, NANORTC_SCTP_RECV_BUF_SIZE,
                             0xFFFF, 0xFFFF, sctp->next_tsn, cookie, sizeof(cookie));
    nsctp_queue_output(sctp, pos);

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

    /* Extract and store cookie */
    if (!init.cookie || init.cookie_len == 0 || init.cookie_len > NANORTC_SCTP_COOKIE_SIZE) {
        return NANORTC_ERR_PROTOCOL;
    }

    /* Save peer parameters */
    sctp->remote_vtag = init.initiate_tag;
    sctp->peer_initial_tsn = init.initial_tsn;
    sctp->cumulative_tsn = init.initial_tsn - 1;
    sctp->peer_a_rwnd = init.a_rwnd;
    sctp->peer_forward_tsn = init.forward_tsn_supported;

    memcpy(sctp->cookie, init.cookie, init.cookie_len);
    sctp->cookie_len = init.cookie_len;

    /* Send COOKIE-ECHO */
    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_cookie_echo(nsctp_out_write_buf(sctp) + pos, NANORTC_SCTP_MTU - pos,
                                    sctp->cookie, sctp->cookie_len);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANORTC_SCTP_STATE_COOKIE_ECHOED;
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
        return NANORTC_ERR_PROTOCOL;
    }

    /* Send COOKIE-ACK */
    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_cookie_ack(nsctp_out_write_buf(sctp) + pos);
    nsctp_queue_output(sctp, pos);

    sctp->state = NANORTC_SCTP_STATE_ESTABLISHED;
    return NANORTC_OK;
}

static int nsctp_handle_cookie_ack(nano_sctp_t *sctp)
{
    if (sctp->state != NANORTC_SCTP_STATE_COOKIE_ECHOED) {
        return NANORTC_ERR_STATE;
    }
    sctp->state = NANORTC_SCTP_STATE_ESTABLISHED;
    return NANORTC_OK;
}

/* RFC 9260 §6.5: stream sequence numbers belong to a stream, not a hash bucket. */
static int nsctp_stream(nano_sctp_t *sctp, uint16_t sid)
{
    for (uint8_t i = 0; i < sctp->stream_count; i++)
        if (sctp->stream_ids[i] == sid)
            return i;
    if (sctp->stream_count == NANORTC_MAX_DATACHANNELS)
        return -1;
    uint8_t i = sctp->stream_count++;
    sctp->stream_ids[i] = sid;
    return i;
}

/* Compact only unreferenced bytes. Delivered out-of-order TSNs retain a marker
 * until the cumulative ACK passes them (RFC 9260 §6.2). The bounded table is
 * also the delivery queue: no second queue can lose an acknowledged message. */
static void nsctp_rx_reclaim(nano_sctp_t *sctp)
{
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
        if (!sctp->recv_gap[i].valid || !sctp->recv_gap[i].delivered)
            continue;
        size_t n = sctp->recv_gap[i].data_len;
        uint16_t off = sctp->recv_gap[i].data_offset;
        if (n) {
            memmove(sctp->recv_gap_buf + off, sctp->recv_gap_buf + off + n,
                    sctp->recv_gap_buf_used - off - n);
            sctp->recv_gap_buf_used -= (uint16_t)n;
            sctp->sack_needed = true; /* RFC 9260 §6.2: advertise the reopened receive window. */
            for (uint8_t j = 0; j < NANORTC_SCTP_MAX_RECV_GAP; j++)
                if (sctp->recv_gap[j].valid && sctp->recv_gap[j].data_offset > off)
                    sctp->recv_gap[j].data_offset -= (uint16_t)n;
            sctp->recv_gap[i].data_len = 0;
        }
        if ((int32_t)(sctp->recv_gap[i].tsn - sctp->cumulative_tsn) <= 0) {
            sctp->recv_gap[i].valid = false;
            sctp->recv_gap_count--;
        }
    }
}

static void nsctp_gap_drain(nano_sctp_t *sctp)
{
    for (;;) {
        bool found = false;
        for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
            if (sctp->recv_gap[i].valid && sctp->recv_gap[i].tsn == sctp->cumulative_tsn + 1) {
                sctp->cumulative_tsn++;
                found = true;
                break;
            }
        }
        if (!found)
            break;
    }
    nsctp_rx_reclaim(sctp);
}

/* Rotate a fragment to the assembled prefix without another message-sized
 * buffer. Three reversals take O(pool bytes), bounded by the receive pool. */
static void nsctp_reverse(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n / 2; i++) {
        uint8_t tmp = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = tmp;
    }
}

static void nsctp_rx_move(nano_sctp_t *sctp, uint8_t slot, uint16_t pos)
{
    uint16_t off = sctp->recv_gap[slot].data_offset;
    uint16_t n = sctp->recv_gap[slot].data_len;
    if (off == pos || n == 0)
        return;
    nsctp_reverse(sctp->recv_gap_buf + pos, off - pos);
    nsctp_reverse(sctp->recv_gap_buf + off, n);
    nsctp_reverse(sctp->recv_gap_buf + pos, off - pos + n);
    for (uint8_t j = 0; j < NANORTC_SCTP_MAX_RECV_GAP; j++)
        if (sctp->recv_gap[j].valid && j != slot && sctp->recv_gap[j].data_offset >= pos &&
            sctp->recv_gap[j].data_offset < off)
            sctp->recv_gap[j].data_offset += n;
    sctp->recv_gap[slot].data_offset = pos;
}

/* RFC 9260 §6.9: fragments of one message occupy consecutive TSNs and
 * share stream/SSN/PPID/U; a continuation cannot begin a second message. */
static bool nsctp_fragments_follow(const nano_sctp_rx_entry_t *a, const nano_sctp_rx_entry_t *b)
{
    return b->tsn == a->tsn + 1u && a->stream_id == b->stream_id && a->ssn == b->ssn &&
           a->ppid == b->ppid && !((a->flags ^ b->flags) & SCTP_DATA_FLAG_UNORDERED) &&
           !(a->flags & SCTP_DATA_FLAG_END) && !(b->flags & SCTP_DATA_FLAG_BEGIN);
}

/* Collapse only cumulatively acknowledged fragments: out-of-order TSNs
 * still need individual gap-ACK markers. Each merge frees one descriptor. */
static int nsctp_rx_merge(nano_sctp_t *sctp)
{
    bool merged;
    do {
        merged = false;
        for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP && !merged; i++) {
            nano_sctp_rx_entry_t *a = &sctp->recv_gap[i];
            if (!a->valid || a->delivered)
                continue;
            for (uint8_t j = 0; j < NANORTC_SCTP_MAX_RECV_GAP; j++) {
                nano_sctp_rx_entry_t *b = &sctp->recv_gap[j];
                if (!b->valid || b->delivered || (int32_t)(b->tsn - sctp->cumulative_tsn) > 0 ||
                    !nsctp_fragments_follow(a, b))
                    continue;
                if ((size_t)a->data_len + b->data_len > NANORTC_SCTP_MAX_MESSAGE_SIZE)
                    return NANORTC_ERR_BUFFER_TOO_SMALL;
                nsctp_rx_move(sctp, i, 0);
                nsctp_rx_move(sctp, j, a->data_len);
                a->data_len += b->data_len;
                a->tsn = b->tsn;
                a->flags |= b->flags & SCTP_DATA_FLAG_END;
                a->forwarded |= b->forwarded;
                b->valid = false;
                sctp->recv_gap_count--;
                merged = true;
                break;
            }
        }
    } while (merged);
    return NANORTC_OK;
}

int nsctp_poll_delivery(nano_sctp_t *sctp, nano_sctp_message_t *message)
{
    if (!sctp || !message)
        return NANORTC_ERR_INVALID_PARAM;
    memset(message, 0, sizeof(*message));
    nsctp_rx_reclaim(sctp);
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
        if (!sctp->recv_gap[i].valid || sctp->recv_gap[i].delivered ||
            !(sctp->recv_gap[i].flags & SCTP_DATA_FLAG_BEGIN))
            continue;
        int stream = nsctp_stream(sctp, sctp->recv_gap[i].stream_id);
        bool unordered = (sctp->recv_gap[i].flags & SCTP_DATA_FLAG_UNORDERED) != 0;
        bool forwarded = sctp->recv_gap[i].forwarded;
        if (stream < 0 ||
            (!unordered && !forwarded && sctp->recv_gap[i].ssn != sctp->recv_ssn[stream]))
            continue;
        bool earlier = false;
        for (uint8_t j = 0; j < NANORTC_SCTP_MAX_RECV_GAP; j++)
            if (!unordered && sctp->recv_gap[j].valid && !sctp->recv_gap[j].delivered &&
                sctp->recv_gap[j].stream_id == sctp->recv_gap[i].stream_id &&
                sctp->recv_gap[j].forwarded &&
                (int16_t)(sctp->recv_gap[j].ssn - sctp->recv_gap[i].ssn) < 0)
                earlier = true;
        if (earlier)
            continue;
        uint8_t parts[NANORTC_SCTP_MAX_RECV_GAP];
        uint8_t count = 0;
        size_t total = 0;
        uint32_t tsn = sctp->recv_gap[i].tsn;
        bool complete = false;
        for (uint8_t k = 0; k < NANORTC_SCTP_MAX_RECV_GAP; k++, tsn++) {
            uint8_t j;
            for (j = 0; j < NANORTC_SCTP_MAX_RECV_GAP; j++)
                if (sctp->recv_gap[j].valid && !sctp->recv_gap[j].delivered &&
                    sctp->recv_gap[j].tsn == tsn)
                    break;
            if (j == NANORTC_SCTP_MAX_RECV_GAP)
                break;
            if (k && !nsctp_fragments_follow(&sctp->recv_gap[parts[count - 1]], &sctp->recv_gap[j]))
                return NANORTC_ERR_PROTOCOL;
            if (sctp->recv_gap[j].data_len > NANORTC_SCTP_MAX_MESSAGE_SIZE - total)
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            total += sctp->recv_gap[j].data_len;
            parts[count++] = j;
            if (sctp->recv_gap[j].flags & SCTP_DATA_FLAG_END) {
                complete = true;
                break;
            }
        }
        if (!complete)
            continue;
        size_t pos = 0;
        for (uint8_t k = 0; k < count; k++) {
            uint8_t j = parts[k];
            nsctp_rx_move(sctp, j, (uint16_t)pos);
            pos += sctp->recv_gap[j].data_len;
            sctp->recv_gap[j].delivered = true;
        }
        message->data = sctp->recv_gap_buf;
        message->len = (uint16_t)total;
        message->stream_id = sctp->recv_gap[i].stream_id;
        message->ppid = sctp->recv_gap[i].ppid;
        if (!unordered && !forwarded)
            sctp->recv_ssn[stream]++;
        return NANORTC_OK;
    }
    return NANORTC_ERR_WOULD_BLOCK;
}

static int nsctp_handle_data_chunk(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED)
        return NANORTC_ERR_STATE;
    nsctp_data_t data;
    if (nsctp_parse_data(chunk, clen, &data) != NANORTC_OK)
        return NANORTC_ERR_PARSE;
    if (data.payload_len > NANORTC_SCTP_MAX_MESSAGE_SIZE)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    sctp->sack_needed = true;
    if ((int32_t)(data.tsn - sctp->cumulative_tsn) <= 0)
        return NANORTC_OK;
    nsctp_rx_reclaim(sctp);
    uint8_t slot = NANORTC_SCTP_MAX_RECV_GAP;
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
        if (sctp->recv_gap[i].valid && sctp->recv_gap[i].tsn == data.tsn)
            return NANORTC_OK;
        if (!sctp->recv_gap[i].valid)
            slot = i;
    }
    nano_sctp_rx_entry_t incoming = {
        .tsn = data.tsn,
        .ppid = data.ppid,
        .stream_id = data.stream_id,
        .ssn = data.ssn,
        .data_len = data.payload_len,
        .valid = true,
        .flags =
            data.flags & (SCTP_DATA_FLAG_BEGIN | SCTP_DATA_FLAG_END | SCTP_DATA_FLAG_UNORDERED),
    };
    /* Reject permanent fragmentation errors before receive-window backpressure
     * can hide them (RFC 9260 §6.9, RFC 8841 §6). */
    if (!(incoming.flags & SCTP_DATA_FLAG_BEGIN)) {
        for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++) {
            const nano_sctp_rx_entry_t *previous = &sctp->recv_gap[i];
            if (!previous->valid || previous->delivered || (previous->flags & SCTP_DATA_FLAG_END) ||
                previous->tsn + 1u != incoming.tsn)
                continue;
            if (!nsctp_fragments_follow(previous, &incoming))
                return NANORTC_ERR_PROTOCOL;
            if (incoming.data_len > NANORTC_SCTP_MAX_MESSAGE_SIZE - previous->data_len)
                return NANORTC_ERR_BUFFER_TOO_SMALL;
        }
    }
    /* Leave room for the missing leading packet, so out-of-order data cannot
     * permanently fill the receive window (RFC 9260 §6.2). */
    bool gap = data.tsn != sctp->cumulative_tsn + 1;
    size_t reserve = gap ? (NANORTC_SCTP_MTU - 28u) : 0u;
    size_t free_bytes = NANORTC_SCTP_RECV_GAP_BUF_SIZE - sctp->recv_gap_buf_used;
    if (slot == NANORTC_SCTP_MAX_RECV_GAP ||
        (gap && sctp->recv_gap_count >= NANORTC_SCTP_MAX_RECV_GAP - 1) || free_bytes < reserve ||
        data.payload_len > free_bytes - reserve)
        return NANORTC_ERR_WOULD_BLOCK; /* not acknowledged; peer can retransmit */
    if (nsctp_stream(sctp, data.stream_id) < 0)
        return NANORTC_ERR_PROTOCOL;
    uint16_t off = sctp->recv_gap_buf_used;
    if (data.payload_len)
        memcpy(sctp->recv_gap_buf + off, data.payload, data.payload_len);
    sctp->recv_gap_buf_used += data.payload_len;
    incoming.data_offset = off;
    sctp->recv_gap[slot] = incoming;
    sctp->recv_gap_count++;
    nsctp_gap_drain(sctp);
    return nsctp_rx_merge(sctp);
}

#if NANORTC_FEATURE_DC_RELIABLE
static bool nsctp_gap_acked(const uint8_t *chunk, uint16_t count, int32_t offset)
{
    if (offset <= 0 || offset > UINT16_MAX)
        return false;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *gap = chunk + SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE + 4u * i;
        if (offset >= nanortc_read_u16be(gap) && offset <= nanortc_read_u16be(gap + 2))
            return true;
    }
    return false;
}

static void nsctp_abandon_message(nano_sctp_t *sctp, uint8_t index)
{
    /* RFC 3758 §3.5 A3: abandonment always covers the whole user message. */
    uint8_t first = index;
    while (
        first != sctp->sq_head &&
        !(sctp->send_queue[first & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)].flags & SCTP_DATA_FLAG_BEGIN))
        first--;
    for (uint8_t j = first; j != sctp->sq_tail; j++) {
        nsctp_send_entry_t *part = &sctp->send_queue[j & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        part->abandoned = true;
        if (part->flags & SCTP_DATA_FLAG_END)
            break;
    }
    sctp->forward_pending = true;
}
#endif

static int nsctp_handle_sack_chunk(nano_sctp_t *sctp, const uint8_t *chunk, size_t clen)
{
    nsctp_sack_t sack;
    if (nsctp_parse_sack(chunk, clen, &sack) != NANORTC_OK) {
        return NANORTC_ERR_PARSE;
    }

    /* RFC 9260 §3.3.4: validate gap/duplicate lists before reading or acting. */
    size_t blocks = (clen - SCTP_CHUNK_HDR_SIZE - SCTP_SACK_MIN_SIZE) / 4u;
    if (sack.num_gap_blocks > blocks || sack.num_dup_tsns > blocks - sack.num_gap_blocks)
        return NANORTC_ERR_PARSE;
    uint16_t previous_end = 0;
    for (uint16_t i = 0; i < sack.num_gap_blocks; i++) {
        const uint8_t *gap = chunk + SCTP_CHUNK_HDR_SIZE + SCTP_SACK_MIN_SIZE + 4u * i;
        uint16_t start = nanortc_read_u16be(gap), end = nanortc_read_u16be(gap + 2);
        if (start <= previous_end || end < start)
            return NANORTC_ERR_PARSE;
        previous_end = end;
    }
#if NANORTC_FEATURE_DC_RELIABLE
    /* RFC 3758 §3.1 permits application-specific abandonment policies. For
     * zero retries, a later gap-ACKed in-flight TSN triggers early abandonment.
     * Reordering can also cause a gap: this is a policy, not proof of loss.
     * Gap ACKs do not free storage because the receiver may renege. */
    uint32_t highest = sack.cumulative_tsn;
    for (uint8_t i = sctp->sq_head; i != sctp->sq_tail; i++) {
        const nsctp_send_entry_t *e = &sctp->send_queue[i & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        int32_t offset = (int32_t)(e->tsn - sack.cumulative_tsn);
        if (e->in_flight && (int32_t)(e->tsn - highest) > 0 &&
            nsctp_gap_acked(chunk, sack.num_gap_blocks, offset))
            highest = e->tsn;
    }
    for (uint8_t i = sctp->sq_head; i != sctp->sq_tail; i++) {
        nsctp_send_entry_t *e = &sctp->send_queue[i & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (e->in_flight && !e->acked && !e->abandoned && e->max_retransmits == 0 &&
            (int32_t)(e->tsn - sack.cumulative_tsn) > 0 && (int32_t)(highest - e->tsn) > 0 &&
            !nsctp_gap_acked(chunk, sack.num_gap_blocks, (int32_t)(e->tsn - sack.cumulative_tsn)))
            nsctp_abandon_message(sctp, i);
    }
#endif

    /* Mark acked entries in send queue */
    uint8_t idx = sctp->sq_head;
    while (idx != sctp->sq_tail) {
        nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        /* TSN comparison: tsn <= cumulative_tsn_ack (handling wrap) */
        int32_t diff = (int32_t)(e->tsn - sack.cumulative_tsn);
        if (diff <= 0 && !e->acked) {
            e->acked = true;
        }
        idx++;
    }

    /* Advance sq_head past acked entries to free space */
    while (sctp->sq_head != sctp->sq_tail) {
        nsctp_send_entry_t *e =
            &sctp->send_queue[sctp->sq_head & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->acked)
            break;
        sctp->sq_head++;
    }

    /* If send queue fully drained, reclaim send_buf */
    if (sctp->sq_head == sctp->sq_tail) {
        sctp->send_buf_used = 0;
    }

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

    if (nsctp_out_full(sctp))
        return NANORTC_ERR_WOULD_BLOCK;
    if (SCTP_PAD4(8u + (size_t)info_len) > NANORTC_SCTP_MTU - SCTP_HEADER_SIZE)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
    pos += nsctp_encode_heartbeat_ack(nsctp_out_write_buf(sctp) + pos, NANORTC_SCTP_MTU - pos, info,
                                      info_len);
    nsctp_queue_output(sctp, pos);

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
    if (clen < 8 || (clen - 8) % 4)
        return NANORTC_ERR_PARSE;
    if (!sctp->peer_forward_tsn)
        return NANORTC_ERR_PROTOCOL;
    uint32_t new_tsn = nanortc_read_u32be(chunk + 4);
    sctp->sack_needed = true;
    if ((int32_t)(new_tsn - sctp->cumulative_tsn) <= 0)
        return NANORTC_OK;
    sctp->cumulative_tsn = new_tsn;
    /* RFC 3758 §3.6: preserve complete messages, discard stranded fragments,
     * then release skipped stream sequence numbers and advance across gaps. */
    int rc = nsctp_rx_merge(sctp);
    if (rc != NANORTC_OK)
        return rc;
    for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++)
        if (sctp->recv_gap[i].valid && (int32_t)(sctp->recv_gap[i].tsn - new_tsn) <= 0 &&
            (sctp->recv_gap[i].flags & 3u) != 3u)
            sctp->recv_gap[i].delivered = true;
    for (size_t pos = 8; pos < clen; pos += 4) {
        uint16_t sid = nanortc_read_u16be(chunk + pos);
        uint16_t ssn = nanortc_read_u16be(chunk + pos + 2);
        int stream = nsctp_stream(sctp, sid);
        if (stream < 0)
            return NANORTC_ERR_PROTOCOL;
        if ((int16_t)(ssn - sctp->recv_ssn[stream]) < 0)
            continue;
        for (uint8_t i = 0; i < NANORTC_SCTP_MAX_RECV_GAP; i++)
            if (sctp->recv_gap[i].valid && sctp->recv_gap[i].stream_id == sid &&
                !(sctp->recv_gap[i].flags & SCTP_DATA_FLAG_UNORDERED) &&
                (int16_t)(sctp->recv_gap[i].ssn - ssn) <= 0)
                sctp->recv_gap[i].forwarded = true;
        sctp->recv_ssn[stream] = ssn + 1u;
    }
    nsctp_gap_drain(sctp);
    return NANORTC_OK;
}

static int nsctp_queue_sack(nano_sctp_t *sctp, size_t cap)
{
    if (nsctp_out_full(sctp))
        return NANORTC_ERR_WOULD_BLOCK;
    uint8_t chunk[NSCTP_MAX_SACK_SIZE];
    size_t n =
        nsctp_encode_sack_with_gaps(chunk, sizeof(chunk), sctp->cumulative_tsn,
                                    NANORTC_SCTP_RECV_GAP_BUF_SIZE - sctp->recv_gap_buf_used, sctp);
    if (!n || cap < SCTP_HEADER_SIZE + n)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    nsctp_begin_packet(sctp, sctp->remote_vtag);
    memcpy(nsctp_out_write_buf(sctp) + SCTP_HEADER_SIZE, chunk, n);
    nsctp_queue_output(sctp, SCTP_HEADER_SIZE + n);
    sctp->sack_needed = false;
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
        return rc;
    }

    /* Parse common header */
    nsctp_header_t hdr;
    nsctp_parse_header(data, len, &hdr);

    /* Iterate chunks */
    size_t pos = SCTP_HEADER_SIZE;
    while (pos + SCTP_CHUNK_HDR_SIZE <= len) {
        uint8_t ctype = data[pos];
        uint16_t clen = nanortc_read_u16be(data + pos + 2);

        if (clen < SCTP_CHUNK_HDR_SIZE || pos + clen > len) {
            return NANORTC_ERR_PARSE;
        }

        if ((ctype == SCTP_CHUNK_INIT || ctype == SCTP_CHUNK_INIT_ACK ||
             ctype == SCTP_CHUNK_COOKIE_ECHO || ctype == SCTP_CHUNK_HEARTBEAT) &&
            nsctp_out_full(sctp))
            return NANORTC_ERR_WOULD_BLOCK;
        rc = NANORTC_OK;
        switch (ctype) {
        case SCTP_CHUNK_INIT:
            rc = nsctp_handle_init(sctp, data + pos, clen, &hdr);
            if (rc != NANORTC_OK) {
                return rc;
            }
            break;

        case SCTP_CHUNK_INIT_ACK:
            rc = nsctp_handle_init_ack(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_COOKIE_ECHO:
            rc = nsctp_handle_cookie_echo(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_COOKIE_ACK:
            rc = nsctp_handle_cookie_ack(sctp);
            break;

        case SCTP_CHUNK_DATA:
            rc = nsctp_handle_data_chunk(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_SACK:
            rc = nsctp_handle_sack_chunk(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_HEARTBEAT:
            rc = nsctp_handle_heartbeat(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_HEARTBEAT_ACK:
            rc = nsctp_handle_heartbeat_ack(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_FORWARD_TSN:
            rc = nsctp_handle_forward_tsn(sctp, data + pos, clen);
            break;

        case SCTP_CHUNK_ABORT:
            sctp->state = NANORTC_SCTP_STATE_CLOSED;
            break;

        case SCTP_CHUNK_SHUTDOWN:
            break;

        default:
            break;
        }

        if (rc != NANORTC_OK && !(ctype == SCTP_CHUNK_DATA && rc == NANORTC_ERR_WOULD_BLOCK))
            return rc;
        pos += SCTP_PAD4(clen);
    }

    if (sctp->sack_needed && !nsctp_out_full(sctp)) {
        int sack_rc = nsctp_queue_sack(sctp, NANORTC_SCTP_MTU);
        if (sack_rc != NANORTC_OK)
            return sack_rc;
    }
    return NANORTC_OK;
}

#if NANORTC_FEATURE_DC_RELIABLE
static int nsctp_forward_output(nano_sctp_t *sctp, uint8_t *buf, size_t cap, size_t *out_len)
{
    uint16_t ids[NANORTC_MAX_DATACHANNELS], ssns[NANORTC_MAX_DATACHANNELS];
    uint8_t streams = 0;
    uint32_t advanced = 0;
    bool skipped = false;
    for (uint8_t i = sctp->sq_head; i != sctp->sq_tail; i++) {
        nsctp_send_entry_t *e = &sctp->send_queue[i & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->acked && !e->abandoned)
            break;
        advanced = e->tsn;
        if (!e->abandoned)
            continue;
        skipped = true;
        if (e->flags & SCTP_DATA_FLAG_UNORDERED)
            continue;
        uint8_t j;
        for (j = 0; j < streams && ids[j] != e->stream_id; j++) {
        }
        if (j == streams)
            ids[streams++] = e->stream_id;
        ssns[j] = e->ssn;
    }
    if (!skipped) {
        sctp->forward_pending = false;
        return NANORTC_ERR_NO_DATA;
    }
    size_t len = 20u + 4u * streams;
    if (cap < len)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    nsctp_encode_header(buf, sctp->local_port, sctp->remote_port, sctp->remote_vtag);
    nsctp_encode_forward_tsn(buf + SCTP_HEADER_SIZE, advanced);
    nanortc_write_u16be(buf + SCTP_HEADER_SIZE + 2, (uint16_t)(len - SCTP_HEADER_SIZE));
    for (uint8_t j = 0; j < streams; j++) {
        nanortc_write_u16be(buf + 20 + 4u * j, ids[j]);
        nanortc_write_u16be(buf + 22 + 4u * j, ssns[j]);
    }
    nsctp_finalize_checksum(buf, len);
    *out_len = len;
    sctp->forward_pending = false;
    sctp->forward_sent_at_ms = sctp->now_ms;
    return NANORTC_OK;
}
#endif

/* ---- Poll output ---- */

int nsctp_poll_output(nano_sctp_t *sctp, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!sctp || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    if (sctp->sack_needed && sctp->out_head == sctp->out_tail) {
        int rc = nsctp_queue_sack(sctp, buf_len);
        if (rc != NANORTC_OK)
            return rc;
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

#if NANORTC_FEATURE_DC_RELIABLE
    if (sctp->forward_pending) {
        int rc = nsctp_forward_output(sctp, buf, buf_len, out_len);
        if (rc != NANORTC_ERR_NO_DATA)
            return rc;
    }
#endif

    /* Second: encode pending DATA from send queue */
    if (sctp->state == NANORTC_SCTP_STATE_ESTABLISHED) {
        uint8_t idx = sctp->sq_head;
        while (idx != sctp->sq_tail) {
            nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
            if (!e->in_flight && !e->acked
#if NANORTC_FEATURE_DC_RELIABLE
                && !e->abandoned
#endif
            ) {
                if (buf_len < SCTP_HEADER_SIZE + SCTP_PAD4(16u + (size_t)e->data_len)) {
                    return NANORTC_ERR_BUFFER_TOO_SMALL;
                }
                /* Build DATA packet directly into output buffer */
                size_t pos = nsctp_begin_packet(sctp, sctp->remote_vtag);
                pos += nsctp_encode_data(nsctp_out_write_buf(sctp) + pos, NANORTC_SCTP_MTU - pos,
                                         e->tsn, e->stream_id, e->ssn, e->ppid, e->flags,
                                         sctp->send_buf + e->data_offset, e->data_len);

                nsctp_queue_output(sctp, pos);
                e->in_flight = true;
#if NANORTC_FEATURE_DC_RELIABLE
                e->sent_at_ms = sctp->now_ms;
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
#if NANORTC_FEATURE_DC_RELIABLE
    if (sctp->forward_pending)
        return true;
#endif
    if (sctp->sack_needed || sctp->out_head != sctp->out_tail) {
        return true;
    }
    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED) {
        return false;
    }
    for (uint8_t idx = sctp->sq_head; idx != sctp->sq_tail; idx++) {
        const nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (!e->in_flight && !e->acked
#if NANORTC_FEATURE_DC_RELIABLE
            && !e->abandoned
#endif
        ) {
            return true;
        }
    }
    return false;
}

/* ---- nsctp_send: enqueue application data ---- */

int nsctp_send_options(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid, const uint8_t *data,
                       size_t len, bool unordered, int32_t max_retransmits)
{
    if (!sctp || (len && !data))
        return NANORTC_ERR_INVALID_PARAM;
    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED)
        return NANORTC_ERR_STATE;
    if (max_retransmits < -1 || max_retransmits > UINT16_MAX)
        return NANORTC_ERR_INVALID_PARAM;
    if (max_retransmits >= 0 && (!sctp->peer_forward_tsn || !NANORTC_FEATURE_DC_RELIABLE))
        return NANORTC_ERR_NOT_IMPLEMENTED;
    if (len > NANORTC_SCTP_MAX_MESSAGE_SIZE || len > NANORTC_SCTP_SEND_BUF_SIZE ||
        (sctp->peer_max_message_size && len > sctp->peer_max_message_size))
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    size_t mtu_payload = (NANORTC_SCTP_MTU & ~3u) - 28u;
    size_t count = len ? (len + mtu_payload - 1) / mtu_payload : 1;
    if (count > NANORTC_SCTP_MAX_SEND_QUEUE)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    if (count > NANORTC_SCTP_MAX_SEND_QUEUE - (size_t)sq_count(sctp) ||
        len > NANORTC_SCTP_SEND_BUF_SIZE - (size_t)sctp->send_buf_used)
        return NANORTC_ERR_WOULD_BLOCK;
    int stream = nsctp_stream(sctp, stream_id);
    if (stream < 0)
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    uint16_t off = sctp->send_buf_used;
    if (len)
        memcpy(sctp->send_buf + off, data, len);
    sctp->send_buf_used += (uint16_t)len;
    uint16_t ssn = unordered ? 0 : sctp->next_ssn[stream]++;
    size_t remaining = len;
    for (size_t i = 0; i < count; i++) {
        nsctp_send_entry_t *e =
            &sctp->send_queue[sctp->sq_tail & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        memset(e, 0, sizeof(*e));
        size_t n = remaining > mtu_payload ? mtu_payload : remaining;
        e->tsn = sctp->next_tsn++;
        e->stream_id = stream_id;
        e->ssn = ssn;
        e->ppid = ppid;
        e->flags = (i == 0 ? SCTP_DATA_FLAG_BEGIN : 0) | (i + 1 == count ? SCTP_DATA_FLAG_END : 0) |
                   (unordered ? SCTP_DATA_FLAG_UNORDERED : 0);
        e->data_offset = off;
        e->data_len = (uint16_t)n;
#if NANORTC_FEATURE_DC_RELIABLE
        e->max_retransmits = max_retransmits;
#else
        (void)max_retransmits;
#endif
        off += (uint16_t)n;
        remaining -= n;
        sctp->sq_tail++;
    }
    return NANORTC_OK;
}

int nsctp_send(nano_sctp_t *sctp, uint16_t stream_id, uint32_t ppid, const uint8_t *data,
               size_t len)
{
    return nsctp_send_options(sctp, stream_id, ppid, data, len, !NANORTC_FEATURE_DC_ORDERED, -1);
}

/* ---- Timeout handling ---- */

int nsctp_handle_timeout(nano_sctp_t *sctp, uint32_t now_ms)
{
    if (!sctp) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    sctp->now_ms = now_ms;
    if (sctp->state != NANORTC_SCTP_STATE_ESTABLISHED) {
        return NANORTC_OK;
    }

#if NANORTC_FEATURE_DC_RELIABLE
    /* Retransmission: check send queue for timed-out entries */
    uint8_t idx = sctp->sq_head;
    while (idx != sctp->sq_tail) {
        nsctp_send_entry_t *e = &sctp->send_queue[idx & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
        if (e->in_flight && !e->acked && !e->abandoned) {
            uint32_t elapsed = nano_time_elapsed(now_ms, e->sent_at_ms);
            if (elapsed >= sctp->rto_ms) {
                if (e->max_retransmits >= 0 && e->retransmit_count >= e->max_retransmits) {
                    nsctp_abandon_message(sctp, idx);
                    idx++;
                    continue;
                }
                if (e->max_retransmits < 0 && e->retransmit_count >= NANORTC_SCTP_MAX_RETRANSMITS) {
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
            }
        }
        idx++;
    }
#endif /* NANORTC_FEATURE_DC_RELIABLE */

    /* RFC 3758 §3.5 C5/A5: repeat FORWARD TSN until cumulative SACK acknowledges it. */
#if NANORTC_FEATURE_DC_RELIABLE
    if (nano_time_elapsed(now_ms, sctp->forward_sent_at_ms) >= sctp->rto_ms) {
        for (uint8_t i = sctp->sq_head; i != sctp->sq_tail; i++) {
            nsctp_send_entry_t *e = &sctp->send_queue[i & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)];
            if (!e->acked && !e->abandoned)
                break;
            if (e->abandoned)
                sctp->forward_pending = true;
        }
    }
#endif

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
            pos += nsctp_encode_heartbeat(nsctp_out_write_buf(sctp) + pos, NANORTC_SCTP_MTU - pos,
                                          heartbeat_nonce, sizeof(heartbeat_nonce));
            nsctp_queue_output(sctp, pos);
            memcpy(sctp->heartbeat_nonce, heartbeat_nonce, sizeof(heartbeat_nonce));
            sctp->heartbeat_pending = true;
            sctp->last_heartbeat_ms = now_ms;
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
        if (e->abandoned && !e->acked) {
            if (!sctp->send_queue[sctp->sq_head & (NANORTC_SCTP_MAX_SEND_QUEUE - 1)].abandoned)
                continue;
            uint32_t elapsed = nano_time_elapsed(now_ms, sctp->forward_sent_at_ms);
            uint32_t left = elapsed >= sctp->rto_ms ? 0 : sctp->rto_ms - elapsed;
            if (left < best)
                best = left;
            continue;
        }
        if (!e->in_flight || e->acked) {
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
