/*
 * nanortc — SDP parser/generator (RFC 8866, RFC 8829)
 *
 * Sans I/O: no stdio.h. Uses memcpy + manual itoa for string building.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sdp.h"
#include "nano_ice.h"
#include "nano_addr.h"
#include "nanortc.h"
#include "nanortc_util.h"
#include <string.h>

/* ================================================================
 * Helpers (no stdio.h)
 * ================================================================ */

/** Append a string to buf at *pos, advance *pos. Returns false if overflow. */
static bool sdp_append(char *buf, size_t buf_len, size_t *pos, const char *str)
{
    size_t slen = 0;
    while (str[slen])
        slen++;

    if (*pos + slen >= buf_len) {
        return false;
    }
    memcpy(buf + *pos, str, slen);
    *pos += slen;
    buf[*pos] = '\0';
    return true;
}

/** Append uint16_t as decimal to buf. */
static bool sdp_append_u16(char *buf, size_t buf_len, size_t *pos, uint16_t val)
{
    char tmp[8];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        char rev[8];
        int r = 0;
        uint16_t v = val;
        while (v > 0) {
            rev[r++] = '0' + (v % 10);
            v /= 10;
        }
        while (r > 0) {
            tmp[i++] = rev[--r];
        }
    }
    tmp[i] = '\0';
    return sdp_append(buf, buf_len, pos, tmp);
}

static bool sdp_append_u32(char *buf, size_t buf_len, size_t *pos, uint32_t val)
{
    char tmp[12];
    int i = 0;
    if (val == 0) {
        tmp[i++] = '0';
    } else {
        char rev[12];
        int r = 0;
        uint32_t v = val;
        while (v > 0) {
            rev[r++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (r > 0) {
            tmp[i++] = rev[--r];
        }
    }
    tmp[i] = '\0';
    return sdp_append(buf, buf_len, pos, tmp);
}

/** Parse decimal number from string, advancing pointer past digits. */
static uint32_t parse_u32(const char *p, const char *end, const char **next)
{
    uint32_t val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        val = val * 10 + (uint32_t)(*p - '0');
        p++;
    }
    if (next) {
        *next = p;
    }
    return val;
}

/* ================================================================
 * Parser
 * ================================================================ */

/** Find next line end (\r\n or \n). Returns index of first byte after EOL. */
static size_t find_eol(const char *s, size_t len, size_t start)
{
    size_t i = start;
    while (i < len) {
        if (s[i] == '\n') {
            return i + 1;
        }
        if (s[i] == '\r' && i + 1 < len && s[i + 1] == '\n') {
            return i + 2;
        }
        i++;
    }
    return len;
}

/** Check if line at pos starts with prefix. */
static bool line_starts_with(const char *line, size_t line_len, const char *prefix)
{
    size_t plen = 0;
    while (prefix[plen])
        plen++;
    if (line_len < plen)
        return false;
    return memcmp(line, prefix, plen) == 0;
}

/** Copy value after prefix into dst, up to dst_size-1. */
static void extract_value(const char *line, size_t line_len, const char *prefix, char *dst,
                          size_t dst_size)
{
    size_t plen = 0;
    while (prefix[plen])
        plen++;

    size_t vlen = 0;
    const char *val = line + plen;
    /* Skip to end of line, exclude \r\n */
    while (plen + vlen < line_len) {
        char c = val[vlen];
        if (c == '\r' || c == '\n')
            break;
        vlen++;
    }

    /* Strip trailing whitespace (defense against SDP generators that
     * add trailing spaces — protects exact-fit buffers like fingerprint). */
    while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
        vlen--;
    }

    if (vlen >= dst_size) {
        vlen = dst_size - 1;
    }
    memcpy(dst, val, vlen);
    dst[vlen] = '\0';
}

static int sdp_store_attribute(const char *value, size_t len, char *dst, size_t cap)
{
    while (len && (value[len - 1] == '\r' || value[len - 1] == '\n' || value[len - 1] == ' ' ||
                   value[len - 1] == '\t'))
        len--;
    if (!len || len >= cap)
        return NANORTC_ERR_PARSE;
    if (dst[0] && (nanortc_strnlen(dst, cap) != len || memcmp(dst, value, len) != 0))
        return NANORTC_ERR_PARSE;
    memcpy(dst, value, len);
    dst[len] = '\0';
    return NANORTC_OK;
}

static int sdp_hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int sdp_parse_fingerprint(const char *value, size_t len, uint8_t out[NANORTC_DTLS_FINGERPRINT_SIZE])
{
    if (!value || !out)
        return NANORTC_ERR_INVALID_PARAM;
    while (len && (value[len - 1] == '\r' || value[len - 1] == '\n' || value[len - 1] == ' ' ||
                   value[len - 1] == '\t'))
        len--;
    if (len != 8u + NANORTC_DTLS_FINGERPRINT_SIZE * 3u - 1u || memcmp(value, "sha-256 ", 8) != 0)
        return NANORTC_ERR_PARSE;
    uint8_t parsed[NANORTC_DTLS_FINGERPRINT_SIZE];
    for (size_t i = 0; i < sizeof(parsed); i++) {
        int hi = sdp_hex(value[8u + i * 3u]);
        int lo = sdp_hex(value[9u + i * 3u]);
        if (hi < 0 || lo < 0 || (i + 1u < sizeof(parsed) && value[10u + i * 3u] != ':'))
            return NANORTC_ERR_PARSE;
        parsed[i] = (uint8_t)(hi * 16 + lo);
    }
    memcpy(out, parsed, sizeof(parsed));
    return NANORTC_OK;
}

typedef struct {
    const char *ptr;
    size_t len;
} sdp_token_t;
static bool sdp_token(const char **cursor, const char *end, sdp_token_t *token)
{
    const char *p = *cursor;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        p++;
    token->ptr = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        p++;
    token->len = (size_t)(p - token->ptr);
    *cursor = p;
    return token->len != 0;
}
static bool sdp_token_eq(sdp_token_t token, const char *value, size_t len)
{
    if (token.len != len)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = token.ptr[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != value[i])
            return false;
    }
    return true;
}
static bool sdp_token_u32(sdp_token_t token, uint32_t *out)
{
    uint32_t v = 0;
    if (!token.len)
        return false;
    for (size_t i = 0; i < token.len; i++) {
        unsigned digit = (unsigned)(token.ptr[i] - '0');
        if (digit > 9u || v > (UINT32_MAX - digit) / 10u)
            return false;
        v = v * 10u + digit;
    }
    *out = v;
    return true;
}

int sdp_parse_candidate(const char *text, size_t len, nano_sdp_candidate_t *candidate)
{
    if (!text || !candidate || !len)
        return NANORTC_ERR_INVALID_PARAM;
    const char *p = text, *end = text + len;
    if (memchr(text, 0, len))
        return NANORTC_ERR_PARSE;
    if (len >= 2u && memcmp(p, "a=", 2) == 0)
        p += 2;
    bool full = (size_t)(end - p) >= 10u && memcmp(p, "candidate:", 10) == 0;
    sdp_token_t address, port, key, value;
    uint8_t type = NANORTC_ICE_CAND_HOST;
    uint32_t priority_value = ICE_HOST_PRIORITY(0);
    uint32_t port_value;
    uint8_t related_addr[NANORTC_ADDR_SIZE] = {0}, related_family = 0;
    uint32_t related_port = 0;
    bool has_related_addr = false, has_related_port = false;
    if (full) {
        p += 10;
        sdp_token_t foundation, component, transport, priority;
        uint32_t component_value;
        if (!sdp_token(&p, end, &foundation) || !sdp_token(&p, end, &component) ||
            !sdp_token(&p, end, &transport) || !sdp_token(&p, end, &priority) ||
            !sdp_token(&p, end, &address) || !sdp_token(&p, end, &port) ||
            !sdp_token_u32(component, &component_value) ||
            !sdp_token_u32(priority, &priority_value))
            return NANORTC_ERR_PARSE;
        if (component_value != 1u || !sdp_token_eq(transport, "udp", 3))
            return NANORTC_ERR_NOT_IMPLEMENTED;
        if (!sdp_token(&p, end, &key) || !sdp_token_eq(key, "typ", 3) ||
            !sdp_token(&p, end, &value))
            return NANORTC_ERR_PARSE;
        if (sdp_token_eq(value, "host", 4))
            type = NANORTC_ICE_CAND_HOST;
        else if (sdp_token_eq(value, "srflx", 5))
            type = NANORTC_ICE_CAND_SRFLX;
        else if (sdp_token_eq(value, "prflx", 5))
            type = NANORTC_ICE_CAND_PRFLX;
        else if (sdp_token_eq(value, "relay", 5))
            type = NANORTC_ICE_CAND_RELAY;
        else
            return NANORTC_ERR_PARSE;
        while (sdp_token(&p, end, &key)) {
            if (!sdp_token(&p, end, &value))
                return NANORTC_ERR_PARSE;
            if (sdp_token_eq(key, "raddr", 5)) {
                if (has_related_addr)
                    return NANORTC_ERR_PARSE;
                int rc = addr_parse_auto(value.ptr, value.len, related_addr, &related_family);
                if (rc != NANORTC_OK)
                    return rc;
                has_related_addr = true;
            } else if (sdp_token_eq(key, "rport", 5)) {
                if (has_related_port || !sdp_token_u32(value, &related_port) ||
                    related_port > UINT16_MAX)
                    return NANORTC_ERR_PARSE;
                has_related_port = true;
            }
        }
        /* RFC 8839 §5.1: related address/port are required except for host. */
        if (has_related_addr != has_related_port ||
            (type == NANORTC_ICE_CAND_HOST ? has_related_addr : !has_related_addr))
            return NANORTC_ERR_PARSE;
    } else {
        if (!sdp_token(&p, end, &address) || !sdp_token(&p, end, &port) || sdp_token(&p, end, &key))
            return NANORTC_ERR_PARSE;
    }
    if (!sdp_token_u32(port, &port_value) || !port_value || port_value > UINT16_MAX ||
        address.len >= NANORTC_IPV6_STR_SIZE)
        return NANORTC_ERR_PARSE;
    uint8_t binary[NANORTC_ADDR_SIZE] = {0}, family;
    int rc = addr_parse_auto(address.ptr, address.len, binary, &family);
    if (rc != NANORTC_OK)
        return rc;
    memset(candidate, 0, sizeof(*candidate));
    memcpy(candidate->addr, binary, sizeof(candidate->addr));
    candidate->family = family;
    candidate->priority = priority_value;
    candidate->port = (uint16_t)port_value;
    candidate->type = type;
    memcpy(candidate->related_addr, related_addr, sizeof(candidate->related_addr));
    candidate->related_family = related_family;
    candidate->related_port = (uint16_t)related_port;
    candidate->has_related = has_related_addr;
    return NANORTC_OK;
}

void sdp_commit_remote(nano_sdp_t *sdp, const nano_sdp_remote_t *remote)
{
    memcpy(sdp->remote_ufrag, remote->remote_ufrag, sizeof(sdp->remote_ufrag));
    memcpy(sdp->remote_pwd, remote->remote_pwd, sizeof(sdp->remote_pwd));
    memcpy(sdp->remote_fingerprint, remote->remote_fingerprint, sizeof(sdp->remote_fingerprint));
    sdp->has_remote_fingerprint = remote->has_remote_fingerprint;
    sdp->remote_max_message_size = remote->remote_max_message_size;
    sdp->remote_sctp_port = remote->remote_sctp_port;
    sdp->remote_setup = remote->remote_setup;
    sdp->candidate_count = remote->candidate_count;
    sdp->end_of_candidates = remote->end_of_candidates;
    sdp->has_datachannel = remote->has_datachannel;
    sdp->dc_mid = remote->dc_mid;
    sdp->mid_count = remote->mid_count;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    for (uint8_t i = 0; i < NANORTC_MAX_MEDIA_TRACKS; i++) {
        nano_sdp_mline_t *m = &sdp->mlines[i];
        const nano_sdp_remote_mline_t *r = &remote->mlines[i];
        m->kind = r->kind;
        m->mid = r->mid;
        m->pt = r->pt;
        m->remote_pt = r->remote_pt;
        m->codec = r->codec;
        m->channels = r->channels;
        m->sample_rate = r->sample_rate;
        m->direction = r->direction;
        m->remote_direction = r->remote_direction;
        m->active = r->active;
        m->video_h264_rtpmap_pt = r->video_h264_rtpmap_pt;
        m->twcc_ext_id = r->twcc_ext_id;
#if NANORTC_FEATURE_H265
        m->video_h265_rtpmap_pt = r->video_h265_rtpmap_pt;
#endif
    }
    sdp->mline_count = remote->mline_count;
#endif
    sdp->parsed = remote->parsed;
}

void sdp_snapshot_remote(const nano_sdp_t *sdp, nano_sdp_remote_t *remote)
{
    memcpy(remote->remote_ufrag, sdp->remote_ufrag, sizeof(sdp->remote_ufrag));
    memcpy(remote->remote_pwd, sdp->remote_pwd, sizeof(sdp->remote_pwd));
    memcpy(remote->remote_fingerprint, sdp->remote_fingerprint, sizeof(sdp->remote_fingerprint));
    remote->has_remote_fingerprint = sdp->has_remote_fingerprint;
    remote->remote_max_message_size = sdp->remote_max_message_size;
    remote->remote_sctp_port = sdp->remote_sctp_port;
    remote->remote_setup = sdp->remote_setup;
    /* Persisted remote candidate addresses belong to ICE, not this SDP snapshot. */
    memset(remote->remote_candidates, 0, sizeof(remote->remote_candidates));
    remote->candidate_count = sdp->candidate_count;
    remote->end_of_candidates = sdp->end_of_candidates;
    remote->has_datachannel = sdp->has_datachannel;
    remote->dc_mid = sdp->dc_mid;
    remote->mid_count = sdp->mid_count;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    for (uint8_t i = 0; i < NANORTC_MAX_MEDIA_TRACKS; i++) {
        const nano_sdp_mline_t *m = &sdp->mlines[i];
        nano_sdp_remote_mline_t *r = &remote->mlines[i];
        r->kind = m->kind;
        r->mid = m->mid;
        r->pt = m->pt;
        r->remote_pt = m->remote_pt;
        r->codec = m->codec;
        r->channels = m->channels;
        r->sample_rate = m->sample_rate;
        r->direction = m->direction;
        r->remote_direction = m->remote_direction;
        r->active = m->active;
        r->video_h264_rtpmap_pt = m->video_h264_rtpmap_pt;
        r->twcc_ext_id = m->twcc_ext_id;
#if NANORTC_FEATURE_H265
        r->video_h265_rtpmap_pt = m->video_h265_rtpmap_pt;
#endif
    }
    remote->mline_count = sdp->mline_count;
#endif
    remote->parsed = sdp->parsed;
}

int sdp_parse(nano_sdp_t *sdp, const char *text, size_t len)
{
    nano_sdp_remote_t remote;
    int rc = sdp_parse_remote(sdp, text, len, &remote);
    if (rc == NANORTC_OK)
        sdp_commit_remote(sdp, &remote);
    return rc;
}

#if NANORTC_HAVE_MEDIA_TRANSPORT

/**
 * Parse m=<type> line and extract first payload type (field 4).
 * Format: m=<type> <port> <proto> <pt> [<pt2> ...]
 * @param skip  Number of chars to skip for "m=audio " or "m=video " prefix.
 */
static uint8_t parse_mline_first_pt(const char *line, size_t line_len, size_t skip)
{
    const char *p = line + skip;
    const char *end = line + line_len;
    int field = 1;

    /* Skip port(1), proto(2), then first PT is field 3 */
    while (p < end && field < 3) {
        if (*p == ' ') {
            field++;
            while (p < end && *p == ' ')
                p++;
        } else {
            p++;
        }
    }
    if (field == 3 && p < end) {
        return (uint8_t)parse_u32(p, end, NULL);
    }
    return 0;
}

/**
 * Parse a=rtpmap:<pt> <codec>/<rate>[/<channels>]
 * Applies to the current m-line entry.
 */
static void parse_rtpmap(nano_sdp_remote_mline_t *ml, const char *line, size_t line_len)
{
    const char *p = line + 9; /* skip "a=rtpmap:" (9 chars) */
    const char *end = line + line_len;

    const char *next;
    uint32_t pt = parse_u32(p, end, &next);
    if (next == p || next >= end || *next != ' ') {
        return;
    }

    if (ml->kind == SDP_MLINE_AUDIO) {
        /* Only parse the rtpmap for the remote's preferred PT */
        if ((uint8_t)pt != ml->remote_pt) {
            return;
        }
        p = next + 1;
        const char *slash = memchr(p, '/', (size_t)(end - p));
        ml->codec = NANORTC_CODEC_NONE;
        if (!slash)
            return;
        sdp_token_t name = {p, (size_t)(slash - p)};
        if (sdp_token_eq(name, "opus", 4))
            ml->codec = NANORTC_CODEC_OPUS;
        else if (sdp_token_eq(name, "pcma", 4))
            ml->codec = NANORTC_CODEC_PCMA;
        else if (sdp_token_eq(name, "pcmu", 4))
            ml->codec = NANORTC_CODEC_PCMU;
        ml->sample_rate = parse_u32(slash + 1, end, &next);
        ml->channels = 1;
        if (next < end && *next == '/') {
            uint32_t channels = parse_u32(next + 1, end, &next);
            ml->channels = channels <= UINT8_MAX ? (uint8_t)channels : 0;
        }
    } else if (ml->kind == SDP_MLINE_VIDEO) {
        p = next + 1;
        /* Match H264 (4 chars) */
        if (p + 4 <= end && memcmp(p, "H264", 4) == 0) {
            ml->video_h264_rtpmap_pt = (uint8_t)pt;
            if (ml->codec == NANORTC_CODEC_NONE)
                ml->codec = NANORTC_CODEC_H264;
        }
#if NANORTC_FEATURE_H265
        /* Match H265 (4 chars) — must come before any broader match */
        else if (p + 4 <= end && memcmp(p, "H265", 4) == 0) {
            ml->video_h265_rtpmap_pt = (uint8_t)pt;
            if (ml->codec == NANORTC_CODEC_NONE)
                ml->codec = NANORTC_CODEC_H265;
            /* If the local track is H.265, adopt the offer's PT here. A
             * later fmtp line for the same codec can still override, but
             * offers that ship an H.265 rtpmap with no companion fmtp
             * (Safari/WebKit) would otherwise leave ml->pt at the local
             * default — and that PT often maps to a different codec on
             * the offerer's side, silently breaking decode. */
            if (ml->codec == NANORTC_CODEC_H265) {
                ml->pt = (uint8_t)pt;
            }
        }
#endif
    }
}

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

int sdp_init(nano_sdp_t *sdp)
{
    if (!sdp) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(sdp, 0, sizeof(*sdp));
    sdp->local_sctp_port = 5000;
    sdp->local_setup = NANORTC_SDP_SETUP_PASSIVE; /* answerer default */
    return NANORTC_OK;
}

int sdp_parse_remote(const nano_sdp_t *local, const char *sdp_str, size_t len,
                     nano_sdp_remote_t *sdp)
{
    if (!local || !sdp || !sdp_str || len == 0)
        return NANORTC_ERR_INVALID_PARAM;
    memset(sdp, 0, sizeof(*sdp));
    if (memchr(sdp_str, 0, len))
        return NANORTC_ERR_PARSE;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    int current_mline_idx = -1;
#if NANORTC_FEATURE_H265
    bool h265_reject = false;
#endif
#endif

    sdp->remote_max_message_size = 65536u; /* RFC 8841 §6.1: absent attribute */
    size_t pos = 0;
    while (pos < len) {
        size_t eol = find_eol(sdp_str, len, pos);
        const char *line = sdp_str + pos;
        size_t line_len = eol - pos;

#if NANORTC_HAVE_MEDIA_TRANSPORT
        if (line_starts_with(line, line_len, "m=application ")) {
            current_mline_idx = -1;
            sdp->has_datachannel = true;
            sdp->dc_mid = sdp->mid_count;
            sdp->mid_count++;
        } else if (line_starts_with(line, line_len, "m=audio ") ||
                   line_starts_with(line, line_len, "m=video ")) {
            bool is_video = line_starts_with(line, line_len, "m=video ");
            if (sdp->mline_count < NANORTC_MAX_MEDIA_TRACKS) {
                const uint8_t idx = sdp->mline_count;
                const nano_sdp_mline_t *p = idx < local->mline_count ? &local->mlines[idx] : NULL;
                nano_sdp_remote_mline_t *ml = &sdp->mlines[idx];
                memset(ml, 0, sizeof(*ml));
                ml->kind = is_video ? SDP_MLINE_VIDEO : SDP_MLINE_AUDIO;
                ml->mid = sdp->mid_count;
                ml->active = true;
                ml->remote_pt = parse_mline_first_pt(line, line_len, 8);
                if (p) {
                    ml->codec = p->codec;
                    ml->direction = p->direction;
                    ml->sample_rate = p->sample_rate;
                    ml->channels = p->channels;
                }
                if (!is_video) {
                    /* RFC 3551 §6: only PT 0 and 8 have fixed audio mappings here. */
                    ml->codec = ml->remote_pt == 0   ? NANORTC_CODEC_PCMU
                                : ml->remote_pt == 8 ? NANORTC_CODEC_PCMA
                                                     : NANORTC_CODEC_NONE;
                    ml->sample_rate = 8000;
                    ml->channels = 1;
                }
                current_mline_idx = (int)sdp->mline_count;
                sdp->mline_count++;
            } else {
                return NANORTC_ERR_BUFFER_TOO_SMALL;
            }
            sdp->mid_count++;
        } else if (line_len >= 2 && line[0] == 'm' && line[1] == '=') {
            current_mline_idx = -1;
            sdp->mid_count++;
        } else if (current_mline_idx >= 0 && line_starts_with(line, line_len, "a=rtpmap:")) {
            parse_rtpmap(&sdp->mlines[current_mline_idx], line, line_len);
        } else if (current_mline_idx >= 0 &&
                   sdp->mlines[current_mline_idx].kind == SDP_MLINE_VIDEO &&
                   line_starts_with(line, line_len, "a=fmtp:")) {
            nano_sdp_remote_mline_t *ml = &sdp->mlines[current_mline_idx];
            /* Parse video fmtp for H264 packetization-mode=1 (RFC 6184 §8.1).
             * Prefer the entry whose profile-level-id matches our own (42e01f,
             * Constrained Baseline Level 3.1).  Fall back to any H264 with
             * packetization-mode=1 if no exact match. */
            const char *fp = line + 7;
            const char *fend = line + line_len;
            const char *fnext;
            uint32_t fmtp_pt = parse_u32(fp, fend, &fnext);
            bool has_mode1 = false;
            bool has_preferred_profile = false;
#if NANORTC_FEATURE_H265
            bool h265_bad_txmode = false;
            bool h265_bad_don = false;
#endif
            const char *search = fnext;
            size_t remain = (size_t)(fend - search);
            /* Scan to the shortest fingerprint we care about. H.264 needs
             * "packetization-mode=1" (20 B) and "profile-level-id=42e01f"
             * (23 B); H.265 needs "tx-mode=MSMT"/"tx-mode=MSST" (12 B) and
             * "sprop-max-don-diff=" + digit (20 B). The outer gate must be
             * loose enough that the shortest match can still fit — the inner
             * bounds gate each individual memcmp. */
            while (remain >= 12) {
                if (remain >= 20 && memcmp(search, "packetization-mode=1", 20) == 0) {
                    has_mode1 = true;
                }
                if (remain >= 23 && memcmp(search, "profile-level-id=42e01f", 23) == 0) {
                    has_preferred_profile = true;
                }
#if NANORTC_FEATURE_H265
                /* RFC 7798 §4.1: only SRST (single-stream) is supported. */
                if (memcmp(search, "tx-mode=MSMT", 12) == 0) {
                    h265_bad_txmode = true;
                }
                if (memcmp(search, "tx-mode=MSST", 12) == 0) {
                    h265_bad_txmode = true;
                }
                /* RFC 7798 §6.1: DON reordering unsupported; any non-zero value rejects. */
                if (remain >= 20 && memcmp(search, "sprop-max-don-diff=", 19) == 0) {
                    const char *vp = search + 19;
                    if (vp < fend && *vp >= '0' && *vp <= '9' && *vp != '0') {
                        h265_bad_don = true;
                    }
                }
#endif
                search++;
                remain--;
            }
            /* Select this PT if it's H264 with packetization-mode=1 and
             * the rtpmap PT matches (or is the first H264 we've seen).
             * Skip when the local track is explicitly H.265 — the H.265
             * branch below owns PT selection for that codec, and letting
             * the H.264 preferred-profile match win here would silently
             * downgrade the advertised codec. */
            bool is_valid_h264 = has_mode1 && (ml->video_h264_rtpmap_pt == 0 ||
                                               ml->video_h264_rtpmap_pt == (uint8_t)fmtp_pt);
#if NANORTC_FEATURE_H265
            bool local_is_h265 = (ml->codec == NANORTC_CODEC_H265);
#else
            bool local_is_h265 = false;
#endif
            if (is_valid_h264 && !local_is_h265 && (has_preferred_profile || ml->pt == 0)) {
                ml->pt = (uint8_t)fmtp_pt;
            }
#if NANORTC_FEATURE_H265
            /* H.265 fmtp validation: applies only when this fmtp's PT maps to an
             * H.265 rtpmap we already parsed. */
            if (ml->video_h265_rtpmap_pt != 0 && (uint8_t)fmtp_pt == ml->video_h265_rtpmap_pt) {
                if (h265_bad_txmode || h265_bad_don) {
                    h265_reject = true;
                } else if (ml->pt == 0 || ml->codec == NANORTC_CODEC_H265) {
                    ml->pt = (uint8_t)fmtp_pt;
                }
            }
#endif
        } else if (current_mline_idx >= 0 && line_starts_with(line, line_len, "a=extmap:")) {
            /* a=extmap:<id>[/<direction>] <URI> [<extension-attributes>] (RFC 8285 §5).
             * We only recognise the TWCC URI; other extensions are ignored so
             * they do not collide with our negotiated ID. */
            const char *p = line + 9; /* skip "a=extmap:" */
            const char *end = line + line_len;
            const char *next;
            uint32_t ext_id = parse_u32(p, end, &next);
            if (ext_id >= 1 && ext_id <= 14) {
                p = next;
                if (p < end && *p == '/') {
                    p++;
                    while (p < end && *p != ' ' && *p != '\t') {
                        p++;
                    }
                }
                while (p < end && (*p == ' ' || *p == '\t')) {
                    p++;
                }
                const char *uri = NANORTC_TWCC_EXT_URI;
                size_t uri_len = 0;
                while (uri[uri_len]) {
                    uri_len++;
                }
                size_t remain = (size_t)(end - p);
                if (remain >= uri_len && memcmp(p, uri, uri_len) == 0 &&
                    (remain == uri_len || p[uri_len] == ' ' || p[uri_len] == '\t' ||
                     p[uri_len] == '\r' || p[uri_len] == '\n')) {
                    sdp->mlines[current_mline_idx].twcc_ext_id = (uint8_t)ext_id;
                }
            }
        } else if (current_mline_idx >= 0 && (line_starts_with(line, line_len, "a=sendrecv") ||
                                              line_starts_with(line, line_len, "a=sendonly") ||
                                              line_starts_with(line, line_len, "a=recvonly") ||
                                              line_starts_with(line, line_len, "a=inactive"))) {
            nanortc_direction_t dir = NANORTC_DIR_SENDRECV;
            if (line_starts_with(line, line_len, "a=sendonly")) {
                dir = NANORTC_DIR_SENDONLY;
            } else if (line_starts_with(line, line_len, "a=recvonly")) {
                dir = NANORTC_DIR_RECVONLY;
            } else if (line_starts_with(line, line_len, "a=inactive")) {
                dir = NANORTC_DIR_INACTIVE;
            }
            sdp->mlines[current_mline_idx].remote_direction = dir;
        } else
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */
            if (line_starts_with(line, line_len, "a=ice-ufrag:")) {
                int rc = sdp_store_attribute(line + 12, line_len - 12, sdp->remote_ufrag,
                                             sizeof(sdp->remote_ufrag));
                if (rc != NANORTC_OK)
                    return rc;
            } else if (line_starts_with(line, line_len, "a=ice-pwd:")) {
                int rc = sdp_store_attribute(line + 10, line_len - 10, sdp->remote_pwd,
                                             sizeof(sdp->remote_pwd));
                if (rc != NANORTC_OK)
                    return rc;
            } else if (line_starts_with(line, line_len, "a=fingerprint:")) {
                uint8_t fingerprint[NANORTC_DTLS_FINGERPRINT_SIZE];
                int rc = sdp_parse_fingerprint(line + 14, line_len - 14, fingerprint);
                if (rc != NANORTC_OK)
                    return rc;
                if (sdp->has_remote_fingerprint &&
                    memcmp(sdp->remote_fingerprint, fingerprint, sizeof(fingerprint)) != 0)
                    return NANORTC_ERR_PARSE;
                memcpy(sdp->remote_fingerprint, fingerprint, sizeof(fingerprint));
                sdp->has_remote_fingerprint = true;
            } else if (line_starts_with(line, line_len, "a=max-message-size:")) {
                const char *v = line + 19;
                const char *end = line + line_len;
                while (end > v &&
                       (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
                    end--;
                uint32_t limit = 0;
                if (v == end)
                    return NANORTC_ERR_PARSE;
                for (; v < end; v++) {
                    if (*v < '0' || *v > '9' || limit > (UINT32_MAX - (uint32_t)(*v - '0')) / 10u)
                        return NANORTC_ERR_PARSE;
                    limit = limit * 10u + (uint32_t)(*v - '0');
                }
                sdp->remote_max_message_size = limit;
            } else if (line_starts_with(line, line_len, "a=sctp-port:")) {
                const char *val = line + 12;
                sdp->remote_sctp_port = (uint16_t)parse_u32(val, sdp_str + eol, NULL);
            } else if (line_starts_with(line, line_len, "a=setup:")) {
                char setup_str[16];
                extract_value(line, line_len, "a=setup:", setup_str, sizeof(setup_str));
                if (memcmp(setup_str, "active", 6) == 0) {
                    sdp->remote_setup = NANORTC_SDP_SETUP_ACTIVE;
                } else if (memcmp(setup_str, "passive", 7) == 0) {
                    sdp->remote_setup = NANORTC_SDP_SETUP_PASSIVE;
                } else {
                    sdp->remote_setup = NANORTC_SDP_SETUP_ACTPASS;
                }
            } else if (line_starts_with(line, line_len, "a=candidate:")) {
                nano_sdp_candidate_t candidate;
                int rc = sdp_parse_candidate(line, line_len, &candidate);
                if (rc != NANORTC_ERR_NOT_IMPLEMENTED) {
                    if (rc != NANORTC_OK)
                        return rc;
                    if (sdp->candidate_count >= NANORTC_SDP_MAX_CANDIDATES)
                        return NANORTC_ERR_BUFFER_TOO_SMALL;
                    sdp->remote_candidates[sdp->candidate_count++] = candidate;
                }
            } else if (line_starts_with(line, line_len, "a=end-of-candidates")) {
                sdp->end_of_candidates = true;
            }
#if !NANORTC_HAVE_MEDIA_TRANSPORT
            /* When media transport is disabled, DC m-lines still need tracking */
            else if (line_starts_with(line, line_len, "m=application ")) {
                sdp->has_datachannel = true;
                sdp->dc_mid = sdp->mid_count;
                sdp->mid_count++;
            } else if (line_len >= 2 && line[0] == 'm' && line[1] == '=') {
                sdp->mid_count++;
            }
#endif

        pos = eol;
    }

    /* Validate required fields */
    if (sdp->remote_ufrag[0] == '\0' || sdp->remote_pwd[0] == '\0') {
        return NANORTC_ERR_PARSE;
    }

#if NANORTC_HAVE_MEDIA_TRANSPORT && NANORTC_FEATURE_H265
    if (h265_reject) {
        return NANORTC_ERR_PARSE;
    }
#endif

    sdp->parsed = true;
    return NANORTC_OK;
}

/* ================================================================
 * Generator — m-line helpers
 * ================================================================ */

/** Append ICE/DTLS transport attributes shared by all m-lines (BUNDLE). */
static bool sdp_append_transport_attrs(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos)
{
    if (!sdp_append(buf, buf_len, pos, "a=ice-ufrag:"))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp->local_ufrag))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=ice-pwd:"))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp->local_pwd))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (sdp->local_fingerprint[0] != '\0') {
        if (!sdp_append(buf, buf_len, pos, "a=fingerprint:"))
            return false;
        if (!sdp_append(buf, buf_len, pos, sdp->local_fingerprint))
            return false;
        if (!sdp_append(buf, buf_len, pos, "\r\n"))
            return false;
    }
    {
        const char *s = "a=setup:passive\r\n";
        if (sdp->local_setup == NANORTC_SDP_SETUP_ACTIVE) {
            s = "a=setup:active\r\n";
        } else if (sdp->local_setup == NANORTC_SDP_SETUP_ACTPASS) {
            s = "a=setup:actpass\r\n";
        }
        if (!sdp_append(buf, buf_len, pos, s))
            return false;
    }
    return true;
}

/** Append all local host ICE candidates (RFC 8839 §5.1). */
static bool sdp_append_host_candidates(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos)
{
    for (uint8_t i = 0; i < sdp->local_candidate_count; i++) {
        const nano_sdp_candidate_t *candidate = &sdp->local_candidates[i];
        if (!candidate->family)
            continue;
        char address[NANORTC_IPV6_STR_SIZE];
        size_t address_len;
        if (addr_format(candidate->addr, candidate->family, address, sizeof(address),
                        &address_len) != NANORTC_OK)
            return false;
        /* Foundation = index + 1 (RFC 8445 §5.1.1.3) */
        if (!sdp_append(buf, buf_len, pos, "a=candidate:"))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)(i + 1)))
            return false;
        if (!sdp_append(buf, buf_len, pos, " 1 UDP "))
            return false;
        uint32_t prio = candidate->priority ? candidate->priority : ICE_HOST_PRIORITY(i);
        if (!sdp_append_u32(buf, buf_len, pos, prio))
            return false;
        if (!sdp_append(buf, buf_len, pos, " "))
            return false;
        if (!sdp_append(buf, buf_len, pos, address))
            return false;
        if (!sdp_append(buf, buf_len, pos, " "))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, sdp->local_candidates[i].port))
            return false;
        if (!sdp_append(buf, buf_len, pos, " typ host\r\n"))
            return false;
    }
    return true;
}

/* RFC 8839 §5.1 / §9.1: srflx/relay require related fields even without a
 * host candidate. Hide them with a same-family zero address and port 9. */
static bool sdp_append_related_address(char *buf, size_t buf_len, size_t *pos, const char *ip)
{
    size_t len = nanortc_strnlen(ip, NANORTC_IPV6_STR_SIZE);
    return sdp_append(buf, buf_len, pos,
                      memchr(ip, ':', len) ? " raddr :: rport 9" : " raddr 0.0.0.0 rport 9");
}

/** Append server-reflexive ICE candidate (RFC 8839 §5.1).
 *  Priority matches what the ICE layer emits in the STUN PRIORITY attribute
 *  via ICE_SRFLX_PRIORITY(idx) (RFC 8445 §5.1.2.1: type_pref=100). The srflx
 *  candidate occupies the local_candidates[] slot right after all host
 *  candidates, so its idx == local_candidate_count. */
static bool sdp_append_srflx_candidate(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos)
{
    if (!sdp->has_srflx_candidate || sdp->srflx_candidate_ip[0] == '\0')
        return true;
    if (!sdp_append(buf, buf_len, pos, "a=candidate:3 1 UDP "))
        return false;
    if (!sdp_append_u32(buf, buf_len, pos, ICE_SRFLX_PRIORITY(sdp->local_candidate_count)))
        return false;
    if (!sdp_append(buf, buf_len, pos, " "))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp->srflx_candidate_ip))
        return false;
    if (!sdp_append(buf, buf_len, pos, " "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, sdp->srflx_candidate_port))
        return false;
    if (!sdp_append(buf, buf_len, pos, " typ srflx"))
        return false;
    if (!sdp_append_related_address(buf, buf_len, pos, sdp->srflx_candidate_ip))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    return true;
}

/** Append relay ICE candidate from TURN allocation (RFC 8839 §5.1).
 *  Priority matches the ICE layer's STUN PRIORITY attribute via
 *  ICE_RELAY_PRIORITY(idx) (RFC 8445 §5.1.2.1: type_pref=0). A relay
 *  candidate is registered in local_candidates[] alongside the srflx, so
 *  idx == local_candidate_count + (has_srflx ? 1 : 0). */
static bool sdp_append_relay_candidate(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos)
{
    if (!sdp->has_relay_candidate || sdp->relay_candidate_ip[0] == '\0')
        return true;
    uint8_t relay_idx = (uint8_t)(sdp->local_candidate_count + (sdp->has_srflx_candidate ? 1 : 0));
    if (!sdp_append(buf, buf_len, pos, "a=candidate:2 1 UDP "))
        return false;
    if (!sdp_append_u32(buf, buf_len, pos, ICE_RELAY_PRIORITY(relay_idx)))
        return false;
    if (!sdp_append(buf, buf_len, pos, " "))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp->relay_candidate_ip))
        return false;
    if (!sdp_append(buf, buf_len, pos, " "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, sdp->relay_candidate_port))
        return false;
    if (!sdp_append(buf, buf_len, pos, " typ relay"))
        return false;
    if (!sdp_append_related_address(buf, buf_len, pos, sdp->relay_candidate_ip))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    return true;
}

/** Return whether the first local candidate uses an IPv6 address. */
static bool sdp_local_is_ipv6(const nano_sdp_t *sdp)
{
#if NANORTC_FEATURE_IPV6
    return sdp->local_candidate_count > 0 && sdp->local_candidates[0].family == 6;
#else
    (void)sdp;
#endif
    return false;
}

/** Return the SDP connection line based on local candidate address family. */
static const char *sdp_connection_line(const nano_sdp_t *sdp)
{
    return sdp_local_is_ipv6(sdp) ? "c=IN IP6 ::\r\n" : "c=IN IP4 0.0.0.0\r\n";
}

/** Return the SDP origin line based on local candidate address family. */
static const char *sdp_origin_line(const nano_sdp_t *sdp)
{
    return sdp_local_is_ipv6(sdp) ? "o=- 1 1 IN IP6 ::\r\n" : "o=- 1 1 IN IP4 0.0.0.0\r\n";
}

/** Append DataChannel (application) m-line block. */
static bool sdp_append_datachannel_mline(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos,
                                         uint8_t mid)
{
    if (!sdp_append(buf, buf_len, pos, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp_connection_line(sdp)))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=mid:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, mid))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=sendrecv\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=ice-options:trickle\r\n"))
        return false;
    if (!sdp_append_transport_attrs(sdp, buf, buf_len, pos))
        return false;
#if NANORTC_FEATURE_DATACHANNEL
    if (!sdp_append(buf, buf_len, pos, "a=sctp-port:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, sdp->local_sctp_port))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=max-message-size:") ||
        !sdp_append_u16(buf, buf_len, pos, NANORTC_SCTP_MAX_MESSAGE_SIZE) ||
        !sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
#endif
    return true;
}

#if NANORTC_HAVE_MEDIA_TRANSPORT

/** Append a direction attribute line. */
static bool sdp_append_direction(char *buf, size_t buf_len, size_t *pos, nanortc_direction_t dir)
{
    const char *dir_str = "a=sendrecv\r\n";
    if (dir == NANORTC_DIR_SENDONLY) {
        dir_str = "a=sendonly\r\n";
    } else if (dir == NANORTC_DIR_RECVONLY) {
        dir_str = "a=recvonly\r\n";
    } else if (dir == NANORTC_DIR_INACTIVE) {
        dir_str = "a=inactive\r\n";
    }
    return sdp_append(buf, buf_len, pos, dir_str);
}

/** Append audio m-line block. Reads PT/direction/sample_rate from the mline entry. */
static bool sdp_append_audio_mline(nano_sdp_t *sdp, nano_sdp_mline_t *ml, char *buf, size_t buf_len,
                                   size_t *pos, uint8_t mid)
{
    if (!sdp_append(buf, buf_len, pos, "m=audio 9 UDP/TLS/RTP/SAVPF "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp_connection_line(sdp)))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=mid:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, mid))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append_direction(buf, buf_len, pos, ml->direction))
        return false;
    if (!sdp_append_transport_attrs(sdp, buf, buf_len, pos))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=rtcp-mux\r\n"))
        return false;
    /* a=rtpmap */
    if (!sdp_append(buf, buf_len, pos, "a=rtpmap:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;
    {
        const char *codec_str = " opus/48000/2";
        if (ml->pt == 0) {
            codec_str = " PCMU/8000";
        } else if (ml->pt == 8) {
            codec_str = " PCMA/8000";
        }
        if (!sdp_append(buf, buf_len, pos, codec_str))
            return false;
    }
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    /* a=fmtp (Opus decoder hints, RFC 7587 §6.1) */
    if (ml->sample_rate == 48000) {
        if (!sdp_append(buf, buf_len, pos, "a=fmtp:"))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
            return false;
        if (!sdp_append(buf, buf_len, pos,
                        ml->channels >= 2 ? " minptime=10;useinbandfec=1;stereo=1\r\n"
                                          : " minptime=10;useinbandfec=1;stereo=0\r\n"))
            return false;
        if (!sdp_append(buf, buf_len, pos, "a=ptime:20\r\n"))
            return false;
    }
    return true;
}

/** Append video m-line block.
 *
 *  H.264 — RFC 6184 §8.1: profile-level-id 42e01f (Constrained Baseline 3.1),
 *  packetization-mode=1 (STAP-A + FU-A + single NAL).
 *
 *  H.265 — RFC 7798 §7.1: profile-id=1 (Main), tier-flag=0, level-id=93
 *  (Main Level 3.1), tx-mode=SRST. Optional sprop-vps/sprop-sps/sprop-pps
 *  appended when the caller has provided out-of-band parameter sets via
 *  nanortc_video_set_h265_parameter_sets().
 */
static bool sdp_append_video_mline(nano_sdp_t *sdp, nano_sdp_mline_t *ml, char *buf, size_t buf_len,
                                   size_t *pos, uint8_t mid)
{
    if (!sdp_append(buf, buf_len, pos, "m=video 9 UDP/TLS/RTP/SAVPF "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp_connection_line(sdp)))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=mid:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, mid))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append_direction(buf, buf_len, pos, ml->direction))
        return false;
    if (!sdp_append_transport_attrs(sdp, buf, buf_len, pos))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=rtcp-mux\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=rtpmap:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;

#if NANORTC_FEATURE_H265
    if (ml->codec == NANORTC_CODEC_H265) {
        if (!sdp_append(buf, buf_len, pos, " H265/90000\r\n"))
            return false;
        if (!sdp_append(buf, buf_len, pos, "a=fmtp:"))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
            return false;
        /* Profile-tier-level: prefer values extracted from the VPS so the
         * SDP matches the actual stream. Safari's HEVC decoder drops frames
         * silently when the SDP level-id understates the stream level. The
         * defaults (Main profile / Main tier / Level 3.1) remain for callers
         * who never invoke nanortc_video_set_h265_parameter_sets(). */
        uint8_t pid = ml->h265_profile_id ? ml->h265_profile_id : 1;
        uint8_t tier = ml->h265_tier_flag;
        uint8_t level = ml->h265_level_id ? ml->h265_level_id : 93;
        if (!sdp_append(buf, buf_len, pos, " profile-id="))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, pid))
            return false;
        if (!sdp_append(buf, buf_len, pos, ";tier-flag="))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, tier))
            return false;
        if (!sdp_append(buf, buf_len, pos, ";level-id="))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, level))
            return false;
        if (!sdp_append(buf, buf_len, pos, ";tx-mode=SRST"))
            return false;
        if (ml->h265_sprop_fmtp_len > 0) {
            size_t frag_len = (size_t)ml->h265_sprop_fmtp_len;
            if (*pos + frag_len + 1 >= buf_len) {
                return false;
            }
            buf[(*pos)++] = ';';
            memcpy(buf + *pos, ml->h265_sprop_fmtp, frag_len);
            *pos += frag_len;
            buf[*pos] = '\0';
        }
        if (!sdp_append(buf, buf_len, pos, "\r\n"))
            return false;
    } else
#endif /* NANORTC_FEATURE_H265 */
    {
        if (!sdp_append(buf, buf_len, pos, " H264/90000\r\n"))
            return false;
        if (!sdp_append(buf, buf_len, pos, "a=fmtp:"))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
            return false;
        if (!sdp_append(
                buf, buf_len, pos,
                " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"))
            return false;
    }

    if (!sdp_append(buf, buf_len, pos, "a=rtcp-fb:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, " nack pli\r\n"))
        return false;

    /* Transport-wide CC rtcp-fb (draft-holmer-rmcat-transport-wide-cc-extensions-01
     * §4). Required in addition to the extmap below: per libwebrtc, a receiver
     * only emits transport-cc RTCP feedback when this rtcp-fb attribute is
     * negotiated. Without it Chrome sends no congestion feedback and our BWE
     * stays at its seed (no encoder rate adaptation). */
    if (!sdp_append(buf, buf_len, pos, "a=rtcp-fb:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, " transport-cc\r\n"))
        return false;

    /* Transport-wide CC extmap (RFC 8285 / draft-holmer-rmcat-twcc-01).
     * When answering, ml->twcc_ext_id is the offerer's ID (parsed from
     * the remote SDP). When offering, it starts at 0 — fall back to the
     * compile-time preferred ID so the offer actually advertises TWCC. */
    uint8_t twcc_id = ml->twcc_ext_id ? ml->twcc_ext_id : (uint8_t)NANORTC_TWCC_EXT_ID;
    if (twcc_id >= 1 && twcc_id <= 14) {
        if (!sdp_append(buf, buf_len, pos, "a=extmap:"))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, twcc_id))
            return false;
        if (!sdp_append(buf, buf_len, pos, " " NANORTC_TWCC_EXT_URI "\r\n"))
            return false;
        ml->twcc_ext_id = twcc_id; /* persist for RTP-side wiring */
    }
    return true;
}
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

/* ================================================================
 * Generator
 * ================================================================ */

int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *out_len)
{
    if (!sdp || !buf || !out_len || buf_len < NANORTC_SDP_MIN_BUF_SIZE) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    size_t pos = 0;

    /* Session-level */
    if (!sdp_append(buf, buf_len, &pos, "v=0\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, sdp_origin_line(sdp)))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "s=-\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "t=0 0\r\n"))
        goto overflow;

#if NANORTC_HAVE_MEDIA_TRANSPORT
    {
        /* Build m-line table: media mlines + optional DC, sorted by MID.
         * Max entries = NANORTC_MAX_MEDIA_TRACKS + 1 (for DC). */
        struct {
            int type; /* SDP_MLINE_APPLICATION, SDP_MLINE_AUDIO, SDP_MLINE_VIDEO */
            uint8_t mid;
            uint8_t ml_idx; /* index into sdp->mlines[] (only for media) */
        } entries[NANORTC_MAX_MEDIA_TRACKS + 1];
        uint8_t n_entries = 0;

        /* For answer (parsed offer with mid_count > 0) or fresh offer */
        for (uint8_t i = 0; i < sdp->mline_count; i++) {
            if (sdp->mlines[i].active) {
                entries[n_entries].type = sdp->mlines[i].kind;
                entries[n_entries].mid = sdp->mlines[i].mid;
                entries[n_entries].ml_idx = i;
                n_entries++;
            }
        }
        if (sdp->has_datachannel) {
            entries[n_entries].type = SDP_MLINE_APPLICATION;
            entries[n_entries].mid = sdp->dc_mid;
            entries[n_entries].ml_idx = 0; /* unused for DC */
            n_entries++;
        }

        /* Simple bubble sort by MID (at most MAX_MEDIA_TRACKS+1 elements) */
        for (int i = 0; i < n_entries - 1; i++) {
            for (int j = i + 1; j < n_entries; j++) {
                if (entries[j].mid < entries[i].mid) {
                    int tmp_type = entries[i].type;
                    uint8_t tmp_mid = entries[i].mid;
                    uint8_t tmp_idx = entries[i].ml_idx;
                    entries[i].type = entries[j].type;
                    entries[i].mid = entries[j].mid;
                    entries[i].ml_idx = entries[j].ml_idx;
                    entries[j].type = tmp_type;
                    entries[j].mid = tmp_mid;
                    entries[j].ml_idx = tmp_idx;
                }
            }
        }

        /* Write BUNDLE group line */
        if (!sdp_append(buf, buf_len, &pos, "a=group:BUNDLE"))
            goto overflow;
        for (int i = 0; i < n_entries; i++) {
            if (!sdp_append(buf, buf_len, &pos, " "))
                goto overflow;
            if (!sdp_append_u16(buf, buf_len, &pos, entries[i].mid))
                goto overflow;
        }
        if (!sdp_append(buf, buf_len, &pos, "\r\n"))
            goto overflow;

        /* Write m-lines in order. ICE candidate on first m-line (BUNDLE anchor). */
        for (int i = 0; i < n_entries; i++) {
            switch (entries[i].type) {
            case SDP_MLINE_APPLICATION:
                if (!sdp_append_datachannel_mline(sdp, buf, buf_len, &pos, entries[i].mid))
                    goto overflow;
                break;
            case SDP_MLINE_AUDIO:
                if (!sdp_append_audio_mline(sdp, &sdp->mlines[entries[i].ml_idx], buf, buf_len,
                                            &pos, entries[i].mid))
                    goto overflow;
                break;
            case SDP_MLINE_VIDEO:
                if (!sdp_append_video_mline(sdp, &sdp->mlines[entries[i].ml_idx], buf, buf_len,
                                            &pos, entries[i].mid))
                    goto overflow;
                break;
            }
            if (i == 0) {
                if (!sdp_append_host_candidates(sdp, buf, buf_len, &pos))
                    goto overflow;
                if (!sdp_append_srflx_candidate(sdp, buf, buf_len, &pos))
                    goto overflow;
                if (!sdp_append_relay_candidate(sdp, buf, buf_len, &pos))
                    goto overflow;
            }
        }
    }
#else
    if (sdp->has_datachannel) {
        if (!sdp_append(buf, buf_len, &pos, "a=group:BUNDLE 0\r\n"))
            goto overflow;
        if (!sdp_append_datachannel_mline(sdp, buf, buf_len, &pos, 0))
            goto overflow;
    } else if (!sdp_append_transport_attrs(sdp, buf, buf_len, &pos)) {
        goto overflow;
    }
    if (!sdp_append_host_candidates(sdp, buf, buf_len, &pos))
        goto overflow;
    if (!sdp_append_srflx_candidate(sdp, buf, buf_len, &pos))
        goto overflow;
    if (!sdp_append_relay_candidate(sdp, buf, buf_len, &pos))
        goto overflow;
#endif

    *out_len = pos;
    return NANORTC_OK;

overflow:
    return NANORTC_ERR_BUFFER_TOO_SMALL;
}

#if NANORTC_HAVE_MEDIA_TRANSPORT

nano_sdp_mline_t *sdp_find_mline(nano_sdp_t *sdp, uint8_t mid)
{
    if (!sdp) {
        return NULL;
    }
    for (uint8_t i = 0; i < sdp->mline_count; i++) {
        if (sdp->mlines[i].active && sdp->mlines[i].mid == mid) {
            return &sdp->mlines[i];
        }
    }
    return NULL;
}

int sdp_add_mline(nano_sdp_t *sdp, uint8_t kind, uint8_t codec, uint8_t pt, uint32_t sample_rate,
                  uint8_t channels, nanortc_direction_t direction)
{
    if (!sdp) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (sdp->mline_count >= NANORTC_MAX_MEDIA_TRACKS) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    nano_sdp_mline_t *ml = &sdp->mlines[sdp->mline_count];
    memset(ml, 0, sizeof(*ml));
    ml->kind = kind;
    ml->mid = sdp->mid_count;
    ml->pt = pt;
    ml->codec = codec;
    ml->sample_rate = sample_rate;
    ml->channels = channels;
    ml->direction = direction;
    ml->active = true;

    sdp->mid_count++;
    sdp->mline_count++;
    return (int)ml->mid;
}

#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */
