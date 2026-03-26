/*
 * nanortc — STUN message codec internal interface
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_STUN_H_
#define NANO_STUN_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* STUN message types */
#define STUN_BINDING_REQUEST       0x0001
#define STUN_BINDING_RESPONSE      0x0101
#define STUN_BINDING_ERROR         0x0111

/* STUN attribute types */
#define STUN_ATTR_MAPPED_ADDRESS       0x0001
#define STUN_ATTR_USERNAME             0x0006
#define STUN_ATTR_MESSAGE_INTEGRITY    0x0008
#define STUN_ATTR_XOR_MAPPED_ADDRESS   0x0020
#define STUN_ATTR_PRIORITY             0x0024
#define STUN_ATTR_USE_CANDIDATE        0x0025
#define STUN_ATTR_ICE_CONTROLLED       0x8029
#define STUN_ATTR_ICE_CONTROLLING      0x802A
#define STUN_ATTR_FINGERPRINT          0x8028

#define STUN_HEADER_SIZE    20
#define STUN_MAGIC_COOKIE   0x2112A442

typedef struct stun_msg {
    uint16_t type;
    uint16_t length;
    uint8_t transaction_id[12];
    /* Parsed attributes */
    const char *username;
    size_t username_len;
    uint32_t priority;
    bool use_candidate;
    bool has_fingerprint;
    bool has_integrity;
} stun_msg_t;

int stun_parse(const uint8_t *data, size_t len, stun_msg_t *msg);
int stun_encode_binding_response(const stun_msg_t *req,
                                 const uint8_t *src_addr, uint8_t src_family,
                                 uint16_t src_port,
                                 const uint8_t *key, size_t key_len,
                                 uint8_t *buf, size_t buf_len, size_t *out_len);

#endif /* NANO_STUN_H_ */
