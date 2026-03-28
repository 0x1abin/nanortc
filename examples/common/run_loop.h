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
    int fd;               /* UDP socket */
    uint16_t port;
    int running;
    nano_event_cb event_cb;
    void *event_userdata;
} nano_run_loop_t;

/* Initialize: bind UDP socket, associate with nanortc_t */
int nano_run_loop_init(nano_run_loop_t *loop, nanortc_t *rtc,
                       const char *bind_ip, uint16_t port);

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
