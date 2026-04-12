/*
 * nanortc examples — Platform event loop interface
 *
 * Bridges the Sans I/O nanortc_t state machine to real UDP sockets.
 * Each platform provides its own implementation (Linux, ESP-IDF, etc.).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_RUN_LOOP_H_
#define NANORTC_RUN_LOOP_H_

#include "nanortc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nano_event_cb)(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata);

typedef struct nano_run_loop {
    nanortc_t *rtc;
    int fds[NANORTC_MAX_LOCAL_CANDIDATES];               /* one UDP socket per local candidate */
    nanortc_addr_t local_addrs[NANORTC_MAX_LOCAL_CANDIDATES]; /* binary addr for fd matching */
    uint8_t fd_count;
    uint16_t port;
    volatile int running;
    nano_event_cb event_cb;
    void *event_userdata;
    uint32_t max_poll_ms; /* max select timeout (0 = default 100ms) */
} nano_run_loop_t;

/* Initialize: bind a single UDP socket on INADDR_ANY, associate with nanortc_t.
 * Does NOT register ICE candidates — call nanortc_add_local_candidate() after.
 * With NANORTC_FEATURE_IPV6: uses AF_INET6 dual-stack socket. */
int nano_run_loop_init(nano_run_loop_t *loop, nanortc_t *rtc, uint16_t port);

/* Auto-detect local interfaces and bind one socket per non-loopback IPv4 address.
 * Calls nanortc_add_local_candidate() for each. Linux/macOS only (uses getifaddrs). */
int nano_run_loop_auto_candidates(nano_run_loop_t *loop, nanortc_t *rtc, uint16_t port);

/* Set application event callback */
void nano_run_loop_set_event_cb(nano_run_loop_t *loop,
                                nano_event_cb cb, void *userdata);

/* Run one iteration: poll_output → sendto, select → handle_receive/timeout */
int nano_run_loop_step(nano_run_loop_t *loop);

/* Block until disconnected or error */
int nano_run_loop_run(nano_run_loop_t *loop);

/* Stop the loop (can be called from signal handler) */
void nano_run_loop_stop(nano_run_loop_t *loop);

/* Cleanup */
void nano_run_loop_destroy(nano_run_loop_t *loop);

/* Platform helper: monotonic milliseconds */
uint32_t nano_get_millis(void);

/* ----------------------------------------------------------------
 * Single-session offer handler (high-level convenience)
 *
 * Encapsulates the "tear down -> init -> add tracks -> bind UDP ->
 * add candidate -> set event cb -> accept offer -> start loop" chain
 * that every single-peer example (ESP32 web UI, etc.) performs
 * whenever a new browser offer arrives.
 *
 * The caller provides:
 *   - an already-built nanortc_config_t (crypto/role/ICE servers/log)
 *   - a track_setup callback that calls nanortc_add_audio_track /
 *     nanortc_add_video_track as needed (returning the first negative
 *     rc aborts the session; 0 = success)
 *   - UDP bind parameters and the event callback
 *
 * On success, loop->running is set to 1 and the generated SDP answer
 * is written to `answer`. On failure, both rtc and loop are left in
 * a destroyed state ready for the next attempt.
 * ---------------------------------------------------------------- */

typedef int (*nano_track_setup_fn)(nanortc_t *rtc, void *userdata);

typedef struct {
    const nanortc_config_t *rtc_cfg;     /* required */
    nano_track_setup_fn     track_setup; /* optional (NULL = no tracks) */
    void                   *track_userdata;
    const char             *local_ip;    /* required: candidate IP */
    uint16_t                udp_port;    /* required: bind port */
    uint32_t                max_poll_ms; /* 0 = default (100ms) */
    nano_event_cb           event_cb;    /* optional */
    void                   *event_userdata;
} nano_accept_offer_params_t;

/* Returns NANORTC_OK on success, negative on error. */
int nano_session_accept_offer(nanortc_t *rtc,
                              nano_run_loop_t *loop,
                              const nano_accept_offer_params_t *params,
                              const char *offer,
                              char *answer, size_t answer_size,
                              size_t *answer_len);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_RUN_LOOP_H_ */
