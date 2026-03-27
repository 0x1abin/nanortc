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
#include "nano_crypto.h"
#include "nanortc.h"
#include <string.h>

/* ICE check interval is now NANO_ICE_CHECK_INTERVAL_MS in nanortc_config.h */

/* ICE candidate priority for host candidate, component 1 (RFC 8445 §5.1.2.1) */
#define ICE_HOST_PRIORITY ((uint32_t)((126 << 24) | (65535 << 8) | 255))

/* ----------------------------------------------------------------
 * ice_init
 * ---------------------------------------------------------------- */

int ice_init(nano_ice_t *ice, int is_controlling)
{
    if (!ice) {
        return NANO_ERR_INVALID_PARAM;
    }
    memset(ice, 0, sizeof(*ice));
    ice->state = NANO_ICE_STATE_NEW;
    ice->is_controlling = is_controlling;
    ice->check_interval_ms = NANO_ICE_CHECK_INTERVAL_MS;
    return NANO_OK;
}

/* ----------------------------------------------------------------
 * ice_is_stun — RFC 7983
 * ---------------------------------------------------------------- */

bool ice_is_stun(const uint8_t *data, size_t len)
{
    if (!data || len < 1) {
        return false;
    }
    /* RFC 7983: STUN messages start with 0x00-0x03 */
    return data[0] <= 0x03;
}

/* ----------------------------------------------------------------
 * Username validation helper
 *
 * RFC 8445 §7.2.1.1: incoming request USERNAME must be "local:remote".
 * We verify the local_ufrag matches the portion before ':'.
 * ---------------------------------------------------------------- */

static bool ice_verify_username(const nano_ice_t *ice, const stun_msg_t *msg)
{
    if (!msg->username || msg->username_len == 0) {
        return false;
    }

    /* Find the ':' separator */
    const char *colon = NULL;
    for (size_t i = 0; i < msg->username_len; i++) {
        if (msg->username[i] == ':') {
            colon = &msg->username[i];
            break;
        }
    }
    if (!colon) {
        return false;
    }

    size_t local_len = (size_t)(colon - msg->username);
    size_t local_ufrag_len = ice->local_ufrag_len;

    if (local_len != local_ufrag_len) {
        return false;
    }
    return memcmp(msg->username, ice->local_ufrag, local_len) == 0;
}

/* Map nano_addr_t family to STUN family constant */
static uint8_t addr_to_stun_family(uint8_t addr_family)
{
    if (addr_family == 4) {
        return STUN_FAMILY_IPV4;
    }
    if (addr_family == 6) {
        return STUN_FAMILY_IPV6;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * ice_handle_stun — process incoming STUN message (both roles)
 * ---------------------------------------------------------------- */

int ice_handle_stun(nano_ice_t *ice, const uint8_t *data, size_t len, const nano_addr_t *src,
                    const nano_crypto_provider_t *crypto, uint8_t *resp_buf, size_t resp_buf_len,
                    size_t *resp_len)
{
    if (!ice || !data || !src || !crypto || !resp_buf || !resp_len) {
        return NANO_ERR_INVALID_PARAM;
    }

    *resp_len = 0;
    stun_msg_t msg;

    int rc = stun_parse(data, len, &msg);
    if (rc != NANO_OK) {
        return rc;
    }

    if (msg.type == STUN_BINDING_REQUEST) {
        /*
         * Incoming Binding Request — both roles respond.
         *
         * RFC 8445 §7.2.1: Verify FINGERPRINT, USERNAME, MESSAGE-INTEGRITY.
         * MI key = our local_pwd (remote signed with our password).
         */
        if (msg.has_fingerprint) {
            rc = stun_verify_fingerprint(data, len);
            if (rc != NANO_OK) {
                return rc;
            }
        }

        if (!ice_verify_username(ice, &msg)) {
            return NANO_ERR_PROTOCOL;
        }

        if (msg.has_integrity) {
            rc = stun_verify_integrity(data, len, &msg, (const uint8_t *)ice->local_pwd,
                                       ice->local_pwd_len, crypto->hmac_sha1);
            if (rc != NANO_OK) {
                return rc;
            }
        }

        /* Encode Binding Response — sign with our local_pwd (RFC 8445 §7.2.2) */
        uint8_t stun_family = addr_to_stun_family(src->family);
        if (stun_family == 0) {
            return NANO_ERR_INVALID_PARAM;
        }

        rc = stun_encode_binding_response(&msg, src->addr, stun_family, src->port,
                                          (const uint8_t *)ice->local_pwd, ice->local_pwd_len,
                                          crypto->hmac_sha1, resp_buf, resp_buf_len, resp_len);
        if (rc != NANO_OK) {
            return rc;
        }

        /*
         * RFC 8445 §7.2.1.4: If USE-CANDIDATE is present and we are
         * controlled, nominate this pair and transition to CONNECTED.
         */
        if (msg.use_candidate && !ice->is_controlling) {
            memcpy(ice->selected_addr, src->addr, NANO_ADDR_SIZE);
            ice->selected_port = src->port;
            ice->selected_family = src->family;
            ice->nominated = true;
            ice->state = NANO_ICE_STATE_CONNECTED;
        }

        return NANO_OK;

    } else if (msg.type == STUN_BINDING_RESPONSE) {
        /*
         * Incoming Binding Response — controlling role only.
         *
         * RFC 8445 §7.1.3: Verify transaction ID matches our request,
         * then verify MESSAGE-INTEGRITY with remote_pwd.
         */
        if (!ice->is_controlling) {
            return NANO_ERR_PROTOCOL;
        }

        /* Verify transaction ID matches our last request */
        if (memcmp(msg.transaction_id, ice->last_txid, STUN_TXID_SIZE) != 0) {
            return NANO_ERR_PROTOCOL;
        }

        if (msg.has_fingerprint) {
            rc = stun_verify_fingerprint(data, len);
            if (rc != NANO_OK) {
                return rc;
            }
        }

        if (msg.has_integrity) {
            rc = stun_verify_integrity(data, len, &msg, (const uint8_t *)ice->remote_pwd,
                                       ice->remote_pwd_len, crypto->hmac_sha1);
            if (rc != NANO_OK) {
                return rc;
            }
        }

        /* ICE connectivity established — record the remote address */
        memcpy(ice->selected_addr, ice->remote_addr, NANO_ADDR_SIZE);
        ice->selected_port = ice->remote_port;
        ice->selected_family = ice->remote_family;
        ice->nominated = true;
        ice->state = NANO_ICE_STATE_CONNECTED;

        /* No response needed for a Binding Response */
        *resp_len = 0;
        return NANO_OK;
    }

    /* Unknown message type */
    return NANO_ERR_PROTOCOL;
}

/* ----------------------------------------------------------------
 * ice_generate_check — controlling role STUN Binding Request
 * ---------------------------------------------------------------- */

int ice_generate_check(nano_ice_t *ice, uint32_t now_ms, const nano_crypto_provider_t *crypto,
                       uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!ice || !crypto || !buf || !out_len) {
        return NANO_ERR_INVALID_PARAM;
    }

    *out_len = 0;

    /* Only controlling role generates checks */
    if (!ice->is_controlling) {
        return NANO_OK;
    }

    /* Don't generate checks once connected or failed */
    if (ice->state == NANO_ICE_STATE_CONNECTED || ice->state == NANO_ICE_STATE_FAILED) {
        return NANO_OK;
    }

    /* Pacing: don't send before next_check_ms */
    if (ice->state == NANO_ICE_STATE_CHECKING && now_ms < ice->next_check_ms) {
        return NANO_OK;
    }

    /* Check count limit */
    if (ice->check_count >= NANO_ICE_MAX_CHECKS) {
        ice->state = NANO_ICE_STATE_FAILED;
        return NANO_OK;
    }

    /* Generate random transaction ID */
    if (crypto->random_bytes(ice->last_txid, STUN_TXID_SIZE) != 0) {
        return NANO_ERR_CRYPTO;
    }

    /* Build USERNAME = "remote_ufrag:local_ufrag" (RFC 8445 §7.1.1) */
    char username[64];
    size_t rlen = ice->remote_ufrag_len;
    size_t llen = ice->local_ufrag_len;
    if (rlen + 1 + llen >= sizeof(username)) {
        return NANO_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(username, ice->remote_ufrag, rlen);
    username[rlen] = ':';
    memcpy(username + rlen + 1, ice->local_ufrag, llen);
    size_t ulen = rlen + 1 + llen;

    /* Encode Binding Request — sign with remote_pwd (RFC 8445 §7.1.1) */
    int rc = stun_encode_binding_request(
        username, ulen, ICE_HOST_PRIORITY, true, /* use_candidate (controlling) */
        true,                                    /* is_controlling */
        ice->tie_breaker, ice->last_txid, (const uint8_t *)ice->remote_pwd, ice->remote_pwd_len,
        crypto->hmac_sha1, buf, buf_len, out_len);
    if (rc != NANO_OK) {
        return rc;
    }

    ice->check_count++;
    ice->next_check_ms = now_ms + ice->check_interval_ms;

    if (ice->state == NANO_ICE_STATE_NEW) {
        ice->state = NANO_ICE_STATE_CHECKING;
    }

    return NANO_OK;
}
