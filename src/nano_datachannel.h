/*
 * nanortc — DataChannel / DCEP internal interface (RFC 8831, RFC 8832)
 *
 * Reference: str0m src/sctp/dcep.rs (message format).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_DATACHANNEL_H_
#define NANORTC_DATACHANNEL_H_

#include "nanortc_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------------
 * DCEP message types (RFC 8832 §5)
 * ---------------------------------------------------------------- */

#define DCEP_DATA_CHANNEL_OPEN 0x03
#define DCEP_DATA_CHANNEL_ACK  0x02

/* ----------------------------------------------------------------
 * SCTP PPIDs for DataChannel (RFC 8831 §8)
 * ---------------------------------------------------------------- */

#define DCEP_PPID_CONTROL        50
#define DCEP_PPID_STRING         51
#define DCEP_PPID_BINARY_PARTIAL 52
#define DCEP_PPID_BINARY         53
#define DCEP_PPID_STRING_PARTIAL 54
#define DCEP_PPID_STRING_EMPTY   56
#define DCEP_PPID_BINARY_EMPTY   57

/* ----------------------------------------------------------------
 * Channel types (RFC 8832 §5.1)
 * ---------------------------------------------------------------- */

#define DCEP_CHANNEL_RELIABLE           0x00
#define DCEP_CHANNEL_RELIABLE_UNORDERED 0x80
#define DCEP_CHANNEL_REXMIT             0x01
#define DCEP_CHANNEL_REXMIT_UNORDERED   0x81
#define DCEP_CHANNEL_TIMED              0x02
#define DCEP_CHANNEL_TIMED_UNORDERED    0x82

/* ----------------------------------------------------------------
 * Parsed DCEP OPEN message
 * ---------------------------------------------------------------- */

typedef struct {
    uint8_t channel_type;
    uint16_t priority;
    uint32_t reliability_param;
    const char *label;
    uint16_t label_len;
    const char *protocol;
    uint16_t protocol_len;
} dcep_open_t;

/* ----------------------------------------------------------------
 * Per-channel state
 * ---------------------------------------------------------------- */

typedef enum {
    NANORTC_DC_STATE_CLOSED,
    NANORTC_DC_STATE_OPENING, /* sent OPEN, awaiting ACK */
    NANORTC_DC_STATE_OPEN,
} nano_dc_state_t;

typedef struct nano_dc_channel {
    nano_dc_state_t state;
    uint16_t stream_id;
    char label[NANORTC_DC_LABEL_SIZE];
    uint8_t channel_type;
    bool ordered;
    uint16_t max_retransmits;
} nano_dc_channel_t;

/* ----------------------------------------------------------------
 * DataChannel manager
 * ---------------------------------------------------------------- */

typedef struct nano_dc {
    nano_dc_channel_t channels[NANORTC_MAX_DATACHANNELS];
    uint8_t channel_count;

    /* Output: DCEP message to send via SCTP (PPID=50) */
    uint8_t out_buf[NANORTC_DC_OUT_BUF_SIZE];
    uint16_t out_len;
    uint16_t out_stream;
    bool has_output;
} nano_dc_t;

/* ----------------------------------------------------------------
 * Functions
 * ---------------------------------------------------------------- */

/** Initialize DataChannel manager. */
int dc_init(nano_dc_t *dc);

/**
 * Handle incoming SCTP payload routed by PPID.
 *
 * - PPID=50 (DCEP control): parse OPEN/ACK, manage channel state.
 * - PPID=51/53/56/57: deliver as data to caller.
 *
 * @param dc        DataChannel manager.
 * @param stream_id SCTP stream ID.
 * @param ppid      Payload Protocol Identifier.
 * @param data      Payload data.
 * @param len       Payload length.
 * @return NANORTC_OK on success, negative error code on failure.
 */
int dc_handle_message(nano_dc_t *dc, uint16_t stream_id, uint32_t ppid, const uint8_t *data,
                      size_t len);

/**
 * Open a DataChannel (generate DCEP OPEN message).
 *
 * @param dc        DataChannel manager.
 * @param stream_id SCTP stream ID to use.
 * @param label     Channel label (null-terminated).
 * @return NANORTC_OK on success.
 */
int dc_open(nano_dc_t *dc, uint16_t stream_id, const char *label);

/** Poll for outbound DCEP message. Caller sends via nsctp_send(PPID=50). */
int dc_poll_output(nano_dc_t *dc, uint8_t *buf, size_t buf_len, size_t *out_len,
                   uint16_t *stream_id);

/* DCEP codec functions are static in nano_datachannel.c */

#endif /* NANORTC_DATACHANNEL_H_ */
