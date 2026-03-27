/*
 * nanortc — SDP parser/generator (RFC 8866, RFC 8829)
 *
 * Sans I/O: no stdio.h. Uses memcpy + manual itoa for string building.
 * Reference: libpeer src/sdp.c (attribute format).
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
        /* Reverse digits */
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

int sdp_init(nano_sdp_t *sdp)
{
    if (!sdp) {
        return NANO_ERR_INVALID_PARAM;
    }
    memset(sdp, 0, sizeof(*sdp));
    sdp->local_sctp_port = 5000;
    sdp->local_setup = NANO_SDP_SETUP_PASSIVE; /* answerer default */
    return NANO_OK;
}

int sdp_parse(nano_sdp_t *sdp, const char *sdp_str, size_t len)
{
    if (!sdp || !sdp_str || len == 0) {
        return NANO_ERR_INVALID_PARAM;
    }

    size_t pos = 0;
    while (pos < len) {
        size_t eol = find_eol(sdp_str, len, pos);
        const char *line = sdp_str + pos;
        size_t line_len = eol - pos;

        if (line_starts_with(line, line_len, "a=ice-ufrag:")) {
            extract_value(line, line_len, "a=ice-ufrag:", sdp->remote_ufrag,
                          sizeof(sdp->remote_ufrag));
        } else if (line_starts_with(line, line_len, "a=ice-pwd:")) {
            extract_value(line, line_len, "a=ice-pwd:", sdp->remote_pwd, sizeof(sdp->remote_pwd));
        } else if (line_starts_with(line, line_len, "a=fingerprint:")) {
            extract_value(line, line_len, "a=fingerprint:", sdp->remote_fingerprint,
                          sizeof(sdp->remote_fingerprint));
        } else if (line_starts_with(line, line_len, "a=sctp-port:")) {
            /* Parse sctp port number */
            const char *val = line + 12;
            uint16_t port = 0;
            while (val < sdp_str + eol && *val >= '0' && *val <= '9') {
                port = port * 10 + (*val - '0');
                val++;
            }
            if (port > 0) {
                sdp->remote_sctp_port = port;
            }
        } else if (line_starts_with(line, line_len, "a=setup:")) {
            char setup_str[16];
            extract_value(line, line_len, "a=setup:", setup_str, sizeof(setup_str));
            if (memcmp(setup_str, "active", 6) == 0) {
                sdp->remote_setup = NANO_SDP_SETUP_ACTIVE;
            } else if (memcmp(setup_str, "passive", 7) == 0) {
                sdp->remote_setup = NANO_SDP_SETUP_PASSIVE;
            } else {
                sdp->remote_setup = NANO_SDP_SETUP_ACTPASS;
            }
        }

        pos = eol;
    }

    /* Validate required fields */
    if (sdp->remote_ufrag[0] == '\0' || sdp->remote_pwd[0] == '\0') {
        NANO_LOGW("SDP", "missing ice-ufrag or ice-pwd");
        return NANO_ERR_PARSE;
    }

    sdp->parsed = true;
    NANO_LOGI("SDP", "offer parsed");
    return NANO_OK;
}

/* ================================================================
 * Generator
 * ================================================================ */

int sdp_generate_answer(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *out_len)
{
    if (!sdp || !buf || !out_len || buf_len < NANO_SDP_MIN_BUF_SIZE) {
        return NANO_ERR_INVALID_PARAM;
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
    if (!sdp_append(buf, buf_len, &pos, "a=group:BUNDLE 0\r\n"))
        goto overflow;

#if NANO_FEATURE_DATACHANNEL
    /* DataChannel media line */
    if (!sdp_append(buf, buf_len, &pos, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"))
        goto overflow;
#else
    /* Media-only: use a dummy m-line for DTLS/ICE bundle */
    if (!sdp_append(buf, buf_len, &pos, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"))
        goto overflow;
#endif
    if (!sdp_append(buf, buf_len, &pos, "c=IN IP4 0.0.0.0\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "a=mid:0\r\n"))
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
    const char *setup_str = "a=setup:passive\r\n";
    if (sdp->local_setup == NANO_SDP_SETUP_ACTIVE) {
        setup_str = "a=setup:active\r\n";
    } else if (sdp->local_setup == NANO_SDP_SETUP_ACTPASS) {
        setup_str = "a=setup:actpass\r\n";
    }
    if (!sdp_append(buf, buf_len, &pos, setup_str))
        goto overflow;

#if NANO_FEATURE_DATACHANNEL
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

    *out_len = pos;
    NANO_LOGD("SDP", "answer generated");
    return NANO_OK;

overflow:
    NANO_LOGE("SDP", "buffer overflow generating answer");
    return NANO_ERR_BUFFER_TOO_SMALL;
}
