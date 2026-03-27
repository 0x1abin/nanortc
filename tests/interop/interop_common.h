/*
 * nanortc interop tests — Shared signaling and utility layer
 *
 * Provides a socketpair-based signaling pipe for SDP/ICE exchange
 * between the nanortc peer and the libdatachannel peer within a
 * single process.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEROP_COMMON_H_
#define INTEROP_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * Signaling pipe — socketpair-based SDP/ICE exchange
 * ---------------------------------------------------------------- */

/* Message types on the signaling pipe */
#define SIG_MSG_SDP_OFFER     0x01
#define SIG_MSG_SDP_ANSWER    0x02
#define SIG_MSG_ICE_CANDIDATE 0x03
#define SIG_MSG_DONE          0x04

typedef struct {
    int fd[2]; /* fd[0] = nanortc side, fd[1] = libdatachannel side */
} interop_sig_pipe_t;

/* Create the socketpair. Returns 0 on success. */
int interop_sig_create(interop_sig_pipe_t *pipe);

/* Close both ends. */
void interop_sig_destroy(interop_sig_pipe_t *pipe);

/*
 * Send a framed message: type(1) + len(2 big-endian) + payload.
 * Returns 0 on success, -1 on error.
 */
int interop_sig_send(int fd, uint8_t type, const void *data, size_t len);

/*
 * Receive a framed message (blocking with timeout).
 * Writes type into *out_type, payload into buf (up to buf_len).
 * Returns payload length on success, -1 on error/timeout.
 */
int interop_sig_recv(int fd, uint8_t *out_type, void *buf, size_t buf_len,
                     int timeout_ms);

/* ----------------------------------------------------------------
 * Timing helpers
 * ---------------------------------------------------------------- */

/* Monotonic milliseconds (same as run_loop's nano_get_millis) */
uint32_t interop_get_millis(void);

/* Sleep for the given number of milliseconds. */
void interop_sleep_ms(int ms);

/* ----------------------------------------------------------------
 * Test configuration
 * ---------------------------------------------------------------- */

/* Overall timeout per test (ms) */
#define INTEROP_TIMEOUT_MS 10000

/* Port range for nanortc UDP binding (auto-selected) */
#define INTEROP_PORT_BASE 19000

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_COMMON_H_ */
