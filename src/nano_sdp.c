/*
 * nanortc — SDP parser/generator (RFC 8866, RFC 8829)
 *
 * Sans I/O: no stdio.h. Uses memcpy + manual itoa for string building.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_sdp.h"
#include "nano_log.h"
#include "nanortc.h"
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

    if (vlen >= dst_size) {
        vlen = dst_size - 1;
    }
    memcpy(dst, val, vlen);
    dst[vlen] = '\0';
}

#if NANORTC_HAVE_MEDIA_TRANSPORT

/* m-line type tracking for multi m-line SDP */
#define SDP_MLINE_NONE        0
#define SDP_MLINE_APPLICATION 1
#define SDP_MLINE_AUDIO       2

/**
 * Parse a=rtpmap:<pt> <codec>/<rate>[/<channels>]
 * Example: a=rtpmap:111 opus/48000/2
 */
static void parse_rtpmap(nano_sdp_t *sdp, const char *line, size_t line_len)
{
    const char *p = line + 9; /* skip "a=rtpmap:" (9 chars) */
    const char *end = line + line_len;

    /* Parse payload type */
    const char *next;
    uint32_t pt = parse_u32(p, end, &next);
    if (next == p || next >= end || *next != ' ') {
        return;
    }
    p = next + 1; /* skip space */

    sdp->audio_pt = (uint8_t)pt;

    /* Skip codec name, find '/' */
    while (p < end && *p != '/') {
        p++;
    }
    if (p >= end) {
        return;
    }
    p++; /* skip '/' */

    /* Parse sample rate */
    sdp->audio_sample_rate = parse_u32(p, end, &next);
    p = next;

    /* Parse channels (optional) */
    if (p < end && *p == '/') {
        p++;
        sdp->audio_channels = (uint8_t)parse_u32(p, end, NULL);
    }
}

/**
 * Parse m=audio line and extract first payload type.
 * Format: m=audio <port> <proto> <pt> [<pt2> ...]
 */
static void parse_audio_mline(nano_sdp_t *sdp, const char *line, size_t line_len)
{
    const char *p = line + 8; /* skip "m=audio " */
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
        sdp->audio_pt = (uint8_t)parse_u32(p, end, NULL);
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

int sdp_parse(nano_sdp_t *sdp, const char *sdp_str, size_t len)
{
    if (!sdp || !sdp_str || len == 0) {
        return NANORTC_ERR_INVALID_PARAM;
    }

#if NANORTC_HAVE_MEDIA_TRANSPORT
    int current_mline = SDP_MLINE_NONE;
#endif

    size_t pos = 0;
    while (pos < len) {
        size_t eol = find_eol(sdp_str, len, pos);
        const char *line = sdp_str + pos;
        size_t line_len = eol - pos;

        /*
         * Unified line dispatch: m-lines first, then attributes.
         * WebRTC BUNDLE: ICE/DTLS attributes are shared across m-lines,
         * so we parse them regardless of current_mline context.
         * Media-specific attributes (a=rtpmap) use current_mline context.
         */

#if NANORTC_HAVE_MEDIA_TRANSPORT
        if (line_starts_with(line, line_len, "m=application ")) {
            current_mline = SDP_MLINE_APPLICATION;
        } else if (line_starts_with(line, line_len, "m=audio ")) {
            current_mline = SDP_MLINE_AUDIO;
            sdp->has_audio = true;
            parse_audio_mline(sdp, line, line_len);
        } else if (line_len >= 2 && line[0] == 'm' && line[1] == '=') {
            current_mline = SDP_MLINE_NONE;
        } else if (current_mline == SDP_MLINE_AUDIO &&
                   line_starts_with(line, line_len, "a=rtpmap:")) {
            parse_rtpmap(sdp, line, line_len);
        } else
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */
            if (line_starts_with(line, line_len, "a=ice-ufrag:")) {
                extract_value(line, line_len, "a=ice-ufrag:", sdp->remote_ufrag,
                              sizeof(sdp->remote_ufrag));
            } else if (line_starts_with(line, line_len, "a=ice-pwd:")) {
                extract_value(line, line_len, "a=ice-pwd:", sdp->remote_pwd,
                              sizeof(sdp->remote_pwd));
            } else if (line_starts_with(line, line_len, "a=fingerprint:")) {
                extract_value(line, line_len, "a=fingerprint:", sdp->remote_fingerprint,
                              sizeof(sdp->remote_fingerprint));
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
                /* Parse ICE candidate (RFC 8839 §5.1):
                 * a=candidate:<foundation> <component> <transport> <priority> <addr> <port> ...
                 * Fields are 1-indexed after prefix. We need field 5 (addr) and field 6 (port). */
                if (sdp->candidate_count < NANORTC_SDP_MAX_CANDIDATES) {
                    const char *p = line + 12; /* skip "a=candidate:" */
                    const char *line_end = line + line_len;
                    int field = 1;

                    /* Skip to field 5 (addr) */
                    while (p < line_end && field < 5) {
                        if (*p == ' ') {
                            field++;
                            while (p < line_end && *p == ' ')
                                p++;
                        } else {
                            p++;
                        }
                    }

                    /* Extract addr */
                    const char *addr_start = p;
                    while (p < line_end && *p != ' ')
                        p++;
                    size_t addr_len = (size_t)(p - addr_start);

                    /* Skip to port field */
                    while (p < line_end && *p == ' ')
                        p++;

                    /* Parse port */
                    uint16_t cand_port = (uint16_t)parse_u32(p, line_end, NULL);

                    if (addr_len > 0 && addr_len < NANORTC_IPV6_STR_SIZE && cand_port > 0) {
                        nano_sdp_candidate_t *c = &sdp->remote_candidates[sdp->candidate_count];
                        memcpy(c->addr, addr_start, addr_len);
                        c->addr[addr_len] = '\0';
                        c->port = cand_port;
                        sdp->candidate_count++;
                        NANORTC_LOGD("SDP", "parsed candidate from SDP");
                    }
                }
            }

        pos = eol;
    }

    /* Validate required fields */
    if (sdp->remote_ufrag[0] == '\0' || sdp->remote_pwd[0] == '\0') {
        NANORTC_LOGW("SDP", "missing ice-ufrag or ice-pwd");
        return NANORTC_ERR_PARSE;
    }

    sdp->parsed = true;
    NANORTC_LOGI("SDP", "offer parsed");
    return NANORTC_OK;
}

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
    if (!sdp_append(buf, buf_len, &pos, "o=- 1 1 IN IP4 0.0.0.0\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "s=-\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "t=0 0\r\n"))
        goto overflow;

    /* BUNDLE group: include all active MIDs */
    {
        const char *bundle = "a=group:BUNDLE 0\r\n";
#if NANORTC_HAVE_MEDIA_TRANSPORT
        if (sdp->has_audio) {
            bundle = "a=group:BUNDLE 0 1\r\n";
        }
#endif
        if (!sdp_append(buf, buf_len, &pos, bundle))
            goto overflow;
    }

    /* First m-line: DataChannel or DTLS-only bundle anchor (MID=0) */
    if (!sdp_append(buf, buf_len, &pos, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "c=IN IP4 0.0.0.0\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "a=mid:0\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "a=sendrecv\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "a=ice-options:trickle\r\n"))
        goto overflow;

    /* ICE credentials */
    if (!sdp_append(buf, buf_len, &pos, "a=ice-ufrag:"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, sdp->local_ufrag))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "\r\n"))
        goto overflow;

    if (!sdp_append(buf, buf_len, &pos, "a=ice-pwd:"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, sdp->local_pwd))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "\r\n"))
        goto overflow;

    /* Fingerprint */
    if (sdp->local_fingerprint[0] != '\0') {
        if (!sdp_append(buf, buf_len, &pos, "a=fingerprint:"))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, sdp->local_fingerprint))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, "\r\n"))
            goto overflow;
    }

    /* Setup role */
    {
        const char *setup_str = "a=setup:passive\r\n";
        if (sdp->local_setup == NANORTC_SDP_SETUP_ACTIVE) {
            setup_str = "a=setup:active\r\n";
        } else if (sdp->local_setup == NANORTC_SDP_SETUP_ACTPASS) {
            setup_str = "a=setup:actpass\r\n";
        }
        if (!sdp_append(buf, buf_len, &pos, setup_str))
            goto overflow;
    }

#if NANORTC_FEATURE_DATACHANNEL
    /* SCTP port */
    if (!sdp_append(buf, buf_len, &pos, "a=sctp-port:"))
        goto overflow;
    if (!sdp_append_u16(buf, buf_len, &pos, sdp->local_sctp_port))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "\r\n"))
        goto overflow;

    /* Max message size */
    if (!sdp_append(buf, buf_len, &pos, "a=max-message-size:262144\r\n"))
        goto overflow;
#endif

    /* Local ICE candidate (RFC 8839 §5.1) */
    if (sdp->has_local_candidate && sdp->local_candidate_ip[0] != '\0') {
        if (!sdp_append(buf, buf_len, &pos, "a=candidate:1 1 UDP 2122252543 "))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, sdp->local_candidate_ip))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, " "))
            goto overflow;
        if (!sdp_append_u16(buf, buf_len, &pos, sdp->local_candidate_port))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, " typ host\r\n"))
            goto overflow;
    }

#if NANORTC_HAVE_MEDIA_TRANSPORT
    /* Audio m-line (MID=1) */
    if (sdp->has_audio) {
        if (!sdp_append(buf, buf_len, &pos, "m=audio 9 UDP/TLS/RTP/SAVPF "))
            goto overflow;
        if (!sdp_append_u16(buf, buf_len, &pos, (uint16_t)sdp->audio_pt))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, "\r\n"))
            goto overflow;

        if (!sdp_append(buf, buf_len, &pos, "c=IN IP4 0.0.0.0\r\n"))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, "a=mid:1\r\n"))
            goto overflow;
        if (!sdp_append(buf, buf_len, &pos, "a=sendrecv\r\n"))
            goto overflow;

        /* a=rtpmap:<pt> <codec>/<rate>/<channels> */
        if (!sdp_append(buf, buf_len, &pos, "a=rtpmap:"))
            goto overflow;
        if (!sdp_append_u16(buf, buf_len, &pos, (uint16_t)sdp->audio_pt))
            goto overflow;

        /* Determine codec name from sample rate/channels */
        {
            const char *codec_str = " opus/48000/2";
            if (sdp->audio_sample_rate == 8000 && sdp->audio_channels <= 1) {
                codec_str = " PCMU/8000";
            }
            if (!sdp_append(buf, buf_len, &pos, codec_str))
                goto overflow;
        }
        if (!sdp_append(buf, buf_len, &pos, "\r\n"))
            goto overflow;
    }
#endif /* NANORTC_HAVE_MEDIA_TRANSPORT */

    *out_len = pos;
    NANORTC_LOGD("SDP", "answer generated");
    return NANORTC_OK;

overflow:
    NANORTC_LOGE("SDP", "buffer overflow generating answer");
    return NANORTC_ERR_BUFFER_TOO_SMALL;
}
