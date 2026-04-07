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

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_RUN_LOOP_H_ */
