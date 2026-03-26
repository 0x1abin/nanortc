/*
 * nanortc — ICE internal interface (RFC 8445)
 *
 * Supports two roles:
 *   CONTROLLED (answerer)  — respond to incoming STUN checks (ICE-Lite)
 *   CONTROLLING (offerer)  — initiate STUN connectivity checks
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_ICE_H_
#define NANO_ICE_H_

#include "nanortc_config.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward declarations — avoid including heavy headers */
#ifndef NANO_ADDR_T_DECLARED
#define NANO_ADDR_T_DECLARED
typedef struct nano_addr nano_addr_t;
#endif
#ifndef NANO_CRYPTO_PROVIDER_T_DECLARED
#define NANO_CRYPTO_PROVIDER_T_DECLARED
typedef struct nano_crypto_provider nano_crypto_provider_t;
#endif

typedef enum {
    NANO_ICE_STATE_NEW,
    NANO_ICE_STATE_CHECKING,
    NANO_ICE_STATE_CONNECTED,
    NANO_ICE_STATE_FAILED,
} nano_ice_state_t;

/* ICE_MAX_CHECKS is now NANO_ICE_MAX_CHECKS in nanortc_config.h */

typedef struct nano_ice {
    nano_ice_state_t state;
    int is_controlling; /* 0 = controlled (answerer), 1 = controlling (offerer) */
    char local_ufrag[8];
    char local_pwd[32];
    char remote_ufrag[32];
    char remote_pwd[128];
    uint8_t selected_addr[16];
    uint16_t selected_port;
    uint8_t selected_family;
    uint32_t check_interval_ms; /* for controlling role: STUN check pacing */
    uint32_t next_check_ms;

    /* Controlling role state */
    uint64_t tie_breaker;  /* 8-byte random for ICE-CONTROLLING/CONTROLLED */
    uint8_t last_txid[12]; /* transaction ID of last outgoing request */
    uint8_t check_count;   /* number of checks sent */
    bool nominated;        /* selected pair nominated */

    /* Remote candidate address (for controlling role outgoing checks) */
    uint8_t remote_addr[16];
    uint16_t remote_port;
    uint8_t remote_family; /* 4 = IPv4, 6 = IPv6 */
} nano_ice_t;

int ice_init(nano_ice_t *ice, int is_controlling);

/* Handle incoming STUN message (both roles).
 * Writes response to resp_buf if needed; sets *resp_len to 0 if no response. */
int ice_handle_stun(nano_ice_t *ice, const uint8_t *data, size_t len, const nano_addr_t *src,
                    const nano_crypto_provider_t *crypto, uint8_t *resp_buf, size_t resp_buf_len,
                    size_t *resp_len);

/* Generate outgoing STUN Binding Request (controlling role only).
 * Returns NANO_OK with *out_len=0 if not time to send yet. */
int ice_generate_check(nano_ice_t *ice, uint32_t now_ms, const nano_crypto_provider_t *crypto,
                       uint8_t *buf, size_t buf_len, size_t *out_len);

bool ice_is_stun(const uint8_t *data, size_t len);

#endif /* NANO_ICE_H_ */
