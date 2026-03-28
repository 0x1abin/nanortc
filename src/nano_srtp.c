/*
 * nanortc — SRTP encryption/decryption (RFC 3711)
 *
 * Key derivation: RFC 3711 section 4.3.1
 * Protect/Unprotect: RFC 3711 section 3.3
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_srtp.h"
#include "nano_rtp.h"
#include "nanortc_crypto.h"
#include "nanortc.h"
#include <string.h>

/* RFC 3711 section 4.3.1: key derivation rate labels */
#define SRTP_LABEL_RTP_CIPHER  0x00
#define SRTP_LABEL_RTP_AUTH    0x01
#define SRTP_LABEL_RTP_SALT    0x02
#define SRTP_LABEL_RTCP_CIPHER 0x03
#define SRTP_LABEL_RTCP_AUTH   0x04
#define SRTP_LABEL_RTCP_SALT   0x05

/* Maximum derived key output: 32 bytes (2 AES blocks, for 20-byte auth key) */
#define SRTP_KDF_MAX_OUT 32

/*
 * RFC 3711 section 4.3.1: Key derivation function.
 *
 * input[16] = salt(14 bytes, zero-padded to 16) XOR (label at byte 7)
 * For each 16-byte block needed, AES-CM(master_key, input) with counter
 * at bytes 14-15.
 *
 * AES-CM with zero plaintext = AES-ECB of the IV, which is exactly what
 * the KDF requires: AES_ECB(master_key, input || counter).
 */
static int srtp_kdf(const nanortc_crypto_provider_t *crypto,
                    const uint8_t master_key[NANORTC_SRTP_KEY_SIZE],
                    const uint8_t master_salt[NANORTC_SRTP_SALT_SIZE], uint8_t label, uint8_t *out,
                    size_t out_len)
{
    uint8_t iv[16];
    uint8_t zeros[SRTP_KDF_MAX_OUT];
    uint8_t derived[SRTP_KDF_MAX_OUT];

    if (out_len > SRTP_KDF_MAX_OUT) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Build IV: copy salt into first 14 bytes, XOR label at byte 7 */
    memset(iv, 0, 16);
    memcpy(iv, master_salt, NANORTC_SRTP_SALT_SIZE);
    iv[7] ^= label;
    /* Bytes 14-15 = 0 (counter starts at 0, handled by AES-CM) */

    /* Derive: AES-CM(master_key, iv, zeros) = AES-ECB blocks */
    size_t derive_len = out_len <= 16 ? 16 : 32;
    memset(zeros, 0, derive_len);

    int rc = crypto->aes_128_cm(master_key, iv, zeros, derive_len, derived);
    if (rc != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    memcpy(out, derived, out_len);
    return NANORTC_OK;
}

int srtp_init(nano_srtp_t *srtp, const nanortc_crypto_provider_t *crypto, int is_client)
{
    if (!srtp) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(srtp, 0, sizeof(*srtp));
    srtp->crypto = crypto;
    srtp->is_client = is_client;
    return NANORTC_OK;
}

int srtp_derive_keys(nano_srtp_t *srtp, const uint8_t *keying_material, size_t len)
{
    if (!srtp || !keying_material || !srtp->crypto) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (len < 60) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /*
     * RFC 5764 section 4.2: keying material layout (60 bytes):
     *   client_key(16) | server_key(16) | client_salt(14) | server_salt(14)
     *
     * DTLS client: send=client keys, recv=server keys
     * DTLS server: send=server keys, recv=client keys
     */
    const uint8_t *client_key = keying_material;
    const uint8_t *server_key = keying_material + 16;
    const uint8_t *client_salt = keying_material + 32;
    const uint8_t *server_salt = keying_material + 46;

    const uint8_t *send_master_key, *send_master_salt;
    const uint8_t *recv_master_key, *recv_master_salt;

    if (srtp->is_client) {
        send_master_key = client_key;
        send_master_salt = client_salt;
        recv_master_key = server_key;
        recv_master_salt = server_salt;
    } else {
        send_master_key = server_key;
        send_master_salt = server_salt;
        recv_master_key = client_key;
        recv_master_salt = client_salt;
    }

    int rc;

    /* Derive send RTP keys */
    rc = srtp_kdf(srtp->crypto, send_master_key, send_master_salt, SRTP_LABEL_RTP_CIPHER,
                  srtp->send_key, NANORTC_SRTP_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, send_master_key, send_master_salt, SRTP_LABEL_RTP_AUTH,
                  srtp->send_auth_key, NANORTC_SRTP_AUTH_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, send_master_key, send_master_salt, SRTP_LABEL_RTP_SALT,
                  srtp->send_salt, NANORTC_SRTP_SALT_SIZE);
    if (rc != 0)
        return rc;

    /* Derive send RTCP keys */
    rc = srtp_kdf(srtp->crypto, send_master_key, send_master_salt, SRTP_LABEL_RTCP_CIPHER,
                  srtp->send_rtcp_key, NANORTC_SRTP_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, send_master_key, send_master_salt, SRTP_LABEL_RTCP_AUTH,
                  srtp->send_rtcp_auth_key, NANORTC_SRTP_AUTH_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, send_master_key, send_master_salt, SRTP_LABEL_RTCP_SALT,
                  srtp->send_rtcp_salt, NANORTC_SRTP_SALT_SIZE);
    if (rc != 0)
        return rc;

    /* Derive recv RTP keys */
    rc = srtp_kdf(srtp->crypto, recv_master_key, recv_master_salt, SRTP_LABEL_RTP_CIPHER,
                  srtp->recv_key, NANORTC_SRTP_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, recv_master_key, recv_master_salt, SRTP_LABEL_RTP_AUTH,
                  srtp->recv_auth_key, NANORTC_SRTP_AUTH_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, recv_master_key, recv_master_salt, SRTP_LABEL_RTP_SALT,
                  srtp->recv_salt, NANORTC_SRTP_SALT_SIZE);
    if (rc != 0)
        return rc;

    /* Derive recv RTCP keys */
    rc = srtp_kdf(srtp->crypto, recv_master_key, recv_master_salt, SRTP_LABEL_RTCP_CIPHER,
                  srtp->recv_rtcp_key, NANORTC_SRTP_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, recv_master_key, recv_master_salt, SRTP_LABEL_RTCP_AUTH,
                  srtp->recv_rtcp_auth_key, NANORTC_SRTP_AUTH_KEY_SIZE);
    if (rc != 0)
        return rc;

    rc = srtp_kdf(srtp->crypto, recv_master_key, recv_master_salt, SRTP_LABEL_RTCP_SALT,
                  srtp->recv_rtcp_salt, NANORTC_SRTP_SALT_SIZE);
    if (rc != 0)
        return rc;

    srtp->ready = 1;
    return NANORTC_OK;
}

/*
 * Construct SRTP IV per RFC 3711 section 4.1.1.
 *
 * IV[16]:
 *   bytes 0-3:   0
 *   bytes 4-7:   SSRC (big-endian)
 *   bytes 6-13:  XOR with packet_index (48-bit, as 8-byte big-endian)
 *   bytes 0-13:  XOR with session salt
 *   bytes 14-15: 0 (block counter, handled by AES-CM)
 */
static void srtp_compute_iv(uint8_t iv[16], const uint8_t salt[NANORTC_SRTP_SALT_SIZE],
                            uint32_t ssrc, uint64_t index)
{
    memset(iv, 0, 16);

    /* SSRC at bytes 4-7 */
    iv[4] = (uint8_t)(ssrc >> 24);
    iv[5] = (uint8_t)(ssrc >> 16);
    iv[6] = (uint8_t)(ssrc >> 8);
    iv[7] = (uint8_t)(ssrc);

    /* XOR packet index (48-bit) at bytes 6-13 */
    iv[6] ^= (uint8_t)(index >> 40);
    iv[7] ^= (uint8_t)(index >> 32);
    iv[8] ^= (uint8_t)(index >> 24);
    iv[9] ^= (uint8_t)(index >> 16);
    iv[10] ^= (uint8_t)(index >> 8);
    iv[11] ^= (uint8_t)(index);
    /* index is 48-bit, so only 6 bytes are significant.
     * Bytes 12-13 of index are always 0 for 48-bit values. */

    /* XOR salt at bytes 0-13 */
    for (int i = 0; i < NANORTC_SRTP_SALT_SIZE; i++) {
        iv[i] ^= salt[i];
    }
}

int srtp_protect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len)
{
    if (!srtp || !packet || !out_len || !srtp->ready) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (len < RTP_HEADER_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Parse RTP header fields we need */
    uint16_t seq = nanortc_read_u16be(packet + 2);
    uint32_t ssrc = nanortc_read_u32be(packet + 8);

    /* Determine actual header length (CSRC + extensions) */
    uint8_t cc = packet[0] & 0x0F;
    size_t hdr_len = RTP_HEADER_SIZE + (size_t)cc * 4;
    if (packet[0] & 0x10) { /* X bit: extension header */
        if (len < hdr_len + 4) {
            return NANORTC_ERR_PARSE;
        }
        uint16_t ext_words = nanortc_read_u16be(packet + hdr_len + 2);
        hdr_len += 4 + (size_t)ext_words * 4;
    }
    if (len < hdr_len) {
        return NANORTC_ERR_PARSE;
    }

    /* RFC 3711 section 3.3.1: packet index = ROC * 65536 + SEQ */
    uint64_t index = ((uint64_t)srtp->send_roc << 16) | seq;

    /* Encrypt payload in-place (header remains in the clear) */
    size_t payload_len = len - hdr_len;
    if (payload_len > 0) {
        uint8_t iv[16];
        srtp_compute_iv(iv, srtp->send_salt, ssrc, index);

        int rc = srtp->crypto->aes_128_cm(srtp->send_key, iv, packet + hdr_len, payload_len,
                                          packet + hdr_len);
        if (rc != 0) {
            return NANORTC_ERR_CRYPTO;
        }
    }

    /* RFC 3711 section 4.2: authentication tag
     * HMAC-SHA1(auth_key, header || encrypted_payload || ROC) truncated to 80 bits.
     *
     * We need to HMAC: [packet (len bytes)] [ROC (4 bytes)]
     * Since our hmac_sha1_80 takes a single contiguous buffer, we'll
     * need to use the full HMAC-SHA1 and truncate manually. */
    {
        uint8_t roc_be[4];
        roc_be[0] = (uint8_t)(srtp->send_roc >> 24);
        roc_be[1] = (uint8_t)(srtp->send_roc >> 16);
        roc_be[2] = (uint8_t)(srtp->send_roc >> 8);
        roc_be[3] = (uint8_t)(srtp->send_roc);

        /* Two-part HMAC: first the packet, then ROC.
         * Use full HMAC-SHA1 with concatenated input.
         * We need a scratch buffer for packet + ROC. To avoid allocation,
         * append ROC after the packet data (caller must have room for +14 bytes). */
        memcpy(packet + len, roc_be, 4);

        /* Compute HMAC-SHA1-80 over [packet || ROC] */
        srtp->crypto->hmac_sha1_80(srtp->send_auth_key, NANORTC_SRTP_AUTH_KEY_SIZE, packet, len + 4,
                                   packet + len);
        /* Auth tag now at packet + len, ROC was overwritten by the tag (first 10 bytes).
         * The tag is 10 bytes, placed right after the original packet data. */
    }

    /* Update ROC on sequence wrap */
    if (seq == 0xFFFF) {
        srtp->send_roc++;
    }
    srtp->send_seq = seq;

    *out_len = len + NANORTC_SRTP_AUTH_TAG_SIZE;
    return NANORTC_OK;
}

int srtp_unprotect(nano_srtp_t *srtp, uint8_t *packet, size_t len, size_t *out_len)
{
    if (!srtp || !packet || !out_len || !srtp->ready) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (len < RTP_HEADER_SIZE + NANORTC_SRTP_AUTH_TAG_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Parse RTP header */
    uint16_t seq = nanortc_read_u16be(packet + 2);
    uint32_t ssrc = nanortc_read_u32be(packet + 8);

    /* Determine header length */
    uint8_t cc = packet[0] & 0x0F;
    size_t hdr_len = RTP_HEADER_SIZE + (size_t)cc * 4;
    if (packet[0] & 0x10) {
        if (len < hdr_len + 4 + NANORTC_SRTP_AUTH_TAG_SIZE) {
            return NANORTC_ERR_PARSE;
        }
        uint16_t ext_words = nanortc_read_u16be(packet + hdr_len + 2);
        hdr_len += 4 + (size_t)ext_words * 4;
    }

    size_t srtp_len = len - NANORTC_SRTP_AUTH_TAG_SIZE; /* original RTP packet length */
    if (srtp_len < hdr_len) {
        return NANORTC_ERR_PARSE;
    }

    /* Estimate packet index (RFC 3711 section 3.3.1)
     * Simple ROC estimation with 16-bit window */
    uint32_t est_roc = srtp->recv_roc;
    if (srtp->recv_seq_max != 0 || srtp->recv_roc != 0) {
        /* If seq rolled over (new seq much smaller than max) */
        if ((int16_t)(seq - srtp->recv_seq_max) < -0x4000) {
            est_roc = srtp->recv_roc + 1;
        } else if ((int16_t)(seq - srtp->recv_seq_max) > 0x4000 && srtp->recv_roc > 0) {
            est_roc = srtp->recv_roc - 1;
        }
    }
    uint64_t index = ((uint64_t)est_roc << 16) | seq;

    /* Verify authentication tag (RFC 3711 section 4.2) */
    {
        uint8_t roc_be[4];
        roc_be[0] = (uint8_t)(est_roc >> 24);
        roc_be[1] = (uint8_t)(est_roc >> 16);
        roc_be[2] = (uint8_t)(est_roc >> 8);
        roc_be[3] = (uint8_t)(est_roc);

        /* Save the received tag, then write ROC temporarily after the SRTP payload */
        uint8_t recv_tag[NANORTC_SRTP_AUTH_TAG_SIZE];
        memcpy(recv_tag, packet + srtp_len, NANORTC_SRTP_AUTH_TAG_SIZE);

        /* Temporarily place ROC after encrypted payload for HMAC computation */
        memcpy(packet + srtp_len, roc_be, 4);

        uint8_t computed_tag[NANORTC_SRTP_AUTH_TAG_SIZE];
        srtp->crypto->hmac_sha1_80(srtp->recv_auth_key, NANORTC_SRTP_AUTH_KEY_SIZE, packet,
                                   srtp_len + 4, computed_tag);

        /* Restore the original tag (for debugging) and verify */
        memcpy(packet + srtp_len, recv_tag, NANORTC_SRTP_AUTH_TAG_SIZE);

        /* Constant-time comparison */
        uint8_t diff = 0;
        for (int i = 0; i < NANORTC_SRTP_AUTH_TAG_SIZE; i++) {
            diff |= recv_tag[i] ^ computed_tag[i];
        }
        if (diff != 0) {
            return NANORTC_ERR_CRYPTO;
        }
    }

    /* Decrypt payload in-place */
    size_t payload_len = srtp_len - hdr_len;
    if (payload_len > 0) {
        uint8_t iv[16];
        srtp_compute_iv(iv, srtp->recv_salt, ssrc, index);

        int rc = srtp->crypto->aes_128_cm(srtp->recv_key, iv, packet + hdr_len, payload_len,
                                          packet + hdr_len);
        if (rc != 0) {
            return NANORTC_ERR_CRYPTO;
        }
    }

    /* Update ROC and seq tracking */
    if (est_roc > srtp->recv_roc ||
        (est_roc == srtp->recv_roc && (int16_t)(seq - srtp->recv_seq_max) > 0)) {
        srtp->recv_roc = est_roc;
        srtp->recv_seq_max = seq;
    }

    *out_len = srtp_len;
    return NANORTC_OK;
}
