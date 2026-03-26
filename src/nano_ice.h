/*
 * nanortc — ICE internal interface
 *
 * Supports two roles:
 *   CONTROLLED (answerer)  — respond to incoming STUN checks (ICE-Lite)
 *   CONTROLLING (offerer)  — initiate STUN connectivity checks
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_ICE_H_
#define NANO_ICE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    NANO_ICE_STATE_NEW,
    NANO_ICE_STATE_CHECKING,
    NANO_ICE_STATE_CONNECTED,
    NANO_ICE_STATE_FAILED,
} nano_ice_state_t;

typedef struct nano_ice {
    nano_ice_state_t state;
    int is_controlling;     /* 0 = controlled (answerer), 1 = controlling (offerer) */
    char local_ufrag[8];
    char local_pwd[32];
    char remote_ufrag[32];
    char remote_pwd[128];
    uint8_t selected_addr[16];
    uint16_t selected_port;
    uint8_t selected_family;
    uint32_t check_interval_ms; /* for controlling role: STUN check pacing */
    uint32_t next_check_ms;
} nano_ice_t;

int ice_init(nano_ice_t *ice, int is_controlling);

/* Handle incoming STUN message (both roles) */
int ice_handle_stun(nano_ice_t *ice, const uint8_t *data, size_t len,
                    uint8_t *resp_buf, size_t *resp_len);

/* Generate outgoing STUN Binding Request (controlling role only) */
int ice_generate_check(nano_ice_t *ice, uint32_t now_ms,
                       uint8_t *buf, size_t buf_len, size_t *out_len);

bool ice_is_stun(const uint8_t *data, size_t len);

#endif /* NANO_ICE_H_ */
