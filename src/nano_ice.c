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

/*
 * Compute the priority of a local candidate (RFC 8445 §5.1.2.1).
 *
 * Used by both outgoing connectivity checks (ice_generate_check) and
 * consent freshness (ice_generate_consent), so the PRIORITY attribute
 * sent on a srflx-paired check correctly reflects type_pref=100, not
 * the host preference. Falls back to HOST priority for unknown idx so
 * legacy callers that pre-fill local_candidates as HOST keep working.
 */
static uint32_t ice_compute_local_priority(const nano_ice_t *ice, uint8_t idx)
{
    if (idx >= ice->local_candidate_count) {
        /* Unknown / out-of-range idx (incl. 0xFF sentinel): collapse to host
         * slot 0 so the priority stays in a sane range even if a caller
         * forgets to normalise the sentinel before reaching this path. */
        return ICE_HOST_PRIORITY(0);
    }
    switch (ice->local_candidates[idx].type) {
#if NANORTC_FEATURE_ICE_SRFLX
    case NANORTC_ICE_CAND_SRFLX:
        return ICE_SRFLX_PRIORITY(idx);
#endif
    case NANORTC_ICE_CAND_RELAY:
        return ICE_RELAY_PRIORITY(idx);
    case NANORTC_ICE_CAND_HOST:
    default:
        return ICE_HOST_PRIORITY(idx);
    }
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

/*
 * RFC 8445 §6.1.2.2: candidate pairs MUST only be formed between candidates of
 * the same address family. Advance (current_local, current_remote) to the next
 * same-family position, iterating at most N*M slots so it always terminates.
 * Returns false when no same-family pair exists yet (e.g. local v4 only, remote
 * v6 only — wait for trickle or fail via the existing MAX_CHECKS path).
 */
static bool ice_advance_to_same_family_pair(nano_ice_t *ice)
{
    if (ice->local_candidate_count == 0 || ice->remote_candidate_count == 0) {
        return false;
    }
    uint32_t slots = (uint32_t)ice->local_candidate_count * ice->remote_candidate_count;
    for (uint32_t i = 0; i < slots; i++) {
        if (ice->current_local < ice->local_candidate_count &&
            ice->current_remote < ice->remote_candidate_count &&
            ice->local_candidates[ice->current_local].family ==
                ice->remote_candidates[ice->current_remote].family) {
            return true;
        }
        ice->current_remote++;
        if (ice->current_remote >= ice->remote_candidate_count) {
            ice->current_remote = 0;
            ice->current_local++;
            if (ice->current_local >= ice->local_candidate_count) {
                ice->current_local = 0;
            }
        }
    }
    return false;
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

        /*
         * RFC 8445 §7.2.1.4: If USE-CANDIDATE is present and we are
         * controlled, nominate this pair and transition to CONNECTED.
         */
        if (msg.use_candidate && !ice->is_controlling) {
            memcpy(ice->selected_addr, src->addr, NANORTC_ADDR_SIZE);
            ice->selected_port = src->port;
            ice->selected_family = src->family;
            /* via_turn=true means this Binding Request was unwrapped from a
             * TURN Data Indication / ChannelData (RFC 5766 §10/§11) — i.e. the
             * remote reached us through our relay. Mark the pair as RELAY so
             * rtc_enqueue_transmit() routes the response back through the
             * relay; otherwise a direct sendto() lands on a NAT'd peer
             * address with no return route and the browser drops the reply
             * because the source IP doesn't match the (local→relay) pair it
             * sent the check on. */
            __atomic_store_n(&ice->selected_type,
                             (uint8_t)(via_turn ? NANORTC_ICE_CAND_RELAY : NANORTC_ICE_CAND_HOST),
                             __ATOMIC_RELAXED);
            /*
             * Resolve the local candidate that received this Binding Request.
             * The caller (rtc_process_receive) maps the receive socket's bound
             * address to a local_candidates[] index; pass-through preserves
             * srflx/relay typing so consent freshness uses the right PRIORITY
             * (RFC 7675 §5.1). When the caller cannot determine the index
             * (NANORTC_ICE_LOCAL_IDX_UNKNOWN), fall back to 0 — the historical
             * behavior, correct on single-candidate single-interface setups.
             */
            uint8_t resolved_idx = (local_idx == NANORTC_ICE_LOCAL_IDX_UNKNOWN ||
                                    local_idx >= ice->local_candidate_count)
                                       ? 0
                                       : local_idx;
            ice->selected_local_idx = resolved_idx;
            if (resolved_idx < ice->local_candidate_count) {
                memcpy(ice->selected_local_addr, ice->local_candidates[resolved_idx].addr,
                       NANORTC_ADDR_SIZE);
                ice->selected_local_port = ice->local_candidates[resolved_idx].port;
                ice->selected_local_family = ice->local_candidates[resolved_idx].family;
                /* Atomic write — read concurrently from test threads via
                 * __atomic_load_n. Mirrors the treatment of selected_type
                 * (see Binding Response path / PR #41 fix). */
                __atomic_store_n(&ice->selected_local_type,
                                 (uint8_t)ice->local_candidates[resolved_idx].type,
                                 __ATOMIC_RELAXED);
            }
            ice->nominated = true;
            ice->state = NANORTC_ICE_STATE_CONNECTED;
            /* Arm consent freshness (RFC 7675) — caller sets now_ms-based times */
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
            /* Consent refreshed — reset expiry */
            ice->consent_pending = false;
            /* consent_expiry_ms is updated by caller with now_ms + CONSENT_TIMEOUT */
            *resp_len = 0;
            return NANORTC_OK;
        }

        /*
         * Connectivity check response — controlling role only.
         *
         * RFC 8445 §7.1.3: Verify transaction ID matches one of our
         * in-flight requests, then verify MESSAGE-INTEGRITY with remote_pwd.
         */
        if (!ice->is_controlling) {
            return NANORTC_ERR_PROTOCOL;
        }

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

        /*
         * ICE connectivity established — record the selected pair using
         * the indices captured when this specific check was sent.
         *
         * Only clear the slot after MI verification succeeds; on MI failure
         * we return early (above) without touching in_flight so a legitimate
         * response arriving microseconds later can still be matched.
         */
        uint8_t sel_remote_idx = ice->pending[pending_slot].remote_idx;
        uint8_t sel_local_idx = ice->pending[pending_slot].local_idx;
        if (sel_remote_idx < ice->remote_candidate_count) {
            memcpy(ice->selected_addr, ice->remote_candidates[sel_remote_idx].addr,
                   NANORTC_ADDR_SIZE);
            ice->selected_port = ice->remote_candidates[sel_remote_idx].port;
            ice->selected_family = ice->remote_candidates[sel_remote_idx].family;
            __atomic_store_n(&ice->selected_type,
                             (uint8_t)ice->remote_candidates[sel_remote_idx].type,
                             __ATOMIC_RELAXED);
        }
        if (sel_local_idx < ice->local_candidate_count) {
            memcpy(ice->selected_local_addr, ice->local_candidates[sel_local_idx].addr,
                   NANORTC_ADDR_SIZE);
            ice->selected_local_port = ice->local_candidates[sel_local_idx].port;
            ice->selected_local_family = ice->local_candidates[sel_local_idx].family;
            /* Atomic write — see USE-CANDIDATE branch above. */
            __atomic_store_n(&ice->selected_local_type,
                             (uint8_t)ice->local_candidates[sel_local_idx].type, __ATOMIC_RELAXED);
        }
        ice->selected_local_idx = sel_local_idx;
        ice->pending[pending_slot].in_flight = false; /* free slot */
        ice->nominated = true;
        ice->state = NANORTC_ICE_STATE_CONNECTED;

        /* No response needed for a Binding Response */
        *resp_len = 0;
        return NANORTC_OK;

    } else if (msg.type == STUN_BINDING_ERROR) {
        /*
         * Binding Error Response (RFC 8489 §6.3.4 / RFC 8445 §7.3.1.1).
         *
         * We match it to a pending slot and free it so the transaction does
         * not block the table; the error itself is surfaced as a protocol
         * error so the caller can log it (TURN error codes 401/438 are
         * handled separately in nano_turn.c; plain Binding Errors on an ICE
         * pair usually mean 400 Bad Request from a malformed check or 487
         * Role Conflict — the latter would need tie-breaker comparison and
         * role swap per §7.3.1.1, deferred).
         */
        if (!ice->is_controlling) {
            return NANORTC_ERR_PROTOCOL;
        }
        for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
            if (ice->pending[i].in_flight &&
                memcmp(msg.transaction_id, ice->pending[i].txid, STUN_TXID_SIZE) == 0) {
                ice->pending[i].in_flight = false;
                break;
            }
        }
        *resp_len = 0;
        return NANORTC_ERR_PROTOCOL;
    }

    /* Unknown message type */
    return NANORTC_ERR_PROTOCOL;
}

/* ----------------------------------------------------------------
 * ice_generate_check — controlling role STUN Binding Request
 * ---------------------------------------------------------------- */

int ice_generate_check(nano_ice_t *ice, uint32_t now_ms, const nanortc_crypto_provider_t *crypto,
                       uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!ice || !crypto || !buf || !out_len) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    *out_len = 0;

    /* Only controlling role generates checks */
    if (!ice->is_controlling) {
        return NANORTC_OK;
    }

    /* Don't generate checks once connected, failed, or disconnected.
     * DISCONNECTED is reached when consent freshness expires (RFC 7675);
     * recovery requires ice_restart(), not more checks on dead credentials. */
    if (ice->state == NANORTC_ICE_STATE_CONNECTED || ice->state == NANORTC_ICE_STATE_FAILED ||
        ice->state == NANORTC_ICE_STATE_DISCONNECTED) {
        return NANORTC_OK;
    }

    /* Pacing: don't send before next_check_ms */
    if (ice->state == NANORTC_ICE_STATE_CHECKING && now_ms < ice->next_check_ms) {
        return NANORTC_OK;
    }

    /* No candidates yet — wait for trickle / application */
    if (ice->remote_candidate_count == 0 || ice->local_candidate_count == 0) {
        if (ice->end_of_candidates && ice->remote_candidate_count == 0) {
            ice->state = NANORTC_ICE_STATE_FAILED;
        }
        return NANORTC_OK;
    }

    /* Check count limit */
    if (ice->check_count >= NANORTC_ICE_MAX_CHECKS) {
        ice->state = NANORTC_ICE_STATE_FAILED;
        return NANORTC_OK;
    }

    /* RFC 8445 §6.1.2.2: skip cross-family pairs before burning a check slot. */
    if (!ice_advance_to_same_family_pair(ice)) {
        return NANORTC_OK;
    }

    /*
     * TD-018: allocate a pending slot for this check. Prefer a free slot,
     * then a stale entry past NANORTC_ICE_CHECK_TIMEOUT_MS, else reap the
     * oldest in-flight entry so we always make forward progress. RFC 8445
     * §6.1.4.2 allows retransmitting a check for a pair that already has
     * an in-flight request, so reap-oldest is RFC-conformant.
     */
    int slot = -1;
    int oldest = -1;
    uint32_t oldest_age = 0;
    for (int i = 0; i < NANORTC_ICE_MAX_PENDING_CHECKS; i++) {
        if (!ice->pending[i].in_flight) {
            slot = i;
            break;
        }
        uint32_t age = now_ms - ice->pending[i].sent_at_ms; /* wrap-safe */
        if (age >= NANORTC_ICE_CHECK_TIMEOUT_MS) {
            slot = i; /* reap stale */
            break;
        }
        if (oldest < 0 || age > oldest_age) {
            oldest = i;
            oldest_age = age;
        }
    }
    if (slot < 0) {
        slot = oldest; /* table full of fresh entries — reap oldest */
    }

    /* Generate random transaction ID into the chosen slot */
    if (crypto->random_bytes(ice->pending[slot].txid, STUN_TXID_SIZE) != 0) {
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

    /* Encode Binding Request — sign with remote_pwd (RFC 8445 §7.1.1) */
    int rc = stun_encode_binding_request(
        username, ulen, ice_compute_local_priority(ice, ice->current_local),
        true, /* use_candidate */
        true, /* is_controlling */
        ice->tie_breaker, ice->pending[slot].txid, (const uint8_t *)ice->remote_pwd,
        ice->remote_pwd_len, crypto->hmac_sha1, buf, buf_len, out_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    /* Commit the pending slot only after encoding succeeded */
    ice->pending[slot].sent_at_ms = now_ms;
    ice->pending[slot].local_idx = ice->current_local;
    ice->pending[slot].remote_idx = ice->current_remote;
    ice->pending[slot].in_flight = true;

    ice->check_count++;
    ice->next_check_ms = now_ms + ice->check_interval_ms;

    if (ice->state == NANORTC_ICE_STATE_NEW) {
        ice->state = NANORTC_ICE_STATE_CHECKING;
    }

    /* Advance pair: remote inner loop, local outer loop */
    ice->current_remote++;
    if (ice->current_remote >= ice->remote_candidate_count) {
        ice->current_remote = 0;
        ice->current_local++;
        if (ice->current_local >= ice->local_candidate_count) {
            ice->current_local = 0;
        }
    }

    return NANORTC_OK;
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
    /* Save local candidates — they survive restart */
    nano_ice_candidate_t saved_local[NANORTC_MAX_LOCAL_CANDIDATES];
    uint8_t saved_local_count = ice->local_candidate_count;
    memcpy(saved_local, ice->local_candidates, sizeof(saved_local));

    memset(ice, 0, sizeof(*ice));
    ice->state = NANORTC_ICE_STATE_NEW;
    ice->is_controlling = is_controlling;
    ice->tie_breaker = tie_breaker;
    ice->generation = generation + 1;
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
    if (now_ms < ice->consent_next_ms) {
        return NANORTC_OK;
    }

    /* Generate random transaction ID for consent check */
    if (crypto->random_bytes(ice->consent_txid, STUN_TXID_SIZE) != 0) {
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
        username, ulen, ice_compute_local_priority(ice, ice->selected_local_idx),
        false, /* no use_candidate */
        ice->is_controlling, ice->tie_breaker, ice->consent_txid, (const uint8_t *)ice->remote_pwd,
        ice->remote_pwd_len, crypto->hmac_sha1, buf, buf_len, out_len);
    if (rc != NANORTC_OK) {
        return rc;
    }

    ice->consent_pending = true;
    ice->consent_next_ms = now_ms + NANORTC_ICE_CONSENT_INTERVAL_MS;

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
    return now_ms >= ice->consent_expiry_ms;
}
