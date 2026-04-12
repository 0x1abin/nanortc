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
    NANORTC_ICE_STATE_DISCONNECTED, /**< Consent lost (RFC 7675) — can recover via restart. */
    NANORTC_ICE_STATE_FAILED,
} nano_ice_state_t;

/* ICE_MAX_CHECKS is now NANORTC_ICE_MAX_CHECKS in nanortc_config.h */

/** @brief ICE candidate type (RFC 8445 §5.1.2.1). */
typedef enum {
    NANORTC_ICE_CAND_HOST = 0,  /**< Local host address. */
    NANORTC_ICE_CAND_SRFLX = 1, /**< Server-reflexive (from STUN). */
    NANORTC_ICE_CAND_RELAY = 2, /**< Relay (from TURN). */
} nano_ice_cand_type_t;

/**
 * @brief ICE candidate priority (RFC 8445 §5.1.2.1).
 *
 * priority = (2^24) * type_pref + (2^8) * local_pref + (2^0) * (256 - component_id)
 * component_id = 1 (RTP), type_pref: host=126, srflx=100, relay=0.
 * local_pref differentiates candidates of the same type: 65535 - index.
 */
#define ICE_HOST_PRIORITY(idx) ((uint32_t)((126u << 24) | ((uint32_t)(65535u - (idx)) << 8) | 255u))

typedef struct nano_ice_candidate {
    uint8_t addr[NANORTC_ADDR_SIZE];
    uint16_t port;
    uint8_t family; /* 4 = IPv4, 6 = IPv6 */
    uint8_t type;   /* nano_ice_cand_type_t */
} nano_ice_candidate_t;

typedef struct nano_ice {
    nano_ice_state_t state;
    int is_controlling; /* 0 = controlled (answerer), 1 = controlling (offerer) */
    char local_ufrag[NANORTC_ICE_UFRAG_SIZE];
    uint16_t local_ufrag_len;
    char local_pwd[NANORTC_ICE_PWD_SIZE];
    uint16_t local_pwd_len;
    char remote_ufrag[NANORTC_ICE_REMOTE_UFRAG_SIZE];
    uint16_t remote_ufrag_len;
    char remote_pwd[NANORTC_ICE_REMOTE_PWD_SIZE];
    uint16_t remote_pwd_len;
    uint8_t selected_addr[NANORTC_ADDR_SIZE];
    uint16_t selected_port;
    uint8_t selected_family;
    uint8_t selected_type;      /**< nano_ice_cand_type_t of selected pair. */
    uint32_t check_interval_ms; /* for controlling role: STUN check pacing */
    uint32_t next_check_ms;

    /* Controlling role state */
    uint64_t tie_breaker;              /* 8-byte random for ICE-CONTROLLING/CONTROLLED */
    uint8_t last_txid[STUN_TXID_SIZE]; /* transaction ID of last outgoing request */
    uint8_t check_count;               /* number of checks sent */
    bool nominated;                    /* selected pair nominated */

    /* Local candidates array */
    nano_ice_candidate_t local_candidates[NANORTC_MAX_LOCAL_CANDIDATES];
    uint8_t local_candidate_count;

    /* Selected pair — local side */
    uint8_t selected_local_addr[NANORTC_ADDR_SIZE];
    uint16_t selected_local_port;
    uint8_t selected_local_family;

    /* Remote candidates array (trickle ICE support) */
    nano_ice_candidate_t remote_candidates[NANORTC_MAX_ICE_CANDIDATES];
    uint8_t remote_candidate_count;

    /* Pair iteration state (controlling role) */
    uint8_t current_local;   /* index of local candidate being checked */
    uint8_t current_remote;  /* index of remote candidate being checked */
    uint8_t last_local_idx;  /* local index used in last outgoing check */
    uint8_t last_remote_idx; /* remote index used in last outgoing check */

    /* Trickle ICE (RFC 8838) */
    bool end_of_candidates; /**< Remote signaled no more candidates. */
    uint8_t generation;     /**< ICE generation counter (bumped on restart). */

    /* Consent freshness (RFC 7675) */
    uint32_t consent_next_ms;             /**< When to send next consent check. */
    uint32_t consent_expiry_ms;           /**< Consent fails if no response by this time. */
    uint8_t consent_txid[STUN_TXID_SIZE]; /**< Transaction ID of last consent request. */
    bool consent_pending;                 /**< True if a consent check is awaiting response. */
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

/**
 * Reset ICE state for a restart (RFC 8445 §9).
 * Clears remote candidates, resets state to NEW, bumps generation.
 * Preserves local credentials (caller must regenerate and set them).
 */
int ice_restart(nano_ice_t *ice);

/**
 * Generate a consent freshness STUN Binding Request (RFC 7675).
 * Called periodically after ICE is CONNECTED to verify path liveness.
 * Returns NANORTC_OK with *out_len=0 if not time to send yet.
 */
int ice_generate_consent(nano_ice_t *ice, uint32_t now_ms, const nanortc_crypto_provider_t *crypto,
                         uint8_t *buf, size_t buf_len, size_t *out_len);

/**
 * Check consent freshness timeout (RFC 7675).
 * Returns true if consent has expired (connection should be considered lost).
 */
bool ice_consent_expired(const nano_ice_t *ice, uint32_t now_ms);

#endif /* NANORTC_ICE_H_ */
