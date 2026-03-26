/*
 * nanortc — DataChannel / DCEP protocol (RFC 8831, RFC 8832)
 *
 * Reference: libpeer peer_connection.c (DCEP handling),
 *            str0m src/sctp/dcep.rs (message format).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_datachannel.h"
#include "nano_log.h"
#include "nanortc.h"
#include <string.h>

/* ================================================================
 * Codec
 * ================================================================ */

int dcep_parse_open(const uint8_t *data, size_t len, dcep_open_t *out)
{
    /*
     * DATA_CHANNEL_OPEN format (RFC 8832 §5.1):
     *   0: message_type (0x03)
     *   1: channel_type
     *   2-3: priority
     *   4-7: reliability_parameter
     *   8-9: label_length
     *  10-11: protocol_length
     *  12+: label + protocol
     */
    if (!data || !out || len < 12) {
        return NANO_ERR_PARSE;
    }
    if (data[0] != DCEP_DATA_CHANNEL_OPEN) {
        return NANO_ERR_PROTOCOL;
    }

    out->channel_type = data[1];
    out->priority = nano_ntohs(*(const uint16_t *)(data + 2));
    out->reliability_param = nano_ntohl(*(const uint32_t *)(data + 4));
    out->label_len = nano_ntohs(*(const uint16_t *)(data + 8));
    out->protocol_len = nano_ntohs(*(const uint16_t *)(data + 10));

    if ((size_t)(12 + out->label_len + out->protocol_len) > len) {
        return NANO_ERR_PARSE;
    }

    out->label = (out->label_len > 0) ? (const char *)(data + 12) : "";
    out->protocol = (out->protocol_len > 0) ? (const char *)(data + 12 + out->label_len) : "";

    return NANO_OK;
}

size_t dcep_encode_open(uint8_t *buf, uint8_t channel_type, uint16_t priority,
                        uint32_t reliability_param, const char *label, uint16_t label_len,
                        const char *protocol, uint16_t protocol_len)
{
    buf[0] = DCEP_DATA_CHANNEL_OPEN;
    buf[1] = channel_type;
    *(uint16_t *)(buf + 2) = nano_htons(priority);
    *(uint32_t *)(buf + 4) = nano_htonl(reliability_param);
    *(uint16_t *)(buf + 8) = nano_htons(label_len);
    *(uint16_t *)(buf + 10) = nano_htons(protocol_len);

    size_t pos = 12;
    if (label && label_len > 0) {
        memcpy(buf + pos, label, label_len);
        pos += label_len;
    }
    if (protocol && protocol_len > 0) {
        memcpy(buf + pos, protocol, protocol_len);
        pos += protocol_len;
    }

    return pos;
}

size_t dcep_encode_ack(uint8_t *buf)
{
    buf[0] = DCEP_DATA_CHANNEL_ACK;
    return 1;
}

/* ================================================================
 * Channel management
 * ================================================================ */

static nano_dc_channel_t *dc_find_channel(nano_dc_t *dc, uint16_t stream_id)
{
    for (uint8_t i = 0; i < dc->channel_count; i++) {
        if (dc->channels[i].stream_id == stream_id &&
            dc->channels[i].state != NANO_DC_STATE_CLOSED) {
            return &dc->channels[i];
        }
    }
    return NULL;
}

static nano_dc_channel_t *dc_alloc_channel(nano_dc_t *dc, uint16_t stream_id)
{
    if (dc->channel_count >= NANO_MAX_DATACHANNELS) {
        return NULL;
    }
    nano_dc_channel_t *ch = &dc->channels[dc->channel_count++];
    memset(ch, 0, sizeof(*ch));
    ch->stream_id = stream_id;
    return ch;
}

/* ================================================================
 * State machine
 * ================================================================ */

int dc_init(nano_dc_t *dc)
{
    if (!dc) {
        return NANO_ERR_INVALID_PARAM;
    }
    memset(dc, 0, sizeof(*dc));
    return NANO_OK;
}

int dc_handle_message(nano_dc_t *dc, uint16_t stream_id, uint32_t ppid, const uint8_t *data,
                      size_t len)
{
    if (!dc) {
        return NANO_ERR_INVALID_PARAM;
    }

    if (ppid == DCEP_PPID_CONTROL) {
        if (len < 1) {
            return NANO_ERR_PARSE;
        }

        if (data[0] == DCEP_DATA_CHANNEL_OPEN) {
            /* Parse and handle incoming OPEN */
            dcep_open_t open;
            int rc = dcep_parse_open(data, len, &open);
            if (rc != NANO_OK) {
                return rc;
            }

            /* Allocate channel */
            nano_dc_channel_t *ch = dc_alloc_channel(dc, stream_id);
            if (!ch) {
                NANO_LOGW("DC", "max channels reached");
                return NANO_ERR_BUFFER_TOO_SMALL;
            }

            ch->state = NANO_DC_STATE_OPEN;
            ch->channel_type = open.channel_type;
            ch->ordered = !(open.channel_type & 0x80);

            /* Copy label (truncate to fit) */
            uint16_t copy_len = open.label_len;
            if (copy_len >= sizeof(ch->label)) {
                copy_len = sizeof(ch->label) - 1;
            }
            memcpy(ch->label, open.label, copy_len);
            ch->label[copy_len] = '\0';

            /* Queue DCEP ACK response */
            dc->out_len = (uint16_t)dcep_encode_ack(dc->out_buf);
            dc->out_stream = stream_id;
            dc->has_output = true;

            NANO_LOGI("DC", "channel opened by peer");
            return NANO_OK;

        } else if (data[0] == DCEP_DATA_CHANNEL_ACK) {
            /* Transition OPENING → OPEN */
            nano_dc_channel_t *ch = dc_find_channel(dc, stream_id);
            if (ch && ch->state == NANO_DC_STATE_OPENING) {
                ch->state = NANO_DC_STATE_OPEN;
                NANO_LOGI("DC", "channel opened (ACK received)");
            }
            return NANO_OK;
        }

        return NANO_ERR_PROTOCOL;
    }

    /* Data messages (PPID=51/53/56/57) — just validate channel exists */
    /* The caller (nano_rtc.c) handles event emission */
    (void)data;
    (void)len;
    (void)stream_id;
    return NANO_OK;
}

int dc_open(nano_dc_t *dc, uint16_t stream_id, const char *label)
{
    if (!dc || !label) {
        return NANO_ERR_INVALID_PARAM;
    }

    nano_dc_channel_t *ch = dc_alloc_channel(dc, stream_id);
    if (!ch) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }

    ch->state = NANO_DC_STATE_OPENING;
    ch->channel_type = DCEP_CHANNEL_RELIABLE;
    ch->ordered = true;

    uint16_t label_len = 0;
    while (label[label_len] && label_len < sizeof(ch->label) - 1) {
        label_len++;
    }
    memcpy(ch->label, label, label_len);
    ch->label[label_len] = '\0';

    /* Encode DCEP OPEN */
    dc->out_len = (uint16_t)dcep_encode_open(dc->out_buf, DCEP_CHANNEL_RELIABLE, 0, 0, label,
                                             label_len, NULL, 0);
    dc->out_stream = stream_id;
    dc->has_output = true;

    NANO_LOGD("DC", "OPEN queued");
    return NANO_OK;
}

int dc_poll_output(nano_dc_t *dc, uint8_t *buf, size_t buf_len, size_t *out_len,
                   uint16_t *stream_id)
{
    if (!dc || !buf || !out_len || !stream_id) {
        return NANO_ERR_INVALID_PARAM;
    }

    if (!dc->has_output || dc->out_len == 0) {
        *out_len = 0;
        return NANO_ERR_NO_DATA;
    }

    if (buf_len < dc->out_len) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(buf, dc->out_buf, dc->out_len);
    *out_len = dc->out_len;
    *stream_id = dc->out_stream;
    dc->has_output = false;
    dc->out_len = 0;
    return NANO_OK;
}
