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
static void parse_rtpmap(nano_sdp_mline_t *ml, const char *line, size_t line_len)
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
    } else if (ml->kind == SDP_MLINE_VIDEO) {
        /* Check if this PT maps to H264 */
        p = next + 1;
        if (p + 4 <= end && memcmp(p, "H264", 4) == 0) {
            ml->video_h264_rtpmap_pt = (uint8_t)pt;
        }
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

    /* Reset m-line tracking — parser assigns MIDs from position in SDP */
    sdp->mid_count = 0;
    sdp->has_datachannel = false;
#if NANORTC_HAVE_MEDIA_TRANSPORT
    sdp->mline_count = 0;
    /* Index into sdp->mlines[] for current m-line, -1 if not a media m-line */
    int current_mline_idx = -1;
#endif

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
        } else if (line_starts_with(line, line_len, "m=audio ")) {
            if (sdp->mline_count < NANORTC_MAX_MEDIA_TRACKS) {
                nano_sdp_mline_t *ml = &sdp->mlines[sdp->mline_count];
                memset(ml, 0, sizeof(*ml));
                ml->kind = SDP_MLINE_AUDIO;
                ml->mid = sdp->mid_count;
                ml->active = true;
                ml->remote_pt = parse_mline_first_pt(line, line_len, 8);
                current_mline_idx = (int)sdp->mline_count;
                sdp->mline_count++;
            } else {
                current_mline_idx = -1;
            }
            sdp->mid_count++;
        } else if (line_starts_with(line, line_len, "m=video ")) {
            if (sdp->mline_count < NANORTC_MAX_MEDIA_TRACKS) {
                nano_sdp_mline_t *ml = &sdp->mlines[sdp->mline_count];
                memset(ml, 0, sizeof(*ml));
                ml->kind = SDP_MLINE_VIDEO;
                ml->mid = sdp->mid_count;
                ml->active = true;
                ml->remote_pt = parse_mline_first_pt(line, line_len, 8);
                current_mline_idx = (int)sdp->mline_count;
                sdp->mline_count++;
            } else {
                current_mline_idx = -1;
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
            nano_sdp_mline_t *ml = &sdp->mlines[current_mline_idx];
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
            const char *search = fnext;
            size_t remain = (size_t)(fend - search);
            while (remain >= 20) {
                if (memcmp(search, "packetization-mode=1", 20) == 0) {
                    has_mode1 = true;
                }
                if (remain >= 23 && memcmp(search, "profile-level-id=42e01f", 23) == 0) {
                    has_preferred_profile = true;
                }
                search++;
                remain--;
            }
            /* Select this PT if it's H264 with packetization-mode=1 and
             * the rtpmap PT matches (or is the first H264 we've seen). */
            bool is_valid_h264 = has_mode1 && (ml->video_h264_rtpmap_pt == 0 ||
                                               ml->video_h264_rtpmap_pt == (uint8_t)fmtp_pt);
            if (is_valid_h264 && (has_preferred_profile || ml->pt == 0)) {
                ml->pt = (uint8_t)fmtp_pt;
                NANORTC_LOGD("SDP", has_preferred_profile ? "video H264 PT selected (profile match)"
                                                          : "video H264 PT selected (fallback)");
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
                if (sdp->candidate_count < NANORTC_SDP_MAX_CANDIDATES) {
                    const char *p = line + 12;
                    const char *line_end = line + line_len;
                    int field = 1;

                    while (p < line_end && field < 5) {
                        if (*p == ' ') {
                            field++;
                            while (p < line_end && *p == ' ')
                                p++;
                        } else {
                            p++;
                        }
                    }

                    const char *addr_start = p;
                    while (p < line_end && *p != ' ')
                        p++;
                    size_t addr_len = (size_t)(p - addr_start);

                    while (p < line_end && *p == ' ')
                        p++;

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
        NANORTC_LOGW("SDP", "missing ice-ufrag or ice-pwd");
        return NANORTC_ERR_PARSE;
    }

    sdp->parsed = true;
    NANORTC_LOGI("SDP", "offer parsed");
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

/** Append local ICE candidate (RFC 8839 §5.1). Called once for the BUNDLE anchor m-line. */
static bool sdp_append_candidate(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos)
{
    if (!sdp->has_local_candidate || sdp->local_candidate_ip[0] == '\0')
        return true;
    if (!sdp_append(buf, buf_len, pos, "a=candidate:1 1 UDP 2122252543 "))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp->local_candidate_ip))
        return false;
    if (!sdp_append(buf, buf_len, pos, " "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, sdp->local_candidate_port))
        return false;
    if (!sdp_append(buf, buf_len, pos, " typ host\r\n"))
        return false;
    return true;
}

/** Append server-reflexive ICE candidate (RFC 8839 §5.1).
 *  Priority 1090519295 = type preference 64 for srflx (RFC 8445 §5.1.2.1). */
static bool sdp_append_srflx_candidate(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos)
{
    if (!sdp->has_srflx_candidate || sdp->srflx_candidate_ip[0] == '\0')
        return true;
    if (!sdp_append(buf, buf_len, pos, "a=candidate:3 1 UDP 1090519295 "))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp->srflx_candidate_ip))
        return false;
    if (!sdp_append(buf, buf_len, pos, " "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, sdp->srflx_candidate_port))
        return false;
    if (!sdp_append(buf, buf_len, pos, " typ srflx"))
        return false;
    /* raddr/rport from host candidate */
    if (sdp->has_local_candidate && sdp->local_candidate_ip[0] != '\0') {
        if (!sdp_append(buf, buf_len, pos, " raddr "))
            return false;
        if (!sdp_append(buf, buf_len, pos, sdp->local_candidate_ip))
            return false;
        if (!sdp_append(buf, buf_len, pos, " rport "))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, sdp->local_candidate_port))
            return false;
    }
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    return true;
}

/** Append relay ICE candidate from TURN allocation (RFC 8839 §5.1).
 *  Priority 16777215 = type preference 0 for relay (RFC 8445 §5.1.2.1). */
static bool sdp_append_relay_candidate(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos)
{
    if (!sdp->has_relay_candidate || sdp->relay_candidate_ip[0] == '\0')
        return true;
    if (!sdp_append(buf, buf_len, pos, "a=candidate:2 1 UDP 16777215 "))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp->relay_candidate_ip))
        return false;
    if (!sdp_append(buf, buf_len, pos, " "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, sdp->relay_candidate_port))
        return false;
    if (!sdp_append(buf, buf_len, pos, " typ relay"))
        return false;
    /* raddr/rport from host candidate (if available) */
    if (sdp->has_local_candidate && sdp->local_candidate_ip[0] != '\0') {
        if (!sdp_append(buf, buf_len, pos, " raddr "))
            return false;
        if (!sdp_append(buf, buf_len, pos, sdp->local_candidate_ip))
            return false;
        if (!sdp_append(buf, buf_len, pos, " rport "))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, sdp->local_candidate_port))
            return false;
    }
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    return true;
}

/** Return the SDP connection line based on local candidate address family.
 *  IPv6 local candidate (contains ':') → "c=IN IP6 ::\r\n", else IPv4. */
static const char *sdp_connection_line(const nano_sdp_t *sdp)
{
#if NANORTC_FEATURE_IPV6
    if (sdp->has_local_candidate) {
        const char *p = sdp->local_candidate_ip;
        while (*p) {
            if (*p == ':') {
                return "c=IN IP6 ::\r\n";
            }
            p++;
        }
    }
#else
    (void)sdp;
#endif
    return "c=IN IP4 0.0.0.0\r\n";
}

/** Return the SDP origin line based on local candidate address family. */
static const char *sdp_origin_line(const nano_sdp_t *sdp)
{
#if NANORTC_FEATURE_IPV6
    if (sdp->has_local_candidate) {
        const char *p = sdp->local_candidate_ip;
        while (*p) {
            if (*p == ':') {
                return "o=- 1 1 IN IP6 ::\r\n";
            }
            p++;
        }
    }
#else
    (void)sdp;
#endif
    return "o=- 1 1 IN IP4 0.0.0.0\r\n";
}

/** Append DataChannel (application) m-line block. */
static bool sdp_append_datachannel_mline(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos,
                                         const char *mid)
{
    if (!sdp_append(buf, buf_len, pos, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, sdp_connection_line(sdp)))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=mid:"))
        return false;
    if (!sdp_append(buf, buf_len, pos, mid))
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
    if (!sdp_append(buf, buf_len, pos, "a=max-message-size:262144\r\n"))
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
                                   size_t *pos, const char *mid)
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
    if (!sdp_append(buf, buf_len, pos, mid))
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

/** Append video m-line block (H.264, RFC 6184 §8.1). */
static bool sdp_append_video_mline(nano_sdp_t *sdp, nano_sdp_mline_t *ml, char *buf, size_t buf_len,
                                   size_t *pos, const char *mid)
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
    if (!sdp_append(buf, buf_len, pos, mid))
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
    if (!sdp_append(buf, buf_len, pos, " H264/90000\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=fmtp:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;
    if (!sdp_append(buf, buf_len, pos,
                    " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=rtcp-fb:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)ml->pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, " nack pli\r\n"))
        return false;
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
        char mid_str[4];
        for (int i = 0; i < n_entries; i++) {
            /* Convert MID to string (supports MID 0-255) */
            if (entries[i].mid < 10) {
                mid_str[0] = '0' + entries[i].mid;
                mid_str[1] = '\0';
            } else if (entries[i].mid < 100) {
                mid_str[0] = '0' + (entries[i].mid / 10);
                mid_str[1] = '0' + (entries[i].mid % 10);
                mid_str[2] = '\0';
            } else {
                mid_str[0] = '0' + (entries[i].mid / 100);
                mid_str[1] = '0' + ((entries[i].mid / 10) % 10);
                mid_str[2] = '0' + (entries[i].mid % 10);
                mid_str[3] = '\0';
            }
            switch (entries[i].type) {
            case SDP_MLINE_APPLICATION:
                if (!sdp_append_datachannel_mline(sdp, buf, buf_len, &pos, mid_str))
                    goto overflow;
                break;
            case SDP_MLINE_AUDIO:
                if (!sdp_append_audio_mline(sdp, &sdp->mlines[entries[i].ml_idx], buf, buf_len,
                                            &pos, mid_str))
                    goto overflow;
                break;
            case SDP_MLINE_VIDEO:
                if (!sdp_append_video_mline(sdp, &sdp->mlines[entries[i].ml_idx], buf, buf_len,
                                            &pos, mid_str))
                    goto overflow;
                break;
            }
            if (i == 0) {
                if (!sdp_append_candidate(sdp, buf, buf_len, &pos))
                    goto overflow;
                if (!sdp_append_srflx_candidate(sdp, buf, buf_len, &pos))
                    goto overflow;
                if (!sdp_append_relay_candidate(sdp, buf, buf_len, &pos))
                    goto overflow;
            }
        }
    }
#else
    if (!sdp_append(buf, buf_len, &pos, "a=group:BUNDLE 0\r\n"))
        goto overflow;
    if (!sdp_append_datachannel_mline(sdp, buf, buf_len, &pos, "0"))
        goto overflow;
    if (!sdp_append_candidate(sdp, buf, buf_len, &pos))
        goto overflow;
    if (!sdp_append_srflx_candidate(sdp, buf, buf_len, &pos))
        goto overflow;
    if (!sdp_append_relay_candidate(sdp, buf, buf_len, &pos))
        goto overflow;
#endif

    *out_len = pos;
    NANORTC_LOGD("SDP", "answer generated");
    return NANORTC_OK;

overflow:
    NANORTC_LOGE("SDP", "buffer overflow generating answer");
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
