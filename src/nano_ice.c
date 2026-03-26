/*
 * nanortc — ICE agent (RFC 8445)
 *
 * Controlled role (answerer): respond to STUN checks — ICE-Lite behavior.
 * Controlling role (offerer): initiate STUN connectivity checks.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_ice.h"
#include "nano_stun.h"
#include <string.h>

#define ICE_DEFAULT_CHECK_INTERVAL_MS 50

int ice_init(nano_ice_t *ice, int is_controlling)
{
    if (!ice) {
        return -1;
    }
    memset(ice, 0, sizeof(*ice));
    ice->state = NANO_ICE_STATE_NEW;
    ice->is_controlling = is_controlling;
    ice->check_interval_ms = ICE_DEFAULT_CHECK_INTERVAL_MS;
    return 0;
}

int ice_handle_stun(nano_ice_t *ice, const uint8_t *data, size_t len,
                    uint8_t *resp_buf, size_t *resp_len)
{
    (void)ice;
    (void)data;
    (void)len;
    (void)resp_buf;
    (void)resp_len;
    return -1;
}

int ice_generate_check(nano_ice_t *ice, uint32_t now_ms,
                       uint8_t *buf, size_t buf_len, size_t *out_len)
{
    (void)ice;
    (void)now_ms;
    (void)buf;
    (void)buf_len;
    (void)out_len;
    return -1;
}

bool ice_is_stun(const uint8_t *data, size_t len)
{
    if (!data || len < 1) {
        return false;
    }
    /* RFC 7983: STUN messages start with 0x00-0x03 */
    return data[0] <= 0x03;
}
