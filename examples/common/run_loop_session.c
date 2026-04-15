/*
 * nanortc examples — Single-session offer/answer convenience helper
 *
 * Platform-neutral: uses only run_loop + nanortc public APIs.
 * Compiled on both Linux and ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */

#include "run_loop.h"

int nano_session_accept_offer(nanortc_t *rtc, nano_run_loop_t *loop,
                              const nano_accept_offer_params_t *params, const char *offer,
                              char *answer, size_t answer_size, size_t *answer_len)
{
    if (!rtc || !loop || !params || !params->rtc_cfg || !params->local_ip || !offer || !answer) {
        return NANORTC_ERR_INVALID_PARAM;
    }

    /* Tear down any prior session (idempotent: safe to call on zero-initialized) */
    nano_run_loop_destroy(loop);
    nanortc_destroy(rtc);

    int rc = nanortc_init(rtc, params->rtc_cfg);
    if (rc != NANORTC_OK) {
        return rc;
    }

    if (params->track_setup) {
        rc = params->track_setup(rtc, params->track_userdata);
        if (rc < 0) {
            nanortc_destroy(rtc);
            return rc;
        }
    }

    rc = nano_run_loop_init(loop, rtc, params->udp_port);
    if (rc < 0) {
        nanortc_destroy(rtc);
        return rc;
    }

    nanortc_add_local_candidate(rtc, params->local_ip, params->udp_port);
    nano_run_loop_set_event_cb(loop, params->event_cb, params->event_userdata);
    loop->max_poll_ms = params->max_poll_ms;

    rc = nanortc_accept_offer(rtc, offer, answer, answer_size, answer_len);
    if (rc != NANORTC_OK) {
        nano_run_loop_destroy(loop);
        nanortc_destroy(rtc);
        return rc;
    }

    /* nanortc fully initialized — safe to start pumping the loop. */
    loop->running = 1;
    return NANORTC_OK;
}
