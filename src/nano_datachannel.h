/*
 * nanortc — DataChannel (DCEP) internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_DATACHANNEL_H_
#define NANO_DATACHANNEL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NANO_DC_MAX_CHANNELS 8

/* DCEP message types */
#define DCEP_DATA_CHANNEL_OPEN 0x03
#define DCEP_DATA_CHANNEL_ACK  0x02

/* SCTP PPIDs for DataChannel */
#define DCEP_PPID_CONTROL    50
#define DCEP_PPID_STRING     51
#define DCEP_PPID_BINARY     53
#define DCEP_PPID_STRING_EMPTY 56
#define DCEP_PPID_BINARY_EMPTY 57

typedef enum {
    NANO_DC_STATE_CLOSED,
    NANO_DC_STATE_OPENING,
    NANO_DC_STATE_OPEN,
} nano_dc_state_t;

typedef struct nano_dc_channel {
    nano_dc_state_t state;
    uint16_t stream_id;
    char label[32];
    bool ordered;
    uint16_t max_retransmits;
} nano_dc_channel_t;

typedef struct nano_dc {
    nano_dc_channel_t channels[NANO_DC_MAX_CHANNELS];
    uint8_t channel_count;
} nano_dc_t;

int dc_init(nano_dc_t *dc);
int dc_handle_dcep(nano_dc_t *dc, uint16_t stream_id,
                   const uint8_t *data, size_t len);
int dc_open(nano_dc_t *dc, uint16_t stream_id, const char *label,
            uint8_t *out_buf, size_t *out_len);

#endif /* NANO_DATACHANNEL_H_ */
