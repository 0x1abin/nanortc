/*
 * nanortc — TURN client internal interface (RFC 5766 / RFC 8656)
 * @internal Not part of the public API.
 *
 * Minimal TURN client: Allocate, Refresh, CreatePermission, Send/Data indication.
 * ChannelBind is deferred (Send/Data indication sufficient for MVP).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_TURN_H_
#define NANORTC_TURN_H_

#include "nanortc_config.h"
#include "nano_stun.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward declarations */
#ifndef NANORTC_CRYPTO_PROVIDER_T_DECLARED
#define NANORTC_CRYPTO_PROVIDER_T_DECLARED
typedef struct nanortc_crypto_provider nanortc_crypto_provider_t;
#endif

/** @brief TURN client state machine. */
typedef enum {
    NANORTC_TURN_IDLE,        /**< Not configured or not started. */
    NANORTC_TURN_ALLOCATING,  /**< Initial Allocate sent, awaiting response. */
    NANORTC_TURN_CHALLENGED,  /**< Got 401, re-sending with credentials. */
    NANORTC_TURN_ALLOCATED,   /**< Have relay address. */
    NANORTC_TURN_FAILED,      /**< Unrecoverable error. */
} nano_turn_state_t;

/** @brief TURN client state. Embedded in nanortc_t. */
typedef struct nano_turn {
    nano_turn_state_t state;
    bool configured; /**< True if server address + credentials are set. */

    /* Server address */
    uint8_t server_addr[NANORTC_ADDR_SIZE];
    uint16_t server_port;
    uint8_t server_family; /* 4 or 6 */

    /* Credentials (passed by application) */
    char username[NANORTC_TURN_USERNAME_SIZE];
    size_t username_len;
    char password[NANORTC_TURN_PASSWORD_SIZE];
    size_t password_len;

    /* From 401 challenge */
    char realm[NANORTC_TURN_REALM_SIZE];
    size_t realm_len;
    char nonce[NANORTC_TURN_NONCE_SIZE];
    size_t nonce_len;

    /* Derived HMAC key: MD5(username:realm:password) */
    uint8_t hmac_key[16];
    bool hmac_key_valid;

    /* Relay allocation result */
    uint8_t relay_addr[NANORTC_ADDR_SIZE];
    uint16_t relay_port;
    uint8_t relay_family;
    uint32_t lifetime_s;

    /* Timing */
    uint32_t refresh_at_ms;    /**< When to send next Refresh. */
    uint32_t permission_at_ms; /**< When to refresh permissions. */

    /* Permissions (peer addresses we've authorized) */
    struct {
        uint8_t addr[NANORTC_ADDR_SIZE];
        uint16_t port;
        uint8_t family;
        bool active;
    } permissions[NANORTC_TURN_MAX_PERMISSIONS];
    uint8_t permission_count;

    /* Transaction tracking */
    uint8_t last_txid[STUN_TXID_SIZE];
    uint8_t alloc_retries;
} nano_turn_t;

/** Initialize TURN state. */
int turn_init(nano_turn_t *turn);

/**
 * Configure TURN server credentials.
 * Does NOT start allocation — call turn_start_allocate() after.
 */
int turn_configure(nano_turn_t *turn, const uint8_t *server_addr, uint8_t server_family,
                   uint16_t server_port, const char *username, size_t username_len,
                   const char *password, size_t password_len);

/**
 * Generate an Allocate request.
 * First call sends unauthenticated; after 401, sends with credentials.
 */
int turn_start_allocate(nano_turn_t *turn, const nanortc_crypto_provider_t *crypto,
                        uint8_t *buf, size_t buf_len, size_t *out_len);

/**
 * Handle a STUN response from the TURN server.
 * Processes Allocate success/error, Refresh, CreatePermission responses.
 * Returns NANORTC_OK on success.
 */
int turn_handle_response(nano_turn_t *turn, const uint8_t *data, size_t len,
                         const nanortc_crypto_provider_t *crypto);

/**
 * Generate a Refresh request (keep allocation alive).
 * Returns NANORTC_OK with *out_len=0 if not time yet.
 */
int turn_generate_refresh(nano_turn_t *turn, uint32_t now_ms,
                          const nanortc_crypto_provider_t *crypto,
                          uint8_t *buf, size_t buf_len, size_t *out_len);

/**
 * Generate a CreatePermission request for a peer address.
 */
int turn_create_permission(nano_turn_t *turn, const uint8_t *peer_addr, uint8_t peer_family,
                           uint16_t peer_port, const nanortc_crypto_provider_t *crypto,
                           uint8_t *buf, size_t buf_len, size_t *out_len);

/**
 * Wrap outgoing data in a TURN Send indication.
 * @param peer_addr Destination peer address (binary).
 * @param peer_family 4 or 6.
 * @param peer_port Destination port.
 * @param payload Application data to relay.
 * @param payload_len Payload length.
 * @param buf Output buffer for the Send indication.
 * @param buf_len Output buffer size.
 * @param out_len Actual output length.
 */
int turn_wrap_send(const uint8_t *peer_addr, uint8_t peer_family, uint16_t peer_port,
                   const uint8_t *payload, size_t payload_len,
                   uint8_t *buf, size_t buf_len, size_t *out_len);

/**
 * Unwrap a TURN Data indication.
 * @param data Incoming STUN Data indication.
 * @param len Data length.
 * @param peer_addr Output: peer address.
 * @param peer_family Output: peer family.
 * @param peer_port Output: peer port.
 * @param payload Output: pointer to unwrapped payload (into data buffer).
 * @param payload_len Output: payload length.
 */
int turn_unwrap_data(const uint8_t *data, size_t len,
                     uint8_t *peer_addr, uint8_t *peer_family, uint16_t *peer_port,
                     const uint8_t **payload, size_t *payload_len);

/** Check if a STUN message is from the configured TURN server. */
bool turn_is_from_server(const nano_turn_t *turn, const uint8_t *src_addr,
                         uint8_t src_family, uint16_t src_port);

#endif /* NANORTC_TURN_H_ */
