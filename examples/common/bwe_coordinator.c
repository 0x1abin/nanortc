/*
 * bwe_coordinator — see bwe_coordinator.h for the API contract.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bwe_coordinator.h"

#include "nanortc.h" /* NANORTC_BWE_DIR_* / NANORTC_BWE_SRC_* enums */

#include <string.h>

void bwe_coordinator_init(bwe_coordinator_t *c, uint32_t apply_interval_ms,
                          uint8_t dampen_pct, bwe_apply_fn apply, void *ctx)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->apply_interval_ms = apply_interval_ms;
    c->dampen_pct = dampen_pct;
    c->apply = apply;
    c->apply_ctx = ctx;
}

void bwe_coordinator_reset(bwe_coordinator_t *c)
{
    if (!c)
        return;
    c->applied_bps = 0;
    c->last_apply_ms = 0;
}

int bwe_coordinator_try_apply(bwe_coordinator_t *c, uint32_t candidate_bps, int contributors,
                              uint32_t now_ms)
{
    (void)contributors; /* caller-facing metric; not used by the decision */
    if (!c || !c->apply)
        return -1;
    if (candidate_bps == 0)
        return BWE_APPLY_SKIP_NO_CONTRIB;

    if (c->last_apply_ms && (now_ms - c->last_apply_ms) < c->apply_interval_ms) {
        return BWE_APPLY_SKIP_INTERVAL;
    }

    /* Dead-band check: skip updates that barely move the needle. */
    if (c->applied_bps && c->dampen_pct) {
        uint32_t diff = candidate_bps > c->applied_bps ? candidate_bps - c->applied_bps
                                                       : c->applied_bps - candidate_bps;
        if ((uint64_t)diff * 100u / c->applied_bps < c->dampen_pct) {
            return BWE_APPLY_SKIP_DAMPEN;
        }
    }

    int rc = c->apply(candidate_bps, c->apply_ctx);
    if (rc != 0) {
        return -1;
    }
    c->applied_bps = candidate_bps;
    c->last_apply_ms = now_ms ? now_ms : 1; /* avoid 0 sentinel ambiguity */
    return BWE_APPLY_OK;
}

const char *bwe_dir_str(uint8_t dir)
{
    switch (dir) {
    case NANORTC_BWE_DIR_UP:
        return "UP";
    case NANORTC_BWE_DIR_DOWN:
        return "DOWN";
    default:
        return "STABLE";
    }
}

const char *bwe_src_str(uint8_t src)
{
    switch (src) {
    case NANORTC_BWE_SRC_TWCC_LOSS:
        return "TWCC";
    default:
        return "REMB";
    }
}
