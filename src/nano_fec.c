/*
 * nanortc — ULPFEC codec (RFC 5109 level-0). See nano_fec.h.
 *
 * Protection operation (RFC 5109 §6.2.1): each media packet's "FEC bit string"
 * is  RTP[0:8] || u16(payload_len) || RTP[12:]  (SSRC at [8:12] excluded; CSRC /
 * extensions out of scope — nanortc media carries neither). The FEC packet
 * stores the XOR of these, with the SN field replaced by SN base and a level-0
 * mask of the protected offsets. Recovery XORs the FEC packet with the received
 * members to reconstruct the one absent member.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_fec.h"

#include "nanortc.h" /* NANORTC_OK / NANORTC_ERR_* + read/write u16/u32 helpers */

#include <string.h>

#if NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC

/* Earliest SN in signed-16 space (handles wraparound), so every member sits at
 * a non-negative offset from the base. */
static uint16_t fec_sn_base(const uint8_t *const *pkts, uint8_t count)
{
    uint16_t base = nanortc_read_u16be(pkts[0] + 2);
    for (uint8_t i = 1; i < count; i++) {
        uint16_t sn = nanortc_read_u16be(pkts[i] + 2);
        if ((int16_t)(sn - base) < 0) {
            base = sn;
        }
    }
    return base;
}

int fec_encode(const uint8_t *const *pkts, const uint16_t *lens, uint8_t count, uint8_t *out,
               size_t out_cap, size_t *out_len)
{
    if (!pkts || !lens || !out || !out_len || count == 0 || count > FEC_MAX_GROUP) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    uint16_t sn_base = fec_sn_base(pkts, count);
    size_t max_bs = 0; /* longest FEC bit string = max(len_i - 2) */
    for (uint8_t i = 0; i < count; i++) {
        if (!pkts[i] || lens[i] < FEC_RTP_HDR) {
            return NANORTC_ERR_INVALID_PARAM;
        }
        if ((uint16_t)(nanortc_read_u16be(pkts[i] + 2) - sn_base) >= FEC_MAX_GROUP) {
            return NANORTC_ERR_INVALID_PARAM; /* group spans >= 16 SNs */
        }
        size_t bs = (size_t)lens[i] - 2;
        if (bs > max_bs) {
            max_bs = bs;
        }
    }

    /* protection length = the FEC payload region (bit string minus the 10-byte
     * header part). total = 10 + 4 + protlen = max_bs + 4. */
    uint16_t protlen = (uint16_t)(max_bs - FEC_HEADER_SIZE);
    size_t total = FEC_OVERHEAD + (size_t)protlen;
    if (out_cap < total) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    memset(out, 0, total);

    uint16_t mask = 0;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = pkts[i];
        out[0] ^= (uint8_t)(p[0] & 0x3Fu); /* P/X/CC recovery (V dropped) */
        out[1] ^= p[1];                    /* M/PT recovery */
        out[4] ^= p[4];                    /* TS recovery */
        out[5] ^= p[5];
        out[6] ^= p[6];
        out[7] ^= p[7];
        uint16_t len16 = (uint16_t)(lens[i] - FEC_RTP_HDR);
        out[8] ^= (uint8_t)(len16 >> 8); /* length recovery */
        out[9] ^= (uint8_t)(len16 & 0xFFu);
        for (uint16_t k = 0; k < len16; k++) {
            out[FEC_OVERHEAD + k] ^= p[FEC_RTP_HDR + k]; /* payload XOR */
        }
        uint16_t off = (uint16_t)(nanortc_read_u16be(p + 2) - sn_base);
        mask |= (uint16_t)(0x8000u >> off); /* level-0 mask: MSB = base+0 */
    }

    nanortc_write_u16be(out + 2, sn_base); /* SN base (not XOR'd) */
    nanortc_write_u16be(out + 10, protlen);
    nanortc_write_u16be(out + 12, mask);

    *out_len = total;
    return NANORTC_OK;
}

int fec_recover(const uint8_t *fec, size_t fec_len, uint32_t media_ssrc, const uint8_t *const *recv,
                const uint16_t *recv_lens, uint8_t n_recv, uint8_t *out, size_t out_cap,
                size_t *out_len, uint16_t *out_seq)
{
    if (!fec || !out || !out_len || !out_seq || fec_len < FEC_OVERHEAD) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    uint16_t sn_base = nanortc_read_u16be(fec + 2);
    uint16_t protlen = nanortc_read_u16be(fec + 10);
    uint16_t mask = nanortc_read_u16be(fec + 12);
    if (fec_len < (size_t)FEC_OVERHEAD + protlen) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Find the single protected SN absent from recv[]. */
    int missing_off = -1;
    int missing_count = 0;
    for (int off = 0; off < FEC_MAX_GROUP; off++) {
        if (!(mask & (uint16_t)(0x8000u >> off))) {
            continue;
        }
        uint16_t sn = (uint16_t)(sn_base + off);
        int present = 0;
        for (uint8_t j = 0; j < n_recv; j++) {
            if (recv[j] && recv_lens[j] >= FEC_RTP_HDR && nanortc_read_u16be(recv[j] + 2) == sn) {
                present = 1;
                break;
            }
        }
        if (!present) {
            missing_count++;
            missing_off = off;
        }
    }
    if (missing_count != 1) {
        return NANORTC_ERR_NO_DATA; /* 0 lost, or >=2 (level-0 recovers exactly one) */
    }
    uint16_t missing_sn = (uint16_t)(sn_base + missing_off);

    if (out_cap < (size_t)FEC_RTP_HDR + protlen) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Seed the recovery fields from the FEC packet, then XOR in each received
     * member of this group. */
    uint8_t r0 = (uint8_t)(fec[0] & 0x3Fu);
    uint8_t r1 = fec[1];
    uint8_t rts[4] = {fec[4], fec[5], fec[6], fec[7]};
    uint16_t rlen = nanortc_read_u16be(fec + 8);
    memcpy(out + FEC_RTP_HDR, fec + FEC_OVERHEAD, protlen); /* payload = FEC payload */

    for (uint8_t j = 0; j < n_recv; j++) {
        if (!recv[j] || recv_lens[j] < FEC_RTP_HDR) {
            continue;
        }
        uint16_t off = (uint16_t)(nanortc_read_u16be(recv[j] + 2) - sn_base);
        if (off >= FEC_MAX_GROUP || !(mask & (uint16_t)(0x8000u >> off))) {
            continue; /* not a member of this FEC group */
        }
        const uint8_t *p = recv[j];
        r0 ^= (uint8_t)(p[0] & 0x3Fu);
        r1 ^= p[1];
        rts[0] ^= p[4];
        rts[1] ^= p[5];
        rts[2] ^= p[6];
        rts[3] ^= p[7];
        uint16_t l16 = (uint16_t)(recv_lens[j] - FEC_RTP_HDR);
        rlen ^= l16;
        uint16_t pcopy = (l16 < protlen) ? l16 : protlen;
        for (uint16_t k = 0; k < pcopy; k++) {
            out[FEC_RTP_HDR + k] ^= p[FEC_RTP_HDR + k];
        }
    }

    if (rlen > protlen) {
        rlen = protlen; /* defensive: recovered length cannot exceed protection */
    }

    /* Rebuild the recovered RTP packet header. V=2 (0x80); CSRC/extension out of
     * scope, so the header is the fixed 12 bytes. */
    out[0] = (uint8_t)(0x80u | r0);
    out[1] = r1;
    nanortc_write_u16be(out + 2, missing_sn);
    memcpy(out + 4, rts, 4);
    nanortc_write_u32be(out + 8, media_ssrc);

    *out_len = (size_t)FEC_RTP_HDR + rlen;
    *out_seq = missing_sn;
    return NANORTC_OK;
}

#endif /* NANORTC_FEATURE_VIDEO && NANORTC_FEATURE_VIDEO_FEC */
