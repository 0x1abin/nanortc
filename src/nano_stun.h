/*
 * nanortc — STUN message codec internal interface (RFC 8489)
 * @internal Not part of the public API.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_STUN_H_
#define NANORTC_STUN_H_

#include "nanortc_config.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* STUN message types (RFC 8489 §6) */
#define STUN_BINDING_REQUEST    0x0001
#define STUN_BINDING_RESPONSE   0x0101
#define STUN_BINDING_ERROR      0x0111
#define STUN_BINDING_INDICATION 0x0011

/* STUN attribute types (RFC 8489 §18.2, RFC 8445 §20) */
#define STUN_ATTR_MAPPED_ADDRESS     0x0001
#define STUN_ATTR_USERNAME           0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY  0x0008
#define STUN_ATTR_ERROR_CODE         0x0009
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020
#define STUN_ATTR_PRIORITY           0x0024
#define STUN_ATTR_USE_CANDIDATE      0x0025
#define STUN_ATTR_SOFTWARE           0x8022
#define STUN_ATTR_ICE_CONTROLLED     0x8029
#define STUN_ATTR_ICE_CONTROLLING    0x802A
#define STUN_ATTR_FINGERPRINT        0x8028

#define STUN_HEADER_SIZE  20
#define STUN_MAGIC_COOKIE 0x2112A442

/* STUN FINGERPRINT XOR constant — ASCII "STUN" (RFC 8489 §14.7) */
#define STUN_FINGERPRINT_XOR 0x5354554E

/* STUN transaction ID size (RFC 8489 §6) */
#define STUN_TXID_SIZE 12

/* XOR-MAPPED-ADDRESS family (RFC 8489 §14.1) */
#define STUN_FAMILY_IPV4 0x01
#define STUN_FAMILY_IPV6 0x02

/* HMAC-SHA1 function pointer type (decouples STUN from crypto provider) */
typedef void (*stun_hmac_sha1_fn)(const uint8_t *key, size_t key_len, const uint8_t *data,
                                  size_t data_len, uint8_t out[20]);

typedef struct stun_msg {
    uint16_t type;
    uint16_t length; /* payload length from header (excluding 20-byte header) */
    uint8_t transaction_id[STUN_TXID_SIZE];

    /* Parsed attributes */
    const char *username;
    size_t username_len;
    uint32_t priority;
    bool use_candidate;
    bool has_fingerprint;
    bool has_integrity;

    /* For MESSAGE-INTEGRITY verification */
    uint16_t integrity_offset; /* byte offset from msg start to MI attr type field */

    /* For FINGERPRINT verification */
    uint32_t fingerprint_value; /* parsed fingerprint (host order) */

    /* XOR-MAPPED-ADDRESS (RFC 8489 §14.2) */
    uint8_t mapped_addr[NANORTC_ADDR_SIZE]; /* decoded address (4 bytes IPv4, 16 bytes IPv6) */
    uint16_t mapped_port;                   /* decoded port (host order) */
    uint8_t mapped_family;                  /* STUN_FAMILY_IPV4/IPV6, 0 = not present */

    /* ICE role attributes (RFC 8445 §7.1) */
    uint64_t ice_controlling; /* tie-breaker value */
    uint64_t ice_controlled;  /* tie-breaker value */
    bool has_ice_controlling;
    bool has_ice_controlled;

    /* ERROR-CODE (RFC 8489 §14.8) — 0 = not present */
    uint16_t error_code;
} stun_msg_t;

/* Parse STUN message from wire format. Does NOT validate MI or FP. */
int stun_parse(const uint8_t *data, size_t len, stun_msg_t *msg);

/* Verify FINGERPRINT attribute (CRC-32 XOR 0x5354554E).
 * Returns NANORTC_OK or NANORTC_ERR_PROTOCOL. */
int stun_verify_fingerprint(const uint8_t *data, size_t len);

/* Verify MESSAGE-INTEGRITY using HMAC-SHA1 (RFC 8489 §14.5).
 * Returns NANORTC_OK or NANORTC_ERR_PROTOCOL. */
int stun_verify_integrity(const uint8_t *data, size_t len, const stun_msg_t *msg,
                          const uint8_t *key, size_t key_len, stun_hmac_sha1_fn hmac_sha1);

/* Encode STUN Binding Response (RFC 8489 §7.3.1) */
int stun_encode_binding_response(const stun_msg_t *req, const uint8_t *src_addr, uint8_t src_family,
                                 uint16_t src_port, const uint8_t *key, size_t key_len,
                                 stun_hmac_sha1_fn hmac_sha1, uint8_t *buf, size_t buf_len,
                                 size_t *out_len);

/* Encode STUN Binding Request for ICE connectivity check (RFC 8445 §7.1.1) */
int stun_encode_binding_request(const char *username, size_t username_len, uint32_t priority,
                                bool use_candidate, bool is_controlling, uint64_t tie_breaker,
                                const uint8_t transaction_id[STUN_TXID_SIZE], const uint8_t *key,
                                size_t key_len, stun_hmac_sha1_fn hmac_sha1, uint8_t *buf,
                                size_t buf_len, size_t *out_len);

#endif /* NANORTC_STUN_H_ */
