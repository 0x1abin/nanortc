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
#define SDP_MLINE_VIDEO       3

/**
 * Parse a=rtpmap:<pt> <codec>/<rate>[/<channels>]
 * Example: a=rtpmap:111 opus/48000/2
 */
static void parse_rtpmap(nano_sdp_t *sdp, int current_mline, const char *line, size_t line_len)
{
    const char *p = line + 9; /* skip "a=rtpmap:" (9 chars) */
    const char *end = line + line_len;

    /* Parse payload type */
    const char *next;
    uint32_t pt = parse_u32(p, end, &next);
    if (next == p || next >= end || *next != ' ') {
        return;
    }

    if (current_mline == SDP_MLINE_AUDIO) {
        /* Only parse the rtpmap for the remote's preferred PT */
        if ((uint8_t)pt != sdp->remote_audio_pt) {
            return;
        }
        /* audio_sample_rate / audio_channels are set by nanortc_init()
         * from the local config and must not be overwritten here.
         * nanortc supports a single codec per session, so no need to
         * discover the remote codec's parameters. */
    } else if (current_mline == SDP_MLINE_VIDEO) {
        /* Check if this PT maps to H264 — store for cross-validation with fmtp.
         * Format: a=rtpmap:<pt> H264/90000 */
        p = next + 1; /* skip space after PT */
        if (p + 4 <= end && memcmp(p, "H264", 4) == 0) {
            sdp->video_h264_rtpmap_pt = (uint8_t)pt;
        }
    }
}

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

static void parse_audio_mline(nano_sdp_t *sdp, const char *line, size_t line_len)
{
    /* Store in remote_audio_pt — used by parse_rtpmap() as filter.
     * audio_pt (local PT) is set by nanortc_init() and must not be overwritten
     * when parsing a remote offer (RFC 3264 §6.1). */
    sdp->remote_audio_pt = parse_mline_first_pt(line, line_len, 8); /* skip "m=audio " */
}

/* parse_video_mline intentionally omitted: Chrome lists VP8 first in m=video,
 * so m-line first PT is unreliable for H264. PT is selected from a=fmtp
 * with packetization-mode=1 cross-validated against a=rtpmap H264. */

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
    int first_mline_type = SDP_MLINE_NONE; /* track which m-line appears first */
    bool video_h264_pt_found = false;      /* true once we find H264 with mode=1 */
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
            sdp->has_datachannel = true;
            sdp->dc_mid = sdp->mid_count;
            sdp->mid_count++;
            if (first_mline_type == SDP_MLINE_NONE) {
                first_mline_type = SDP_MLINE_APPLICATION;
            }
        } else if (line_starts_with(line, line_len, "m=audio ")) {
            current_mline = SDP_MLINE_AUDIO;
            sdp->has_audio = true;
            sdp->audio_mid = sdp->mid_count;
            sdp->mid_count++;
            if (first_mline_type == SDP_MLINE_NONE) {
                first_mline_type = SDP_MLINE_AUDIO;
            }
            sdp->audio_before_datachannel = (first_mline_type == SDP_MLINE_AUDIO);
            parse_audio_mline(sdp, line, line_len);
        } else if (line_starts_with(line, line_len, "m=video ")) {
            current_mline = SDP_MLINE_VIDEO;
            sdp->has_video = true;
            sdp->video_mid = sdp->mid_count;
            sdp->mid_count++;
            if (first_mline_type == SDP_MLINE_NONE) {
                first_mline_type = SDP_MLINE_VIDEO;
            }
            sdp->video_before_datachannel = !sdp->has_audio || sdp->audio_before_datachannel;
            sdp->video_before_audio = !sdp->has_audio;
        } else if (line_len >= 2 && line[0] == 'm' && line[1] == '=') {
            current_mline = SDP_MLINE_NONE;
            sdp->mid_count++;
        } else if ((current_mline == SDP_MLINE_AUDIO || current_mline == SDP_MLINE_VIDEO) &&
                   line_starts_with(line, line_len, "a=rtpmap:")) {
            parse_rtpmap(sdp, current_mline, line, line_len);
        } else if (current_mline == SDP_MLINE_VIDEO &&
                   line_starts_with(line, line_len, "a=fmtp:")) {
            /* Parse video fmtp for packetization-mode=1 (RFC 6184 §8.1).
             * Format: a=fmtp:<pt> key=val;key=val...
             * Cross-validate: PT must also be H264 per rtpmap. */
            const char *fp = line + 7; /* skip "a=fmtp:" */
            const char *fend = line + line_len;
            const char *fnext;
            uint32_t fmtp_pt = parse_u32(fp, fend, &fnext);
            /* Search for "packetization-mode=1" substring */
            bool has_mode1 = false;
            const char *search = fnext;
            size_t remain = (size_t)(fend - search);
            while (remain >= 20) {
                if (memcmp(search, "packetization-mode=1", 20) == 0) {
                    has_mode1 = true;
                    break;
                }
                search++;
                remain--;
            }
            if (has_mode1 && !video_h264_pt_found) {
                /* Accept if rtpmap confirmed H264, or if rtpmap not yet seen
                 * (SDP line ordering is not guaranteed). */
                if (sdp->video_h264_rtpmap_pt == 0 ||
                    sdp->video_h264_rtpmap_pt == (uint8_t)fmtp_pt) {
                    sdp->video_pt = (uint8_t)fmtp_pt;
                    video_h264_pt_found = true;
                }
            }
        } else if ((current_mline == SDP_MLINE_AUDIO || current_mline == SDP_MLINE_VIDEO) &&
                   (line_starts_with(line, line_len, "a=sendrecv") ||
                    line_starts_with(line, line_len, "a=sendonly") ||
                    line_starts_with(line, line_len, "a=recvonly") ||
                    line_starts_with(line, line_len, "a=inactive"))) {
            /* Parse direction attribute per media m-line (RFC 3264 §6) */
            nanortc_direction_t dir = NANORTC_DIR_SENDRECV;
            if (line_starts_with(line, line_len, "a=sendonly")) {
                dir = NANORTC_DIR_SENDONLY;
            } else if (line_starts_with(line, line_len, "a=recvonly")) {
                dir = NANORTC_DIR_RECVONLY;
            } else if (line_starts_with(line, line_len, "a=inactive")) {
                dir = NANORTC_DIR_INACTIVE;
            }
            if (current_mline == SDP_MLINE_AUDIO) {
                sdp->remote_audio_direction = dir;
            } else {
                sdp->remote_video_direction = dir;
            }
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

/** Append DataChannel (application) m-line block. */
static bool sdp_append_datachannel_mline(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos,
                                         const char *mid)
{
    if (!sdp_append(buf, buf_len, pos, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "c=IN IP4 0.0.0.0\r\n"))
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

/** Append audio m-line block. */
static bool sdp_append_audio_mline(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos,
                                   const char *mid)
{
    if (!sdp_append(buf, buf_len, pos, "m=audio 9 UDP/TLS/RTP/SAVPF "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)sdp->audio_pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "c=IN IP4 0.0.0.0\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=mid:"))
        return false;
    if (!sdp_append(buf, buf_len, pos, mid))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append_direction(buf, buf_len, pos, sdp->audio_direction))
        return false;
    if (!sdp_append_transport_attrs(sdp, buf, buf_len, pos))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=rtcp-mux\r\n"))
        return false;
    /* a=rtpmap */
    if (!sdp_append(buf, buf_len, pos, "a=rtpmap:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)sdp->audio_pt))
        return false;
    {
        const char *codec_str = " opus/48000/2";
        if (sdp->audio_pt == 0) {
            codec_str = " PCMU/8000";
        } else if (sdp->audio_pt == 8) {
            codec_str = " PCMA/8000";
        }
        if (!sdp_append(buf, buf_len, pos, codec_str))
            return false;
    }
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    /* a=fmtp (Opus decoder hints, RFC 7587 §6.1) */
    if (sdp->audio_sample_rate == 48000) {
        if (!sdp_append(buf, buf_len, pos, "a=fmtp:"))
            return false;
        if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)sdp->audio_pt))
            return false;
        if (!sdp_append(buf, buf_len, pos,
                        sdp->audio_channels >= 2 ? " minptime=10;useinbandfec=1;stereo=1\r\n"
                                                 : " minptime=10;useinbandfec=1;stereo=0\r\n"))
            return false;
        if (!sdp_append(buf, buf_len, pos, "a=ptime:20\r\n"))
            return false;
    }
    return true;
}

/** Append video m-line block (H.264, RFC 6184 §8.1). */
static bool sdp_append_video_mline(nano_sdp_t *sdp, char *buf, size_t buf_len, size_t *pos,
                                   const char *mid)
{
    if (!sdp_append(buf, buf_len, pos, "m=video 9 UDP/TLS/RTP/SAVPF "))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)sdp->video_pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "c=IN IP4 0.0.0.0\r\n"))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=mid:"))
        return false;
    if (!sdp_append(buf, buf_len, pos, mid))
        return false;
    if (!sdp_append(buf, buf_len, pos, "\r\n"))
        return false;
    if (!sdp_append_direction(buf, buf_len, pos, sdp->video_direction))
        return false;
    if (!sdp_append_transport_attrs(sdp, buf, buf_len, pos))
        return false;
    if (!sdp_append(buf, buf_len, pos, "a=rtcp-mux\r\n"))
        return false;
    /* a=rtpmap (RFC 6184 §8.1) */
    if (!sdp_append(buf, buf_len, pos, "a=rtpmap:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)sdp->video_pt))
        return false;
    if (!sdp_append(buf, buf_len, pos, " H264/90000\r\n"))
        return false;
    /* a=fmtp: Constrained Baseline Profile Level 3.1 (RFC 7742 §6.2)
     * packetization-mode=1 enables FU-A fragmentation. */
    if (!sdp_append(buf, buf_len, pos, "a=fmtp:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)sdp->video_pt))
        return false;
    if (!sdp_append(buf, buf_len, pos,
                    " level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"))
        return false;
    /* RTCP feedback: PLI for keyframe requests (RFC 4585) */
    if (!sdp_append(buf, buf_len, pos, "a=rtcp-fb:"))
        return false;
    if (!sdp_append_u16(buf, buf_len, pos, (uint16_t)sdp->video_pt))
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
    if (!sdp_append(buf, buf_len, &pos, "o=- 1 1 IN IP4 0.0.0.0\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "s=-\r\n"))
        goto overflow;
    if (!sdp_append(buf, buf_len, &pos, "t=0 0\r\n"))
        goto overflow;

    /* BUNDLE group: list all active MIDs.
     * We build the m-line table dynamically based on what's active.
     * The answer must preserve the offer's m-line order (RFC 8829 §5.3.1).
     * For offer generation, we use the parsed MID indices; for fresh offers,
     * MIDs are assigned sequentially (dc=0, audio=1, video=2 by default). */
#if NANORTC_HAVE_MEDIA_TRANSPORT
    {
        /* Build m-line table ordered by MID index.
         * Each entry: type + MID string. Max 3 m-lines (DC + audio + video). */
        struct {
            int type;    /* SDP_MLINE_APPLICATION, SDP_MLINE_AUDIO, SDP_MLINE_VIDEO */
            uint8_t mid; /* MID index */
        } mlines[3];
        uint8_t n_mlines = 0;

        /* If parsed from offer, use parsed MIDs; otherwise assign sequentially */
        if (sdp->mid_count > 0) {
            /* Use parsed order: sort by MID index */
            if (sdp->has_datachannel) {
                mlines[n_mlines].type = SDP_MLINE_APPLICATION;
                mlines[n_mlines].mid = sdp->dc_mid;
                n_mlines++;
            }
            if (sdp->has_audio) {
                mlines[n_mlines].type = SDP_MLINE_AUDIO;
                mlines[n_mlines].mid = sdp->audio_mid;
                n_mlines++;
            }
            if (sdp->has_video) {
                mlines[n_mlines].type = SDP_MLINE_VIDEO;
                mlines[n_mlines].mid = sdp->video_mid;
                n_mlines++;
            }
            /* Simple bubble sort by MID (at most 3 elements) */
            for (int i = 0; i < n_mlines - 1; i++) {
                for (int j = i + 1; j < n_mlines; j++) {
                    if (mlines[j].mid < mlines[i].mid) {
                        int tmp_type = mlines[i].type;
                        uint8_t tmp_mid = mlines[i].mid;
                        mlines[i].type = mlines[j].type;
                        mlines[i].mid = mlines[j].mid;
                        mlines[j].type = tmp_type;
                        mlines[j].mid = tmp_mid;
                    }
                }
            }
        } else {
            /* Fresh offer: DC=0, audio=1, video=2 */
            uint8_t next_mid = 0;
            mlines[n_mlines].type = SDP_MLINE_APPLICATION;
            mlines[n_mlines].mid = next_mid++;
            n_mlines++;
            if (sdp->has_audio) {
                mlines[n_mlines].type = SDP_MLINE_AUDIO;
                mlines[n_mlines].mid = next_mid++;
                n_mlines++;
            }
            if (sdp->has_video) {
                mlines[n_mlines].type = SDP_MLINE_VIDEO;
                mlines[n_mlines].mid = next_mid++;
                n_mlines++;
            }
        }

        /* Write BUNDLE group line */
        if (!sdp_append(buf, buf_len, &pos, "a=group:BUNDLE"))
            goto overflow;
        for (int i = 0; i < n_mlines; i++) {
            if (!sdp_append(buf, buf_len, &pos, " "))
                goto overflow;
            if (!sdp_append_u16(buf, buf_len, &pos, mlines[i].mid))
                goto overflow;
        }
        if (!sdp_append(buf, buf_len, &pos, "\r\n"))
            goto overflow;

        /* Write m-lines in order. ICE candidate on first m-line (BUNDLE anchor). */
        char mid_str[4]; /* "0", "1", "2" ... */
        for (int i = 0; i < n_mlines; i++) {
            mid_str[0] = '0' + mlines[i].mid;
            mid_str[1] = '\0';
            switch (mlines[i].type) {
            case SDP_MLINE_APPLICATION:
                if (!sdp_append_datachannel_mline(sdp, buf, buf_len, &pos, mid_str))
                    goto overflow;
                break;
            case SDP_MLINE_AUDIO:
                if (!sdp_append_audio_mline(sdp, buf, buf_len, &pos, mid_str))
                    goto overflow;
                break;
            case SDP_MLINE_VIDEO:
                if (!sdp_append_video_mline(sdp, buf, buf_len, &pos, mid_str))
                    goto overflow;
                break;
            }
            if (i == 0) {
                if (!sdp_append_candidate(sdp, buf, buf_len, &pos))
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
#endif

    *out_len = pos;
    NANORTC_LOGD("SDP", "answer generated");
    return NANORTC_OK;

overflow:
    NANORTC_LOGE("SDP", "buffer overflow generating answer");
    return NANORTC_ERR_BUFFER_TOO_SMALL;
}
