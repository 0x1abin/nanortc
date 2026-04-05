/*
 * nanortc — TURN client (RFC 5766 / RFC 8656)
 *
 * Minimal TURN: Allocate, Refresh, CreatePermission, Send/Data indication.
 * Sans I/O: produces STUN messages in caller-provided buffers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_turn.h"
#include "nano_stun.h"
#include "nano_log.h"
#include "nanortc_crypto.h"
#include "nanortc.h"
#include <string.h>

/* Default TURN allocation lifetime (seconds, RFC 5766 §6.2) */
#define TURN_DEFAULT_LIFETIME 600

/* Refresh margin: refresh 60 seconds before expiry */
#define TURN_REFRESH_MARGIN_MS 60000

/* Permission refresh: every 4 minutes (RFC 5766 §8: permissions expire at 5 min) */
#define TURN_PERMISSION_INTERVAL_MS 240000

/* REQUESTED-TRANSPORT value for UDP (RFC 5766 §14.7) */
#define TURN_TRANSPORT_UDP 17

/* ----------------------------------------------------------------
 * STUN message encoding helpers
 *
 * These are simplified builders for TURN-specific messages.
 * They produce complete STUN messages with TURN attributes.
 * ---------------------------------------------------------------- */

/* Write a STUN attribute header + value. Returns total bytes written (including padding). */
static size_t stun_write_attr(uint8_t *buf, uint16_t type, const void *val, uint16_t val_len)
{
    nanortc_write_u16be(buf, type);
    nanortc_write_u16be(buf + 2, val_len);
    if (val_len > 0) {
        memcpy(buf + 4, val, val_len);
    }
    /* Pad to 4-byte boundary */
    size_t padded = (val_len + 3) & ~3u;
    if (padded > val_len) {
        memset(buf + 4 + val_len, 0, padded - val_len);
    }
    return 4 + padded;
}

/* Encode XOR address attribute (RFC 8489 §14.2) */
static size_t stun_encode_xor_addr(uint8_t *buf, uint16_t attr_type, const uint8_t *addr,
                                   uint8_t family, uint16_t port,
                                   const uint8_t txid[STUN_TXID_SIZE])
{
    uint8_t val[20]; /* max: 4-byte header + 16-byte IPv6 */
    size_t val_len;

    val[0] = 0; /* reserved */
    val[1] = (family == 4) ? STUN_FAMILY_IPV4 : STUN_FAMILY_IPV6;
    nanortc_write_u16be(val + 2, port ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));

    if (family == 4) {
        uint32_t raw = nanortc_read_u32be(addr);
        nanortc_write_u32be(val + 4, raw ^ STUN_MAGIC_COOKIE);
        val_len = 8;
    } else {
        uint8_t mask[16];
        nanortc_write_u32be(mask, STUN_MAGIC_COOKIE);
        memcpy(mask + 4, txid, STUN_TXID_SIZE);
        for (int i = 0; i < 16; i++) {
            val[4 + i] = addr[i] ^ mask[i];
        }
        val_len = 20;
    }

    return stun_write_attr(buf, attr_type, val, (uint16_t)val_len);
}

/* Write STUN header. Caller fills in length after building attributes. */
static void stun_write_header(uint8_t *buf, uint16_t type, const uint8_t txid[STUN_TXID_SIZE])
{
    nanortc_write_u16be(buf, type);
    nanortc_write_u16be(buf + 2, 0); /* length placeholder */
    nanortc_write_u32be(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, STUN_TXID_SIZE);
}

/* Finalize STUN message: set length field. */
static void stun_finalize_length(uint8_t *buf, size_t total_len)
{
    nanortc_write_u16be(buf + 2, (uint16_t)(total_len - STUN_HEADER_SIZE));
}

/* Append MESSAGE-INTEGRITY (RFC 8489 §14.5) using the TURN HMAC key. */
static size_t stun_append_integrity(uint8_t *buf, size_t msg_len,
                                    const uint8_t key[NANORTC_TURN_HMAC_KEY_SIZE],
                                    stun_hmac_sha1_fn hmac_sha1)
{
    /* Temporarily set length to include MI attr (24 bytes: 4 header + 20 HMAC) */
    nanortc_write_u16be(buf + 2, (uint16_t)(msg_len - STUN_HEADER_SIZE + 24));

    uint8_t hmac[20];
    hmac_sha1(key, 16, buf, msg_len, hmac);

    /* Write MI attribute */
    size_t pos = msg_len;
    nanortc_write_u16be(buf + pos, STUN_ATTR_MESSAGE_INTEGRITY);
    nanortc_write_u16be(buf + pos + 2, 20);
    memcpy(buf + pos + 4, hmac, 20);
    pos += 24;

    /* Fix length to final value */
    stun_finalize_length(buf, pos);
    return pos;
}

/* ----------------------------------------------------------------
 * Derive HMAC key: MD5(username:realm:password)
 * RFC 8489 §9.2.2 — long-term credential mechanism
 * ---------------------------------------------------------------- */

static int turn_derive_key(nano_turn_t *turn, const nanortc_crypto_provider_t *crypto)
{
    if (!crypto->md5) {
        NANORTC_LOGE("TURN", "MD5 not available in crypto provider");
        return NANORTC_ERR_CRYPTO;
    }

    /* Build "username:realm:password" */
    uint8_t input[NANORTC_TURN_USERNAME_SIZE + NANORTC_TURN_REALM_SIZE +
                  NANORTC_TURN_PASSWORD_SIZE + 3];
    size_t pos = 0;

    memcpy(input + pos, turn->username, turn->username_len);
    pos += turn->username_len;
    input[pos++] = ':';
    memcpy(input + pos, turn->realm, turn->realm_len);
    pos += turn->realm_len;
    input[pos++] = ':';
    memcpy(input + pos, turn->password, turn->password_len);
    pos += turn->password_len;

    crypto->md5(input, pos, turn->hmac_key);
    turn->hmac_key_valid = true;

    NANORTC_LOGD("TURN", "HMAC key derived");
    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * Public functions
 * ---------------------------------------------------------------- */

int turn_init(nano_turn_t *turn)
{
    if (!turn) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(turn, 0, sizeof(*turn));
    turn->state = NANORTC_TURN_IDLE;
    return NANORTC_OK;
}

int turn_configure(nano_turn_t *turn, const uint8_t *server_addr, uint8_t server_family,
                   uint16_t server_port, const char *username, size_t username_len,
                   const char *password, size_t password_len)
{
    if (!turn || !server_addr || !username || !password) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    if (username_len >= NANORTC_TURN_USERNAME_SIZE || password_len >= NANORTC_TURN_PASSWORD_SIZE) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(turn->server_addr, server_addr, NANORTC_ADDR_SIZE);
    turn->server_family = server_family;
    turn->server_port = server_port;

    memcpy(turn->username, username, username_len);
    turn->username[username_len] = '\0';
    turn->username_len = username_len;

    memcpy(turn->password, password, password_len);
    turn->password[password_len] = '\0';
    turn->password_len = password_len;

    turn->configured = true;
    NANORTC_LOGI("TURN", "configured");
    return NANORTC_OK;
}

int turn_start_allocate(nano_turn_t *turn, const nanortc_crypto_provider_t *crypto, uint8_t *buf,
                        size_t buf_len, size_t *out_len)
{
    if (!turn || !crypto || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    *out_len = 0;

    if (!turn->configured) {
        return NANORTC_ERR_STATE;
    }

    /* Generate transaction ID */
    if (crypto->random_bytes(turn->last_txid, STUN_TXID_SIZE) != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    /* Build Allocate Request */
    if (buf_len < 256) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    stun_write_header(buf, STUN_ALLOCATE_REQUEST, turn->last_txid);
    size_t pos = STUN_HEADER_SIZE;

    /* REQUESTED-TRANSPORT: UDP (RFC 5766 §14.7) */
    uint8_t transport[4] = {TURN_TRANSPORT_UDP, 0, 0, 0};
    pos += stun_write_attr(buf + pos, STUN_ATTR_REQUESTED_TRANSPORT, transport, 4);

    /* LIFETIME */
    uint8_t lifetime_val[4];
    nanortc_write_u32be(lifetime_val, TURN_DEFAULT_LIFETIME);
    pos += stun_write_attr(buf + pos, STUN_ATTR_LIFETIME, lifetime_val, 4);

    if (turn->state == NANORTC_TURN_CHALLENGED && turn->hmac_key_valid) {
        /* Authenticated retry after 401: add USERNAME, REALM, NONCE, MI */
        pos += stun_write_attr(buf + pos, STUN_ATTR_USERNAME, turn->username,
                               (uint16_t)turn->username_len);
        pos += stun_write_attr(buf + pos, STUN_ATTR_REALM, turn->realm, (uint16_t)turn->realm_len);
        pos += stun_write_attr(buf + pos, STUN_ATTR_NONCE, turn->nonce, (uint16_t)turn->nonce_len);

        stun_finalize_length(buf, pos);
        pos = stun_append_integrity(buf, pos, turn->hmac_key, crypto->hmac_sha1);
    } else {
        /* First attempt: unauthenticated */
        stun_finalize_length(buf, pos);
        turn->state = NANORTC_TURN_ALLOCATING;
    }

    *out_len = pos;
    NANORTC_LOGD("TURN", "allocate request sent");
    return NANORTC_OK;
}

int turn_handle_response(nano_turn_t *turn, const uint8_t *data, size_t len,
                         const nanortc_crypto_provider_t *crypto)
{
    if (!turn || !data || !crypto) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    stun_msg_t msg;
    int rc = stun_parse(data, len, &msg);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* Verify transaction ID */
    if (memcmp(msg.transaction_id, turn->last_txid, STUN_TXID_SIZE) != 0) {
        return NANORTC_ERR_PROTOCOL; /* Not our transaction */
    }

    if (msg.type == STUN_ALLOCATE_RESPONSE) {
        /* Success: extract relay address and lifetime */
        if (msg.relayed_family == 0) {
            NANORTC_LOGW("TURN", "allocate response missing XOR-RELAYED-ADDRESS");
            return NANORTC_ERR_PROTOCOL;
        }

        memcpy(turn->relay_addr, msg.relayed_addr, NANORTC_ADDR_SIZE);
        turn->relay_port = msg.relayed_port;
        turn->relay_family = msg.relayed_family;
        turn->lifetime_s = msg.lifetime > 0 ? msg.lifetime : TURN_DEFAULT_LIFETIME;
        turn->state = NANORTC_TURN_ALLOCATED;

        NANORTC_LOGI("TURN", "allocated");
        return NANORTC_OK;

    } else if (msg.type == STUN_ALLOCATE_ERROR) {
        if (msg.error_code == 401 && turn->state == NANORTC_TURN_ALLOCATING) {
            /* 401 Unauthorized: extract REALM + NONCE, derive key, retry */
            if (!msg.realm || !msg.nonce) {
                NANORTC_LOGW("TURN", "401 missing realm/nonce");
                turn->state = NANORTC_TURN_FAILED;
                return NANORTC_ERR_PROTOCOL;
            }

            /* Store realm and nonce */
            size_t rlen = msg.realm_len < NANORTC_TURN_REALM_SIZE - 1 ? msg.realm_len
                                                                      : NANORTC_TURN_REALM_SIZE - 1;
            memcpy(turn->realm, msg.realm, rlen);
            turn->realm[rlen] = '\0';
            turn->realm_len = rlen;

            size_t nlen = msg.nonce_len < NANORTC_TURN_NONCE_SIZE - 1 ? msg.nonce_len
                                                                      : NANORTC_TURN_NONCE_SIZE - 1;
            memcpy(turn->nonce, msg.nonce, nlen);
            turn->nonce[nlen] = '\0';
            turn->nonce_len = nlen;

            /* Derive HMAC key */
            rc = turn_derive_key(turn, crypto);
            if (rc != NANORTC_OK) {
                turn->state = NANORTC_TURN_FAILED;
                return rc;
            }

            turn->state = NANORTC_TURN_CHALLENGED;
            NANORTC_LOGD("TURN", "401 challenge received");
            return NANORTC_OK;

        } else if (msg.error_code == 438) {
            /* 438 Stale Nonce: update nonce, retry */
            if (msg.nonce && msg.nonce_len > 0) {
                size_t nlen = msg.nonce_len < NANORTC_TURN_NONCE_SIZE - 1
                                  ? msg.nonce_len
                                  : NANORTC_TURN_NONCE_SIZE - 1;
                memcpy(turn->nonce, msg.nonce, nlen);
                turn->nonce[nlen] = '\0';
                turn->nonce_len = nlen;
                turn->state = NANORTC_TURN_CHALLENGED;
                NANORTC_LOGD("TURN", "438 stale nonce, retrying");
                return NANORTC_OK;
            }
            turn->state = NANORTC_TURN_FAILED;
            return NANORTC_ERR_PROTOCOL;

        } else {
            NANORTC_LOGW("TURN", "allocate error");
            turn->state = NANORTC_TURN_FAILED;
            return NANORTC_ERR_PROTOCOL;
        }

    } else if (msg.type == STUN_REFRESH_RESPONSE) {
        turn->lifetime_s = msg.lifetime > 0 ? msg.lifetime : TURN_DEFAULT_LIFETIME;
        NANORTC_LOGD("TURN", "refreshed");
        return NANORTC_OK;

    } else if (msg.type == STUN_REFRESH_ERROR) {
        if (msg.error_code == 438 && msg.nonce) {
            /* Stale nonce on refresh — update and retry */
            size_t nlen = msg.nonce_len < NANORTC_TURN_NONCE_SIZE - 1 ? msg.nonce_len
                                                                      : NANORTC_TURN_NONCE_SIZE - 1;
            memcpy(turn->nonce, msg.nonce, nlen);
            turn->nonce[nlen] = '\0';
            turn->nonce_len = nlen;
            turn->refresh_at_ms = 0; /* Retry immediately */
            return NANORTC_OK;
        }
        NANORTC_LOGW("TURN", "refresh error");
        turn->state = NANORTC_TURN_FAILED;
        return NANORTC_ERR_PROTOCOL;

    } else if (msg.type == STUN_CREATE_PERMISSION_RESPONSE) {
        NANORTC_LOGD("TURN", "permission created");
        return NANORTC_OK;

    } else if (msg.type == STUN_CREATE_PERMISSION_ERROR) {
        NANORTC_LOGW("TURN", "permission error");
        return NANORTC_ERR_PROTOCOL;
    }

    return NANORTC_ERR_PROTOCOL;
}

int turn_generate_refresh(nano_turn_t *turn, uint32_t now_ms,
                          const nanortc_crypto_provider_t *crypto, uint8_t *buf, size_t buf_len,
                          size_t *out_len)
{
    if (!turn || !crypto || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    *out_len = 0;

    if (turn->state != NANORTC_TURN_ALLOCATED) {
        return NANORTC_OK;
    }
    if (turn->refresh_at_ms != 0 && now_ms < turn->refresh_at_ms) {
        return NANORTC_OK;
    }

    /* Generate txid */
    if (crypto->random_bytes(turn->last_txid, STUN_TXID_SIZE) != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    if (buf_len < 256) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    stun_write_header(buf, STUN_REFRESH_REQUEST, turn->last_txid);
    size_t pos = STUN_HEADER_SIZE;

    /* LIFETIME */
    uint8_t lifetime_val[4];
    nanortc_write_u32be(lifetime_val, TURN_DEFAULT_LIFETIME);
    pos += stun_write_attr(buf + pos, STUN_ATTR_LIFETIME, lifetime_val, 4);

    /* Authentication */
    pos += stun_write_attr(buf + pos, STUN_ATTR_USERNAME, turn->username,
                           (uint16_t)turn->username_len);
    pos += stun_write_attr(buf + pos, STUN_ATTR_REALM, turn->realm, (uint16_t)turn->realm_len);
    pos += stun_write_attr(buf + pos, STUN_ATTR_NONCE, turn->nonce, (uint16_t)turn->nonce_len);

    stun_finalize_length(buf, pos);
    pos = stun_append_integrity(buf, pos, turn->hmac_key, crypto->hmac_sha1);

    *out_len = pos;

    /* Schedule next refresh */
    uint32_t margin = turn->lifetime_s > 60 ? TURN_REFRESH_MARGIN_MS : (turn->lifetime_s * 500);
    turn->refresh_at_ms = now_ms + (turn->lifetime_s * 1000) - margin;

    return NANORTC_OK;
}

int turn_create_permission(nano_turn_t *turn, const uint8_t *peer_addr, uint8_t peer_family,
                           uint16_t peer_port, const nanortc_crypto_provider_t *crypto,
                           uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!turn || !peer_addr || !crypto || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    *out_len = 0;

    if (turn->state != NANORTC_TURN_ALLOCATED || !turn->hmac_key_valid) {
        return NANORTC_ERR_STATE;
    }

    /* Generate txid */
    uint8_t txid[STUN_TXID_SIZE];
    if (crypto->random_bytes(txid, STUN_TXID_SIZE) != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    if (buf_len < 256) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    stun_write_header(buf, STUN_CREATE_PERMISSION_REQUEST, txid);
    size_t pos = STUN_HEADER_SIZE;

    /* XOR-PEER-ADDRESS */
    pos += stun_encode_xor_addr(buf + pos, STUN_ATTR_XOR_PEER_ADDRESS, peer_addr, peer_family,
                                peer_port, txid);

    /* Authentication */
    pos += stun_write_attr(buf + pos, STUN_ATTR_USERNAME, turn->username,
                           (uint16_t)turn->username_len);
    pos += stun_write_attr(buf + pos, STUN_ATTR_REALM, turn->realm, (uint16_t)turn->realm_len);
    pos += stun_write_attr(buf + pos, STUN_ATTR_NONCE, turn->nonce, (uint16_t)turn->nonce_len);

    stun_finalize_length(buf, pos);
    pos = stun_append_integrity(buf, pos, turn->hmac_key, crypto->hmac_sha1);

    *out_len = pos;

    /* Track permission */
    if (turn->permission_count < NANORTC_TURN_MAX_PERMISSIONS) {
        uint8_t pi = turn->permission_count;
        memcpy(turn->permissions[pi].addr, peer_addr, NANORTC_ADDR_SIZE);
        turn->permissions[pi].port = peer_port;
        turn->permissions[pi].family = peer_family;
        turn->permissions[pi].active = true;
        turn->permission_count++;
    }

    NANORTC_LOGD("TURN", "permission request sent");
    return NANORTC_OK;
}

int turn_wrap_send(const uint8_t *peer_addr, uint8_t peer_family, uint16_t peer_port,
                   const uint8_t *payload, size_t payload_len, uint8_t *buf, size_t buf_len,
                   size_t *out_len)
{
    if (!peer_addr || !payload || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    *out_len = 0;

    /* Estimate needed size: header(20) + XOR-PEER-ADDRESS(12/28) + DATA(4+payload+pad) */
    size_t needed = STUN_HEADER_SIZE + 32 + 4 + ((payload_len + 3) & ~3u);
    if (buf_len < needed) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }

    /* Send indication doesn't need authentication — use zero txid */
    uint8_t txid[STUN_TXID_SIZE];
    memset(txid, 0, sizeof(txid));

    stun_write_header(buf, STUN_SEND_INDICATION, txid);
    size_t pos = STUN_HEADER_SIZE;

    /* XOR-PEER-ADDRESS */
    pos += stun_encode_xor_addr(buf + pos, STUN_ATTR_XOR_PEER_ADDRESS, peer_addr, peer_family,
                                peer_port, txid);

    /* DATA */
    pos += stun_write_attr(buf + pos, STUN_ATTR_DATA, payload, (uint16_t)payload_len);

    stun_finalize_length(buf, pos);
    *out_len = pos;
    return NANORTC_OK;
}

int turn_unwrap_data(const uint8_t *data, size_t len, uint8_t *peer_addr, uint8_t *peer_family,
                     uint16_t *peer_port, const uint8_t **payload, size_t *payload_len)
{
    if (!data || !peer_addr || !peer_family || !peer_port || !payload || !payload_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    stun_msg_t msg;
    int rc = stun_parse(data, len, &msg);
    if (rc != NANORTC_OK) {
        return rc;
    }

    if (msg.type != STUN_DATA_INDICATION) {
        return NANORTC_ERR_PROTOCOL;
    }

    if (msg.peer_family == 0 || !msg.data_attr || msg.data_attr_len == 0) {
        return NANORTC_ERR_PROTOCOL;
    }

    memcpy(peer_addr, msg.peer_addr, NANORTC_ADDR_SIZE);
    *peer_family = msg.peer_family;
    *peer_port = msg.peer_port;
    *payload = msg.data_attr;
    *payload_len = msg.data_attr_len;

    return NANORTC_OK;
}

bool turn_is_from_server(const nano_turn_t *turn, const uint8_t *src_addr, uint8_t src_family,
                         uint16_t src_port)
{
    if (!turn || !turn->configured) {
        return false;
    }
    if (src_family != turn->server_family || src_port != turn->server_port) {
        return false;
    }
    size_t addr_len = (src_family == 4) ? 4 : 16;
    return memcmp(src_addr, turn->server_addr, addr_len) == 0;
}
