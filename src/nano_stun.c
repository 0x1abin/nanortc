/*
 * nanortc — STUN message codec (RFC 8489)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_stun.h"
#include "nano_crc32.h"
#include "nanortc.h"
#include <string.h>

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/* Read big-endian uint16 from wire */
static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Read big-endian uint32 from wire */
static uint32_t read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Read big-endian uint64 from wire */
static uint64_t read_u64(const uint8_t *p)
{
    return ((uint64_t)read_u32(p) << 32) | read_u32(p + 4);
}

/* Write big-endian uint16 to wire */
static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

/* Write big-endian uint32 to wire */
static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* Write big-endian uint64 to wire */
static void write_u64(uint8_t *p, uint64_t v)
{
    write_u32(p, (uint32_t)(v >> 32));
    write_u32(p + 4, (uint32_t)(v));
}

/* Pad attribute length to 4-byte boundary */
static size_t attr_padded(uint16_t len)
{
    return (len + 3u) & ~3u;
}

/* ----------------------------------------------------------------
 * stun_parse — RFC 8489 §5
 * ---------------------------------------------------------------- */

int stun_parse(const uint8_t *data, size_t len, stun_msg_t *msg)
{
    if (!data || !msg) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (len < STUN_HEADER_SIZE) {
        return NANO_ERR_PARSE;
    }

    memset(msg, 0, sizeof(*msg));

    /* RFC 8489 §6: top 2 bits must be 0 */
    if (data[0] & 0xC0) {
        return NANO_ERR_PARSE;
    }

    msg->type = read_u16(data);
    msg->length = read_u16(data + 2);

    /* Magic cookie (RFC 8489 §6) */
    uint32_t cookie = read_u32(data + 4);
    if (cookie != STUN_MAGIC_COOKIE) {
        return NANO_ERR_PARSE;
    }

    /* Length must match packet, must be 4-byte aligned */
    if ((size_t)(msg->length + STUN_HEADER_SIZE) != len) {
        return NANO_ERR_PARSE;
    }
    if (msg->length & 3) {
        return NANO_ERR_PARSE;
    }

    memcpy(msg->transaction_id, data + 8, STUN_TXID_SIZE);

    /*
     * Walk attributes (RFC 8489 §15).
     *
     * Ordering rules (RFC 8489 §14.5, §14.7):
     * - After MESSAGE-INTEGRITY: ignore all attributes except FINGERPRINT
     * - After FINGERPRINT: no more attributes allowed (must be last)
     */
    size_t pos = STUN_HEADER_SIZE;
    bool after_integrity = false;
    bool after_fingerprint = false;

    while (pos + 4 <= len) {
        uint16_t attr_type = read_u16(data + pos);
        uint16_t attr_len = read_u16(data + pos + 2);
        size_t padded = attr_padded(attr_len);
        const uint8_t *val = data + pos + 4;

        if (pos + 4 + padded > len) {
            return NANO_ERR_PARSE;
        }

        /* RFC 8489 §14.7: nothing may follow FINGERPRINT */
        if (after_fingerprint) {
            return NANO_ERR_PARSE;
        }

        /*
         * RFC 8489 §14.5: after MESSAGE-INTEGRITY, ignore all attributes
         * except FINGERPRINT (and MESSAGE-INTEGRITY-SHA256, which we don't
         * support but would be type 0x001C).
         */
        if (after_integrity && attr_type != STUN_ATTR_FINGERPRINT) {
            pos += 4 + padded;
            continue;
        }

        switch (attr_type) {
        case STUN_ATTR_USERNAME:
            msg->username = (const char *)val;
            msg->username_len = attr_len;
            break;

        case STUN_ATTR_PRIORITY:
            if (attr_len != 4) {
                return NANO_ERR_PARSE;
            }
            msg->priority = read_u32(val);
            break;

        case STUN_ATTR_USE_CANDIDATE:
            msg->use_candidate = true;
            break;

        case STUN_ATTR_XOR_MAPPED_ADDRESS:
            /* RFC 8489 §14.2 */
            if (attr_len < 8) {
                return NANO_ERR_PARSE;
            }
            msg->mapped_family = val[1];
            if (msg->mapped_family == STUN_FAMILY_IPV4) {
                /* Port XOR top 16 bits of magic cookie */
                msg->mapped_port = read_u16(val + 2) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
                /* Address XOR magic cookie (network order) */
                uint32_t xaddr = read_u32(val + 4) ^ STUN_MAGIC_COOKIE;
                write_u32(msg->mapped_addr, xaddr);
            } else if (msg->mapped_family == STUN_FAMILY_IPV6) {
                if (attr_len < 20) {
                    return NANO_ERR_PARSE;
                }
                msg->mapped_port = read_u16(val + 2) ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16);
                /* Address XOR (magic_cookie || transaction_id) = 16 bytes */
                uint8_t mask[16];
                write_u32(mask, STUN_MAGIC_COOKIE);
                memcpy(mask + 4, msg->transaction_id, STUN_TXID_SIZE);
                for (int i = 0; i < 16; i++) {
                    msg->mapped_addr[i] = val[4 + i] ^ mask[i];
                }
            } else {
                return NANO_ERR_PARSE;
            }
            break;

        case STUN_ATTR_MESSAGE_INTEGRITY:
            if (attr_len != 20) {
                return NANO_ERR_PARSE;
            }
            msg->has_integrity = true;
            msg->integrity_offset = (uint16_t)pos;
            after_integrity = true;
            break;

        case STUN_ATTR_FINGERPRINT:
            if (attr_len != 4) {
                return NANO_ERR_PARSE;
            }
            msg->has_fingerprint = true;
            msg->fingerprint_value = read_u32(val);
            after_fingerprint = true;
            break;

        case STUN_ATTR_ERROR_CODE:
            /* RFC 8489 §14.8: class(3 bits) * 100 + number(8 bits) */
            if (attr_len < 4) {
                return NANO_ERR_PARSE;
            }
            msg->error_code = (uint16_t)((val[2] & 0x07) * 100 + val[3]);
            break;

        case STUN_ATTR_ICE_CONTROLLING:
            if (attr_len != 8) {
                return NANO_ERR_PARSE;
            }
            msg->has_ice_controlling = true;
            msg->ice_controlling = read_u64(val);
            break;

        case STUN_ATTR_ICE_CONTROLLED:
            if (attr_len != 8) {
                return NANO_ERR_PARSE;
            }
            msg->has_ice_controlled = true;
            msg->ice_controlled = read_u64(val);
            break;

        default:
            /* Skip unknown/unhandled attributes (RFC 8489 §15.1) */
            break;
        }

        pos += 4 + padded;
    }

    return NANO_OK;
}

/* ----------------------------------------------------------------
 * stun_verify_fingerprint — RFC 8489 §14.7
 * ---------------------------------------------------------------- */

int stun_verify_fingerprint(const uint8_t *data, size_t len)
{
    if (!data || len < STUN_HEADER_SIZE + 8) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* FINGERPRINT must be last attribute: 4-byte TLV header + 4-byte value */
    size_t fp_offset = len - 8;
    uint16_t fp_type = read_u16(data + fp_offset);
    uint16_t fp_len = read_u16(data + fp_offset + 2);

    if (fp_type != STUN_ATTR_FINGERPRINT || fp_len != 4) {
        return NANO_ERR_PROTOCOL;
    }

    uint32_t fp_value = read_u32(data + fp_offset + 4);

    /*
     * RFC 8489 §14.7: CRC-32 is computed over the STUN message up to
     * (but excluding) the FINGERPRINT attribute, with the header length
     * field adjusted to include the FINGERPRINT attribute.
     *
     * Stack copy is safe: STUN messages are always < 548 bytes (RFC 8489 §7.1).
     */
    if (fp_offset > 548) {
        return NANO_ERR_PROTOCOL;
    }

    uint8_t msg_copy[548];
    memcpy(msg_copy, data, fp_offset);
    /* Patch header length to include FP attr */
    write_u16(msg_copy + 2, (uint16_t)(len - STUN_HEADER_SIZE));

    uint32_t expected = nano_crc32(msg_copy, fp_offset) ^ STUN_FINGERPRINT_XOR;

    if (expected != fp_value) {
        return NANO_ERR_PROTOCOL;
    }

    return NANO_OK;
}

/* ----------------------------------------------------------------
 * stun_verify_integrity — RFC 8489 §14.5
 * ---------------------------------------------------------------- */

int stun_verify_integrity(const uint8_t *data, size_t len, const stun_msg_t *msg,
                          const uint8_t *key, size_t key_len, stun_hmac_sha1_fn hmac_sha1)
{
    if (!data || !msg || !key || !hmac_sha1) {
        return NANO_ERR_INVALID_PARAM;
    }
    if (!msg->has_integrity) {
        return NANO_ERR_PROTOCOL;
    }
    if ((size_t)msg->integrity_offset + 24 > len) {
        return NANO_ERR_PARSE;
    }

    /*
     * RFC 8489 §14.5: HMAC-SHA1 is computed over the STUN message up to
     * (but excluding) the MESSAGE-INTEGRITY value, with the header length
     * field adjusted to include the MESSAGE-INTEGRITY attribute (24 bytes:
     * 4-byte TLV header + 20-byte HMAC value).
     *
     * Adjusted length = (integrity_offset - STUN_HEADER_SIZE) + 24
     */
    uint16_t adjusted_len = (uint16_t)(msg->integrity_offset - STUN_HEADER_SIZE + 24);

    /* Build HMAC input: modified header + data[4..integrity_offset) */
    if (msg->integrity_offset > 548) {
        return NANO_ERR_PROTOCOL;
    }

    uint8_t hmac_input[548];
    memcpy(hmac_input, data, msg->integrity_offset);
    /* Patch the length field */
    write_u16(hmac_input + 2, adjusted_len);

    uint8_t computed[20];
    hmac_sha1(key, key_len, hmac_input, msg->integrity_offset, computed);

    /* Compare with stored HMAC value at data[integrity_offset + 4 .. + 24] */
    if (memcmp(computed, data + msg->integrity_offset + 4, 20) != 0) {
        return NANO_ERR_PROTOCOL;
    }

    return NANO_OK;
}

/* ----------------------------------------------------------------
 * Encode helpers
 * ---------------------------------------------------------------- */

/* Write attribute TLV header, return pointer to value start */
static uint8_t *write_attr_hdr(uint8_t *buf, uint16_t type, uint16_t length)
{
    write_u16(buf, type);
    write_u16(buf + 2, length);
    return buf + 4;
}

/* Write STUN header (20 bytes) */
static void write_stun_header(uint8_t *buf, uint16_t type, uint16_t length,
                              const uint8_t txid[STUN_TXID_SIZE])
{
    write_u16(buf, type);
    write_u16(buf + 2, length);
    write_u32(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, STUN_TXID_SIZE);
}

/*
 * Append MESSAGE-INTEGRITY and FINGERPRINT attributes.
 * `pos` is the current write position (offset from buf start).
 * Returns the final message length, or negative on error.
 */
static int append_integrity_fingerprint(uint8_t *buf, size_t buf_len, size_t pos,
                                        const uint8_t *key, size_t key_len,
                                        stun_hmac_sha1_fn hmac_sha1)
{
    /* MESSAGE-INTEGRITY: 4 (TLV) + 20 (HMAC) = 24 bytes */
    /* FINGERPRINT: 4 (TLV) + 4 (CRC) = 8 bytes */
    if (pos + 24 + 8 > buf_len) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }

    /* --- MESSAGE-INTEGRITY (RFC 8489 §14.5) --- */
    /* Set header length to include MI attr */
    write_u16(buf + 2, (uint16_t)(pos - STUN_HEADER_SIZE + 24));

    uint8_t *mi_val = write_attr_hdr(buf + pos, STUN_ATTR_MESSAGE_INTEGRITY, 20);
    uint8_t hmac[20];
    hmac_sha1(key, key_len, buf, pos, hmac);
    memcpy(mi_val, hmac, 20);
    pos += 24;

    /* --- FINGERPRINT (RFC 8489 §14.7) --- */
    /* Set header length to include MI + FP */
    write_u16(buf + 2, (uint16_t)(pos - STUN_HEADER_SIZE + 8));

    uint8_t *fp_val = write_attr_hdr(buf + pos, STUN_ATTR_FINGERPRINT, 4);
    uint32_t crc = nano_crc32(buf, pos) ^ STUN_FINGERPRINT_XOR;
    write_u32(fp_val, crc);
    pos += 8;

    return (int)pos;
}

/* ----------------------------------------------------------------
 * stun_encode_binding_response — RFC 8489 §7.3.1
 * ---------------------------------------------------------------- */

int stun_encode_binding_response(const stun_msg_t *req, const uint8_t *src_addr, uint8_t src_family,
                                 uint16_t src_port, const uint8_t *key, size_t key_len,
                                 stun_hmac_sha1_fn hmac_sha1, uint8_t *buf, size_t buf_len,
                                 size_t *out_len)
{
    if (!req || !src_addr || !key || !hmac_sha1 || !buf || !out_len) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* Minimum: header(20) + XOR-MAPPED(12/24) + MI(24) + FP(8) = 64/76 */
    if (buf_len < 76) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }

    /* Header (length will be patched later) */
    write_stun_header(buf, STUN_BINDING_RESPONSE, 0, req->transaction_id);
    size_t pos = STUN_HEADER_SIZE;

    /* XOR-MAPPED-ADDRESS (RFC 8489 §14.2) */
    if (src_family == STUN_FAMILY_IPV4) {
        uint8_t *val = write_attr_hdr(buf + pos, STUN_ATTR_XOR_MAPPED_ADDRESS, 8);
        val[0] = 0; /* reserved */
        val[1] = STUN_FAMILY_IPV4;
        /* Port XOR top 16 bits of magic cookie */
        write_u16(val + 2, src_port ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));
        /* Address XOR magic cookie */
        uint32_t addr = read_u32(src_addr);
        write_u32(val + 4, addr ^ STUN_MAGIC_COOKIE);
        pos += 4 + 8;
    } else if (src_family == STUN_FAMILY_IPV6) {
        uint8_t *val = write_attr_hdr(buf + pos, STUN_ATTR_XOR_MAPPED_ADDRESS, 20);
        val[0] = 0;
        val[1] = STUN_FAMILY_IPV6;
        write_u16(val + 2, src_port ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));
        /* Address XOR (cookie || txid) */
        uint8_t mask[16];
        write_u32(mask, STUN_MAGIC_COOKIE);
        memcpy(mask + 4, req->transaction_id, STUN_TXID_SIZE);
        for (int i = 0; i < 16; i++) {
            val[4 + i] = src_addr[i] ^ mask[i];
        }
        pos += 4 + 20;
    } else {
        return NANO_ERR_INVALID_PARAM;
    }

    /* MESSAGE-INTEGRITY + FINGERPRINT */
    int total = append_integrity_fingerprint(buf, buf_len, pos, key, key_len, hmac_sha1);
    if (total < 0) {
        return total;
    }

    *out_len = (size_t)total;
    return NANO_OK;
}

/* ----------------------------------------------------------------
 * stun_encode_binding_request — RFC 8445 §7.1.1
 * ---------------------------------------------------------------- */

int stun_encode_binding_request(const char *username, size_t username_len, uint32_t priority,
                                bool use_candidate, bool is_controlling, uint64_t tie_breaker,
                                const uint8_t transaction_id[STUN_TXID_SIZE], const uint8_t *key,
                                size_t key_len, stun_hmac_sha1_fn hmac_sha1, uint8_t *buf,
                                size_t buf_len, size_t *out_len)
{
    if (!username || !transaction_id || !key || !hmac_sha1 || !buf || !out_len) {
        return NANO_ERR_INVALID_PARAM;
    }

    /* Estimate: header(20) + USERNAME(4+padded) + PRIORITY(8) + ICE-*(12)
     * + USE-CANDIDATE(4) + MI(24) + FP(8) ≈ 80 + username_len */
    size_t needed = STUN_HEADER_SIZE + (4 + attr_padded((uint16_t)username_len)) + 8 + 12 +
                    (use_candidate ? 4 : 0) + 24 + 8;
    if (buf_len < needed) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }

    /* Header */
    write_stun_header(buf, STUN_BINDING_REQUEST, 0, transaction_id);
    size_t pos = STUN_HEADER_SIZE;

    /* USERNAME (RFC 8489 §14.3) */
    uint8_t *uval = write_attr_hdr(buf + pos, STUN_ATTR_USERNAME, (uint16_t)username_len);
    memcpy(uval, username, username_len);
    /* Zero-pad to 4-byte boundary */
    size_t pad = attr_padded((uint16_t)username_len) - username_len;
    if (pad > 0) {
        memset(uval + username_len, 0, pad);
    }
    pos += 4 + attr_padded((uint16_t)username_len);

    /* PRIORITY (RFC 8445 §7.1.1) */
    uint8_t *pval = write_attr_hdr(buf + pos, STUN_ATTR_PRIORITY, 4);
    write_u32(pval, priority);
    pos += 8;

    /* ICE-CONTROLLING or ICE-CONTROLLED (RFC 8445 §7.1.1) */
    if (is_controlling) {
        uint8_t *cval = write_attr_hdr(buf + pos, STUN_ATTR_ICE_CONTROLLING, 8);
        write_u64(cval, tie_breaker);
        pos += 12;
    } else {
        uint8_t *cval = write_attr_hdr(buf + pos, STUN_ATTR_ICE_CONTROLLED, 8);
        write_u64(cval, tie_breaker);
        pos += 12;
    }

    /* USE-CANDIDATE (RFC 8445 §7.1.1 — controlling role only) */
    if (use_candidate) {
        write_attr_hdr(buf + pos, STUN_ATTR_USE_CANDIDATE, 0);
        pos += 4;
    }

    /* MESSAGE-INTEGRITY + FINGERPRINT */
    int total = append_integrity_fingerprint(buf, buf_len, pos, key, key_len, hmac_sha1);
    if (total < 0) {
        return total;
    }

    *out_len = (size_t)total;
    return NANO_OK;
}
