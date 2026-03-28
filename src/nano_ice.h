/*
 * nanortc — ICE internal interface (RFC 8445)
 * @internal Not part of the public API.
 *
 * Supports two roles:
 *   CONTROLLED (answerer)  — respond to incoming STUN checks (ICE-Lite)
 *   CONTROLLING (offerer)  — initiate STUN connectivity checks
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_ICE_H_
#define NANORTC_ICE_H_

#include "nanortc_config.h"
#include "nano_stun.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward declarations — avoid including heavy headers */
#ifndef NANORTC_ADDR_T_DECLARED
#define NANORTC_ADDR_T_DECLARED
typedef struct nanortc_addr nanortc_addr_t;
#endif
#ifndef NANORTC_CRYPTO_PROVIDER_T_DECLARED
#define NANORTC_CRYPTO_PROVIDER_T_DECLARED
typedef struct nanortc_crypto_provider nanortc_crypto_provider_t;
#endif

typedef enum {
    NANORTC_ICE_STATE_NEW,
    NANORTC_ICE_STATE_CHECKING,
    NANORTC_ICE_STATE_CONNECTED,
    NANORTC_ICE_STATE_FAILED,
} nano_ice_state_t;

/* ICE_MAX_CHECKS is now NANORTC_ICE_MAX_CHECKS in nanortc_config.h */

typedef struct nano_ice_candidate {
    uint8_t addr[NANORTC_ADDR_SIZE];
    uint16_t port;
    uint8_t family; /* 4 = IPv4, 6 = IPv6 */
} nano_ice_candidate_t;

typedef struct nano_ice {
    nano_ice_state_t state;
    int is_controlling; /* 0 = controlled (answerer), 1 = controlling (offerer) */
    char local_ufrag[NANORTC_ICE_UFRAG_SIZE];
    size_t local_ufrag_len;
    char local_pwd[NANORTC_ICE_PWD_SIZE];
    size_t local_pwd_len;
    char remote_ufrag[NANORTC_ICE_REMOTE_UFRAG_SIZE];
    size_t remote_ufrag_len;
    char remote_pwd[NANORTC_ICE_REMOTE_PWD_SIZE];
    size_t remote_pwd_len;
    uint8_t selected_addr[NANORTC_ADDR_SIZE];
    uint16_t selected_port;
    uint8_t selected_family;
    uint32_t check_interval_ms; /* for controlling role: STUN check pacing */
    uint32_t next_check_ms;

    /* Controlling role state */
    uint64_t tie_breaker;              /* 8-byte random for ICE-CONTROLLING/CONTROLLED */
    uint8_t last_txid[STUN_TXID_SIZE]; /* transaction ID of last outgoing request */
    uint8_t check_count;               /* number of checks sent */
    bool nominated;                    /* selected pair nominated */

    /* Remote candidates array (trickle ICE support) */
    nano_ice_candidate_t remote_candidates[NANORTC_MAX_ICE_CANDIDATES];
    uint8_t remote_candidate_count;
    uint8_t current_candidate; /* index of candidate currently being checked */
} nano_ice_t;

int ice_init(nano_ice_t *ice, int is_controlling);

/* Handle incoming STUN message (both roles).
 * Writes response to resp_buf if needed; sets *resp_len to 0 if no response. */
int ice_handle_stun(nano_ice_t *ice, const uint8_t *data, size_t len, const nanortc_addr_t *src,
                    const nanortc_crypto_provider_t *crypto, uint8_t *resp_buf, size_t resp_buf_len,
                    size_t *resp_len);

/* Generate outgoing STUN Binding Request (controlling role only).
 * Returns NANORTC_OK with *out_len=0 if not time to send yet. */
int ice_generate_check(nano_ice_t *ice, uint32_t now_ms, const nanortc_crypto_provider_t *crypto,
                       uint8_t *buf, size_t buf_len, size_t *out_len);

bool ice_is_stun(const uint8_t *data, size_t len);

#endif /* NANORTC_ICE_H_ */
