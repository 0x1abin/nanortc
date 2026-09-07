/*
 * nanortc — ICE agent (RFC 8445)
 *
 * Both roles send ordinary and triggered connectivity checks.
 * The controlling role nominates a pair only after a successful check.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_ice.h"
#include "nano_stun.h"
#include "nano_time.h"
#include "nanortc_crypto.h"
#include "nanortc.h"
#include <string.h>

/* ICE check interval is now NANORTC_ICE_CHECK_INTERVAL_MS in nanortc_config.h */

/* ICE_HOST_PRIORITY(idx) is now in nano_ice.h */

/* ----------------------------------------------------------------
 * ice_init
 * ---------------------------------------------------------------- */

int ice_init(nano_ice_t *ice, int is_controlling)
{
    if (!ice) {
        return NANORTC_ERR_INVALID_PARAM;
    }
    memset(ice, 0, sizeof(*ice));
    ice->state = NANORTC_ICE_STATE_NEW;
    ice->is_controlling = is_controlling;
    ice->check_interval_ms = NANORTC_ICE_CHECK_INTERVAL_MS;
    return NANORTC_OK;
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
    if (!msg->username || !ice->local_ufrag_len || !ice->local_pwd_len || msg->username_len == 0) {
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
    size_t remote_len = msg->username_len - local_len - 1;
    return memcmp(msg->username, ice->local_ufrag, local_len) == 0 && remote_len > 0 &&
           (!ice->remote_ufrag_len || (remote_len == ice->remote_ufrag_len &&
                                       memcmp(colon + 1, ice->remote_ufrag, remote_len) == 0));
}

/* Map nanortc_addr_t family to STUN family constant */
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

/* Compact state in the existing pending-slot padding. */
#define ICE_CHECK_QUEUED      0x01u
#define ICE_CHECK_VALID       0x02u
#define ICE_CHECK_NOMINATE    0x04u
#define ICE_CHECK_TRIGGERED   0x08u
#define ICE_CHECK_RETRY_SHIFT 4u
#define ICE_CHECK_STATE_MASK  0x0fu

/* RFC 8445 §6.1.4.1: FIFO triggered checks. A queued slot has no live
 * transaction, so sent_at_ms can hold its bounded queue position. */
static void ice_queue_check(nano_ice_t *ice, nano_ice_pending_t *pending)
{
    uint32_t position = 0;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++)
        if (ice->pending[i].flags & ICE_CHECK_QUEUED)
            position++;
    pending->sent_at_ms = position;
    pending->flags |= ICE_CHECK_QUEUED;
}

static void ice_unqueue_check(nano_ice_t *ice, int slot)
{
    nano_ice_pending_t *pending = &ice->pending[slot];
    if (!(pending->flags & ICE_CHECK_QUEUED))
        return;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++)
        if ((ice->pending[i].flags & ICE_CHECK_QUEUED) &&
            ice->pending[i].sent_at_ms > pending->sent_at_ms)
            ice->pending[i].sent_at_ms--;
    pending->flags &= ~ICE_CHECK_QUEUED;
}

uint8_t ice_local_base_idx(const nano_ice_t *ice, uint8_t idx)
{
    if (idx < ice->local_candidate_count &&
        ice->local_candidates[idx].type == NANORTC_ICE_CAND_SRFLX &&
        ice->srflx_base_idx < ice->local_candidate_count &&
        ice->local_candidates[ice->srflx_base_idx].type == NANORTC_ICE_CAND_HOST)
        return ice->srflx_base_idx;
    return idx;
}

static bool ice_addr_matches(const nano_ice_candidate_t *c, const nanortc_addr_t *addr)
{
    return c->family == addr->family && c->port == addr->port &&
           memcmp(c->addr, addr->addr, NANORTC_ADDR_SIZE) == 0;
}

static void ice_select_pair(nano_ice_t *ice, uint8_t local, uint8_t remote)
{
    const nano_ice_candidate_t *r = &ice->remote_candidates[remote];
    const nano_ice_candidate_t *l = &ice->local_candidates[ice_local_base_idx(ice, local)];
    memcpy(ice->selected_addr, r->addr, NANORTC_ADDR_SIZE);
    ice->selected_port = r->port;
    ice->selected_family = r->family;
    __atomic_store_n(&ice->selected_type, r->type, __ATOMIC_RELAXED);
    memcpy(ice->selected_local_addr, l->addr, NANORTC_ADDR_SIZE);
    ice->selected_local_port = l->port;
    ice->selected_local_family = l->family;
    ice->selected_local_idx = local;
    __atomic_store_n(&ice->selected_local_type, ice->local_candidates[local].type,
                     __ATOMIC_RELAXED);
    ice->nominated = true;
    ice->state = NANORTC_ICE_STATE_CONNECTED;
}

/* RFC 8445 §7.3.1.3–§7.3.1.5: learn and trigger only after authentication.
 * ponytail: bounded candidate/transaction tables; when full, the peer's next
 * retransmission retries admission. Never evict an active transaction. */
static bool ice_trigger_check(nano_ice_t *ice, uint8_t local, const nanortc_addr_t *src,
                              bool nominate)
{
    if (ice->state == NANORTC_ICE_STATE_CONNECTED || ice->state == NANORTC_ICE_STATE_FAILED ||
        ice->state == NANORTC_ICE_STATE_DISCONNECTED)
        return true;
    if (local >= ice->local_candidate_count || ice->local_candidates[local].family != src->family)
        return !nominate;
    uint8_t remote = 0;
    for (; remote < ice->remote_candidate_count; remote++)
        if (ice_addr_matches(&ice->remote_candidates[remote], src))
            break;
    int slot = -1;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        nano_ice_pending_t *p = &ice->pending[i];
        if ((p->in_flight || p->flags) && p->remote_idx == remote &&
            ice_local_base_idx(ice, p->local_idx) == ice_local_base_idx(ice, local)) {
            slot = i;
            break;
        }
        if (!p->in_flight && !(p->flags & ICE_CHECK_QUEUED) && slot < 0)
            slot = i;
    }
    if (slot < 0 || remote >= NANORTC_MAX_ICE_CANDIDATES)
        return false;
    if (remote == ice->remote_candidate_count) {
        nano_ice_candidate_t *c = &ice->remote_candidates[remote];
        memcpy(c->addr, src->addr, NANORTC_ADDR_SIZE);
        c->family = src->family;
        c->port = src->port;
        c->type = NANORTC_ICE_CAND_PRFLX;
        ice->remote_candidate_count++;
    }
    nano_ice_pending_t *p = &ice->pending[slot];
    if ((!p->in_flight && !p->flags) || p->remote_idx != remote ||
        ice_local_base_idx(ice, p->local_idx) != ice_local_base_idx(ice, local)) {
        memset(p, 0, sizeof(*p));
        p->local_idx = local;
        p->remote_idx = remote;
    }
    if (nominate)
        p->flags |= ICE_CHECK_NOMINATE;
    if (p->flags & ICE_CHECK_VALID) {
        if (nominate)
            ice_select_pair(ice, p->local_idx, remote);
        return true;
    }
    /* Cancel an ordinary transaction once; duplicate requests must not keep
     * cancelling the triggered transaction (RFC 8445 §7.3.1.4). */
    if (!(p->flags & ICE_CHECK_TRIGGERED)) {
        p->in_flight = false;
        p->flags = (p->flags & ICE_CHECK_NOMINATE) | ICE_CHECK_TRIGGERED;
        ice_queue_check(ice, p);
    }
    return true;
}

/* ----------------------------------------------------------------
 * ice_find_local_idx_by_family
 *
 * Find the first local candidate matching @p family. Used as a fallback in
 * ice_handle_stun() when the caller cannot identify the receiving interface
 * (dst.family==0) — RFC 8445 §6.1.2.2 mandates same-family pairing, so on a
 * dual-stack host with multiple registered candidates we must avoid blindly
 * latching idx 0 (typically v4 on Linux enumeration order) against an IPv6
 * remote. Returns NANORTC_ICE_LOCAL_IDX_UNKNOWN when no candidate matches.
 * ---------------------------------------------------------------- */

static uint8_t ice_find_local_idx_by_family(const nano_ice_t *ice, uint8_t family)
{
    if (!ice || family == 0) {
        return NANORTC_ICE_LOCAL_IDX_UNKNOWN;
    }
    for (uint8_t i = 0; i < ice->local_candidate_count; i++) {
        if (ice->local_candidates[i].family == family) {
            return i;
        }
    }
    return NANORTC_ICE_LOCAL_IDX_UNKNOWN;
}

/* ----------------------------------------------------------------
 * ice_handle_stun — process incoming STUN message (both roles)
 * ---------------------------------------------------------------- */

int ice_handle_stun(nano_ice_t *ice, const uint8_t *data, size_t len, const nanortc_addr_t *src,
                    uint8_t local_idx, bool via_turn, const nanortc_crypto_provider_t *crypto,
                    uint8_t *resp_buf, size_t resp_buf_len, size_t *resp_len)
{
    if (!ice || !data || !src || !crypto || !resp_buf || !resp_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    *resp_len = 0;
    stun_msg_t msg;

    int rc = stun_parse(data, len, &msg);
    if (rc != NANORTC_OK) {
        return rc;
    }

    if (msg.type == STUN_BINDING_REQUEST) {
        /*
         * Incoming Binding Request — both roles respond.
         *
         * RFC 8445 §7.2.1 + RFC 8489 §10: FINGERPRINT and MESSAGE-INTEGRITY
         * are mandatory on ICE STUN. WebRTC peers always include both
         * (Chrome, Firefox, Safari, libdatachannel verified); anything
         * without them is either a protocol error or an injection attempt,
         * so reject both cases up-front.
         */
        if (!msg.has_fingerprint) {
            return NANORTC_ERR_PROTOCOL;
        }
        rc = stun_verify_fingerprint(data, len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        if (!ice_verify_username(ice, &msg)) {
            return NANORTC_ERR_PROTOCOL;
        }

        if (!msg.has_integrity) {
            return NANORTC_ERR_PROTOCOL;
        }
        rc = stun_verify_integrity(data, len, &msg, (const uint8_t *)ice->local_pwd,
                                   ice->local_pwd_len, crypto->hmac_sha1);
        if (rc != NANORTC_OK) {
            return rc;
        }

        /* Encode Binding Response — sign with our local_pwd (RFC 8445 §7.2.2) */
        uint8_t stun_family = addr_to_stun_family(src->family);
        if (stun_family == 0) {
            return NANORTC_ERR_INVALID_PARAM;
        }

        rc = stun_encode_binding_response(&msg, src->addr, stun_family, src->port,
                                          (const uint8_t *)ice->local_pwd, ice->local_pwd_len,
                                          crypto->hmac_sha1, resp_buf, resp_buf_len, resp_len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        uint8_t resolved = local_idx;
        if (via_turn) {
            resolved = NANORTC_ICE_LOCAL_IDX_UNKNOWN;
            for (uint8_t i = 0; i < ice->local_candidate_count; i++)
                if (ice->local_candidates[i].type == NANORTC_ICE_CAND_RELAY &&
                    ice->local_candidates[i].family == src->family) {
                    resolved = i;
                    break;
                }
        } else if (resolved >= ice->local_candidate_count) {
            resolved = ice_find_local_idx_by_family(ice, src->family);
        }
        if (!ice_trigger_check(ice, resolved, src, msg.use_candidate && !ice->is_controlling)) {
            *resp_len = 0; /* Do not acknowledge a nomination we cannot retain. */
            return NANORTC_ERR_WOULD_BLOCK;
        }

        return NANORTC_OK;

    } else if (msg.type == STUN_BINDING_RESPONSE) {
        /*
         * Incoming Binding Response — could be a connectivity check reply
         * or a consent freshness reply (RFC 7675).
         */

        /* Check if this is a consent check response */
        if (ice->consent_pending &&
            memcmp(msg.transaction_id, ice->consent_txid, STUN_TXID_SIZE) == 0) {
            /* RFC 7675 §5.1 / RFC 8445 §7.1.3: a matching transaction ID is
             * not sufficient. Authenticate the response before allowing it
             * to extend consent. Keep consent_pending set on failure so a
             * later valid retransmitted response can still satisfy it. */
            if (!msg.has_fingerprint || stun_verify_fingerprint(data, len) != NANORTC_OK) {
                return NANORTC_ERR_PROTOCOL;
            }
            if (!msg.has_integrity ||
                stun_verify_integrity(data, len, &msg, (const uint8_t *)ice->remote_pwd,
                                      ice->remote_pwd_len, crypto->hmac_sha1) != NANORTC_OK) {
                return NANORTC_ERR_PROTOCOL;
            }
            ice->consent_pending = false;
            /* consent_expiry_ms is updated by caller with now_ms + CONSENT_TIMEOUT */
            *resp_len = 0;
            return NANORTC_OK;
        }

        /*
         * Connectivity check response — both roles.
         *
         * RFC 8445 §7.1.3: Verify transaction ID matches one of our
         * in-flight requests, then verify MESSAGE-INTEGRITY with remote_pwd.
         */

        /*
         * TD-018: scan the pending table for a slot whose txid matches
         * this response. Responses may arrive out of order, so we cannot
         * assume the latest-sent check is the one being acknowledged.
         */
        int pending_slot = -1;
        for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
            if (ice->pending[i].in_flight &&
                memcmp(msg.transaction_id, ice->pending[i].txid, STUN_TXID_SIZE) == 0) {
                pending_slot = i;
                break;
            }
        }
        if (pending_slot < 0) {
            return NANORTC_ERR_PROTOCOL;
        }

        /* RFC 8445 §7.1.3: a valid Binding Response MUST carry FINGERPRINT
         * and MESSAGE-INTEGRITY. Missing either → protocol error; the
         * matched pending slot stays in_flight so a retransmitted valid
         * response can still land it. */
        if (!msg.has_fingerprint) {
            return NANORTC_ERR_PROTOCOL;
        }
        rc = stun_verify_fingerprint(data, len);
        if (rc != NANORTC_OK) {
            return rc;
        }

        if (!msg.has_integrity) {
            return NANORTC_ERR_PROTOCOL;
        }
        rc = stun_verify_integrity(data, len, &msg, (const uint8_t *)ice->remote_pwd,
                                   ice->remote_pwd_len, crypto->hmac_sha1);
        if (rc != NANORTC_OK) {
            return rc;
        }

        nano_ice_pending_t *p = &ice->pending[pending_slot];
        if (p->local_idx >= ice->local_candidate_count ||
            p->remote_idx >= ice->remote_candidate_count ||
            !ice_addr_matches(&ice->remote_candidates[p->remote_idx], src) ||
            (local_idx < ice->local_candidate_count &&
             ice_local_base_idx(ice, local_idx) != ice_local_base_idx(ice, p->local_idx) &&
             !via_turn))
            return NANORTC_ERR_PROTOCOL; /* RFC 8445 §7.2.5.2.1: symmetric path. */
        p->in_flight = false;
        /* RFC 8445 §7.2.5.3.2: use the gathered reflexive candidate when
         * XOR-MAPPED-ADDRESS identifies it; retain the socket's base. */
        for (uint8_t i = 0; i < ice->local_candidate_count; i++) {
            const nano_ice_candidate_t *c = &ice->local_candidates[i];
            if (c->type == NANORTC_ICE_CAND_SRFLX && c->port == msg.mapped_port &&
                addr_to_stun_family(c->family) == msg.mapped_family &&
                memcmp(c->addr, msg.mapped_addr, NANORTC_ADDR_SIZE) == 0 &&
                ice_local_base_idx(ice, i) == ice_local_base_idx(ice, p->local_idx)) {
                p->local_idx = i;
                break;
            }
        }
        if (ice->state != NANORTC_ICE_STATE_CONNECTED) {
            if (p->flags & ICE_CHECK_NOMINATE) {
                ice_select_pair(ice, p->local_idx, p->remote_idx);
                p->flags = ICE_CHECK_VALID;
            } else {
                p->flags = ICE_CHECK_VALID;
                if (ice->is_controlling) {
                    p->flags |= ICE_CHECK_NOMINATE;
                    ice_queue_check(ice, p);
                }
            }
        }

        /* No response needed for a Binding Response */
        *resp_len = 0;
        return NANORTC_OK;

    } else if (msg.type == STUN_BINDING_ERROR) {
        /* RFC 8489 §9.1.5: unauthenticated UDP errors cannot change a transaction. */
        if (!msg.has_fingerprint || stun_verify_fingerprint(data, len) != NANORTC_OK ||
            !msg.has_integrity ||
            stun_verify_integrity(data, len, &msg, (const uint8_t *)ice->remote_pwd,
                                  ice->remote_pwd_len, crypto->hmac_sha1) != NANORTC_OK)
            return NANORTC_ERR_PROTOCOL;
        for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
            if (ice->pending[i].in_flight &&
                memcmp(msg.transaction_id, ice->pending[i].txid, STUN_TXID_SIZE) == 0) {
                memset(&ice->pending[i], 0, sizeof(ice->pending[i]));
                break;
            }
        }
        *resp_len = 0;
        return NANORTC_ERR_PROTOCOL;
    }

    /* Unknown message type */
    return NANORTC_ERR_PROTOCOL;
}

/* RFC 8445 §14 / RFC 8489 §6.2.1: exponential retransmission, same txid. */
static uint32_t ice_retry_delay(const nano_ice_pending_t *p)
{
    unsigned retries = p->flags >> ICE_CHECK_RETRY_SHIFT;
    uint64_t delay = (uint64_t)NANORTC_ICE_RTO_MS * ((UINT32_C(1) << retries) - 1u);
    if (retries >= 15u || delay > NANORTC_ICE_CHECK_TIMEOUT_MS)
        return NANORTC_ICE_CHECK_TIMEOUT_MS;
    return (uint32_t)delay;
}

static bool ice_pair_ready(const nano_ice_t *ice, uint8_t local, uint8_t remote,
                           const bool *relay_ready)
{
    return local < ice->local_candidate_count && remote < ice->remote_candidate_count &&
           ice->local_candidates[local].family == ice->remote_candidates[remote].family &&
           (!relay_ready || ice->local_candidates[local].type != NANORTC_ICE_CAND_RELAY ||
            relay_ready[remote]);
}

uint32_t ice_get_check(const nano_ice_t *ice, uint32_t now_ms, const bool *relay_ready,
                       nano_ice_check_t *check)
{
    memset(check, 0, sizeof(*check));
    check->slot = -1;
    if (ice->state != NANORTC_ICE_STATE_NEW && ice->state != NANORTC_ICE_STATE_CHECKING)
        return UINT32_MAX;
    if (ice->end_of_candidates && !ice->remote_candidate_count) {
        check->expire = true;
        return 0;
    }
    if (!ice->local_ufrag_len || !ice->local_pwd_len || !ice->remote_ufrag_len ||
        !ice->remote_pwd_len || !ice->local_candidate_count || !ice->remote_candidate_count)
        return UINT32_MAX;
    uint32_t pace = ice->next_check_ms ? nano_time_until(now_ms, ice->next_check_ms) : 0;
    uint32_t best = UINT32_MAX;
    int free_slot = -1;
    bool active = false;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        const nano_ice_pending_t *p = &ice->pending[i];
        if (!p->in_flight && !(p->flags & ICE_CHECK_QUEUED)) {
            if (!p->flags && free_slot < 0)
                free_slot = i;
            continue;
        }
        active = true;
        uint32_t age = nano_time_elapsed(now_ms, p->sent_at_ms);
        bool expire = (p->in_flight && age >= NANORTC_ICE_CHECK_TIMEOUT_MS) ||
                      (!p->in_flight && !(p->flags & ICE_CHECK_VALID) &&
                       ice->check_count >= NANORTC_ICE_MAX_CHECKS);
        if (!expire && !ice_pair_ready(ice, p->local_idx, p->remote_idx, relay_ready))
            continue;
        uint32_t delay = 0;
        if (p->in_flight) {
            uint32_t due = ice_retry_delay(p);
            delay = age < due ? due - age : 0;
        }
        if (!expire && delay < pace)
            delay = pace;
        if (delay < best ||
            (delay == best && !check->expire && (p->flags & ICE_CHECK_QUEUED) &&
             (check->retransmit ||
              (check->slot >= 0 && p->sent_at_ms < ice->pending[check->slot].sent_at_ms)))) {
            *check = (nano_ice_check_t){i, p->local_idx, p->remote_idx, p->in_flight, expire};
            best = delay;
        }
    }
    if (best == 0)
        return best; /* Triggered checks and due retransmissions take precedence. */
    if (ice->check_count >= NANORTC_ICE_MAX_CHECKS) {
        if (!active) {
            check->slot = -1;
            check->expire = true;
            return 0;
        }
        return best;
    }
    /* Keep successful pairs until nomination; idle valid slots can be reused
     * by authenticated incoming checks when the bounded table is full. */
    if (free_slot >= 0) {
        uint32_t total = (uint32_t)ice->local_candidate_count * ice->remote_candidate_count;
        uint32_t start =
            (uint32_t)ice->current_local * ice->remote_candidate_count + ice->current_remote;
        for (uint32_t n = 0; n < total; n++) {
            uint32_t pair = (start + n) % total;
            uint8_t local = (uint8_t)(pair / ice->remote_candidate_count);
            uint8_t remote = (uint8_t)(pair % ice->remote_candidate_count);
            if (!ice_pair_ready(ice, local, remote, relay_ready))
                continue;
            bool known = false;
            for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
                const nano_ice_pending_t *p = &ice->pending[i];
                if ((p->in_flight || p->flags) && p->local_idx == local && p->remote_idx == remote)
                    known = true;
            }
            if (!known && pace < best) {
                *check = (nano_ice_check_t){free_slot, local, remote, false, false};
                return pace;
            }
        }
    }
    return best;
}

int ice_send_check(nano_ice_t *ice, uint32_t now_ms, const nano_ice_check_t *check,
                   const nanortc_crypto_provider_t *crypto, uint8_t *buf, size_t buf_len,
                   size_t *out_len)
{
    *out_len = 0;
    if (check->expire) {
        if (check->slot < 0)
            ice->state = NANORTC_ICE_STATE_FAILED;
        else {
            ice_unqueue_check(ice, check->slot);
            memset(&ice->pending[check->slot], 0, sizeof(ice->pending[check->slot]));
        }
        return NANORTC_OK;
    }
    if (check->slot < 0 || check->slot >= NANORTC_ICE_MAX_PENDING_CHECKS)
        return NANORTC_ERR_INVALID_PARAM;
    nano_ice_pending_t pending = ice->pending[check->slot];
    if (!check->retransmit) {
        if (!crypto->random_bytes || crypto->random_bytes(pending.txid, sizeof(pending.txid)) != 0)
            return NANORTC_ERR_CRYPTO;
        pending.sent_at_ms = now_ms;
        pending.local_idx = check->local_idx;
        pending.remote_idx = check->remote_idx;
        pending.flags &= ICE_CHECK_STATE_MASK;
    }
    char username[NANORTC_ICE_REMOTE_UFRAG_SIZE + NANORTC_ICE_UFRAG_SIZE + 1];
    size_t rlen = ice->remote_ufrag_len, llen = ice->local_ufrag_len;
    if (rlen >= sizeof(ice->remote_ufrag) || llen >= sizeof(ice->local_ufrag))
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    memcpy(username, ice->remote_ufrag, rlen);
    username[rlen] = ':';
    memcpy(username + rlen + 1, ice->local_ufrag, llen);
    int rc = stun_encode_binding_request(
        username, rlen + 1 + llen, ICE_PRFLX_PRIORITY(check->local_idx),
        ice->is_controlling && (pending.flags & ICE_CHECK_NOMINATE), ice->is_controlling,
        ice->tie_breaker, pending.txid, (const uint8_t *)ice->remote_pwd, ice->remote_pwd_len,
        crypto->hmac_sha1, buf, buf_len, out_len);
    if (rc != NANORTC_OK)
        return rc;
    pending.flags = (uint8_t)((pending.flags & ~ICE_CHECK_QUEUED) + (1u << ICE_CHECK_RETRY_SHIFT));
    pending.in_flight = true;
    ice_unqueue_check(ice, check->slot);
    ice->pending[check->slot] = pending;
    if (!check->retransmit && ice->check_count < UINT8_MAX)
        ice->check_count++;
    ice->next_check_ms = nano_time_deadline(now_ms, ice->check_interval_ms);
    ice->state = NANORTC_ICE_STATE_CHECKING;
    ice->current_remote = (uint8_t)(check->remote_idx + 1u);
    ice->current_local = check->local_idx;
    if (ice->current_remote >= ice->remote_candidate_count) {
        ice->current_remote = 0;
        ice->current_local = (uint8_t)((check->local_idx + 1u) % ice->local_candidate_count);
    }
    return NANORTC_OK;
}

int ice_generate_check(nano_ice_t *ice, uint32_t now_ms, const nanortc_crypto_provider_t *crypto,
                       uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!ice || !crypto || !buf || !out_len)
        return NANORTC_ERR_INVALID_PARAM;
    *out_len = 0;
    nano_ice_check_t check;
    if (ice_get_check(ice, now_ms, NULL, &check) != 0)
        return NANORTC_OK;
    return ice_send_check(ice, now_ms, &check, crypto, buf, buf_len, out_len);
}

/* ----------------------------------------------------------------
 * ice_restart — RFC 8445 §9
 * ---------------------------------------------------------------- */

int ice_restart(nano_ice_t *ice)
{
    if (!ice) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Reset state but preserve role, tie_breaker, and local candidates */
    int is_controlling = ice->is_controlling;
    uint64_t tie_breaker = ice->tie_breaker;
    uint8_t generation = ice->generation;
    uint8_t srflx_base_idx = ice->srflx_base_idx;
    /* Save local candidates — they survive restart */
    nano_ice_candidate_t saved_local[NANORTC_MAX_LOCAL_CANDIDATES];
    uint8_t saved_local_count = ice->local_candidate_count;
    memcpy(saved_local, ice->local_candidates, sizeof(saved_local));

    memset(ice, 0, sizeof(*ice));
    ice->state = NANORTC_ICE_STATE_NEW;
    ice->is_controlling = is_controlling;
    ice->tie_breaker = tie_breaker;
    ice->generation = generation + 1;
    ice->srflx_base_idx = srflx_base_idx;
    ice->check_interval_ms = NANORTC_ICE_CHECK_INTERVAL_MS;
    /* Restore local candidates */
    memcpy(ice->local_candidates, saved_local, sizeof(saved_local));
    ice->local_candidate_count = saved_local_count;

    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * ice_generate_consent — RFC 7675
 *
 * Periodic STUN Binding Request on the selected pair to verify
 * that the remote endpoint still consents to receive traffic.
 * ---------------------------------------------------------------- */

int ice_generate_consent(nano_ice_t *ice, uint32_t now_ms, const nanortc_crypto_provider_t *crypto,
                         uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!ice || !crypto || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    *out_len = 0;

    /* Only send consent checks when connected */
    if (ice->state != NANORTC_ICE_STATE_CONNECTED) {
        return NANORTC_OK;
    }

    /* Pacing */
    if (ice->consent_next_ms != 0u && !nano_time_is_due(now_ms, ice->consent_next_ms)) {
        return NANORTC_OK;
    }

    /* Generate transaction state off-struct and commit only after encoding. */
    uint8_t txid[STUN_TXID_SIZE];
    if (!crypto->random_bytes || crypto->random_bytes(txid, sizeof(txid)) != 0) {
        return NANORTC_ERR_CRYPTO;
    }

    /* Build USERNAME = "remote_ufrag:local_ufrag" (RFC 8445 §7.1.1) */
    char username[64];
    size_t rlen = ice->remote_ufrag_len;
    size_t llen = ice->local_ufrag_len;
    if (rlen + 1 + llen >= sizeof(username)) {
        return NANORTC_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(username, ice->remote_ufrag, rlen);
    username[rlen] = ':';
    memcpy(username + rlen + 1, ice->local_ufrag, llen);
    size_t ulen = rlen + 1 + llen;

    /* Consent check = Binding Request without USE-CANDIDATE (RFC 7675 §5.1) */
    int rc = stun_encode_binding_request(
        username, ulen, ICE_PRFLX_PRIORITY(ice->selected_local_idx), false, /* no use_candidate */
        ice->is_controlling, ice->tie_breaker, txid, (const uint8_t *)ice->remote_pwd,
        ice->remote_pwd_len, crypto->hmac_sha1, buf, buf_len, out_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    memcpy(ice->consent_txid, txid, sizeof(txid));
    ice->consent_pending = true;
    ice->consent_next_ms = nano_time_deadline(now_ms, NANORTC_ICE_CONSENT_INTERVAL_MS);

    return NANORTC_OK;
}

/* ----------------------------------------------------------------
 * ice_consent_expired — RFC 7675 §5.1
 * ---------------------------------------------------------------- */

bool ice_consent_expired(const nano_ice_t *ice, uint32_t now_ms)
{
    if (!ice || ice->state != NANORTC_ICE_STATE_CONNECTED) {
        return false;
    }
    /* consent_expiry_ms must be armed when the caller transitions to
     * CONNECTED (see rtc_process_receive). If it is still 0 here, that is a
     * programming error — rather than silently disabling the timeout (which
     * would mask a dead peer indefinitely), treat an unarmed state as
     * expired so the DISCONNECTED signal reaches the caller. */
    if (ice->consent_expiry_ms == 0) {
        return true;
    }
    return nano_time_is_due(now_ms, ice->consent_expiry_ms);
}

uint32_t ice_next_timeout_ms(const nano_ice_t *ice, uint32_t now_ms)
{
    if (!ice) {
        return UINT32_MAX;
    }

    uint32_t best = UINT32_MAX;

    nano_ice_check_t check;
    best = ice_get_check(ice, now_ms, NULL, &check);

    /* Post-CONNECTED: consent-freshness send + expiry.
     * `consent_next_ms` schedules the keepalive cadence; `consent_expiry_ms`
     * is the hard deadline beyond which we tear the connection down. Both
     * matter — whichever is sooner caps the wakeup. */
    if (ice->state == NANORTC_ICE_STATE_CONNECTED) {
        if (ice->consent_next_ms != 0) {
            uint32_t left = nano_time_until(now_ms, ice->consent_next_ms);
            if (left < best) {
                best = left;
            }
        }
        if (ice->consent_expiry_ms != 0) {
            uint32_t left = nano_time_until(now_ms, ice->consent_expiry_ms);
            if (left < best) {
                best = left;
            }
        }
    }

    return best;
}
