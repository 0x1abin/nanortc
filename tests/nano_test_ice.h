/* Test peer: answer the next outgoing ICE check on its actual transport path. */
#ifndef NANO_TEST_ICE_H_
#define NANO_TEST_ICE_H_
#include "nano_ice.h"
#include "nanortc.h"
#include "nanortc_crypto.h"
#include <string.h>

static int test_ice_answer_check(nano_ice_t *ice, uint32_t now_ms,
                                 const nanortc_crypto_provider_t *crypto)
{
    nano_ice_check_t check;
    if (ice_get_check(ice, now_ms, NULL, &check) != 0 || check.expire)
        return NANORTC_ERR_STATE;
    uint8_t req[256], resp[256], unused[256];
    size_t req_len = 0, resp_len = 0, unused_len = 0;
    int rc = ice_generate_check(ice, now_ms, crypto, req, sizeof(req), &req_len);
    if (rc != NANORTC_OK || !req_len)
        return NANORTC_ERR_STATE;
    stun_msg_t msg;
    rc = stun_parse(req, req_len, &msg);
    if (rc != NANORTC_OK)
        return rc;
    const nano_ice_candidate_t *local = &ice->local_candidates[check.local_idx];
    rc = stun_encode_binding_response(&msg, local->addr, local->family == 4 ? 1 : 2, local->port,
                                      (const uint8_t *)ice->remote_pwd, ice->remote_pwd_len,
                                      crypto->hmac_sha1, resp, sizeof(resp), &resp_len);
    if (rc != NANORTC_OK)
        return rc;
    const nano_ice_candidate_t *remote = &ice->remote_candidates[check.remote_idx];
    nanortc_addr_t src = {.family = remote->family, .port = remote->port};
    memcpy(src.addr, remote->addr, sizeof(src.addr));
    return ice_handle_stun(ice, resp, resp_len, &src, check.local_idx,
                           local->type == NANORTC_ICE_CAND_RELAY, crypto, unused, sizeof(unused),
                           &unused_len);
}
#endif
