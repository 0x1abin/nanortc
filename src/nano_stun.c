/*
 * nanortc — STUN message codec (RFC 8489)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_stun.h"
#include <string.h>

int stun_parse(const uint8_t *data, size_t len, stun_msg_t *msg)
{
    (void)data;
    (void)len;
    (void)msg;
    return -1;
}

int stun_encode_binding_response(const stun_msg_t *req,
                                 const uint8_t *src_addr, uint8_t src_family,
                                 uint16_t src_port,
                                 const uint8_t *key, size_t key_len,
                                 uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)req;
    (void)src_addr;
    (void)src_family;
    (void)src_port;
    (void)key;
    (void)key_len;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}
