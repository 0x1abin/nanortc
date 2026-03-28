/*
 * nanortc examples — Signaling interface (SDP exchange)
 *
 * nanortc doesn't handle signaling. The application must exchange
 * SDP offer/answer through some channel (MQTT, HTTP/WHIP, WebSocket, etc.).
 *
 * This interface abstracts the signaling mechanism so examples can
 * swap implementations without changing application logic.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_SIGNALING_H_
#define NANORTC_SIGNALING_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nano_signaling nano_signaling_t;

typedef enum {
    NANORTC_SIG_STDIN, /* Read offer from stdin, print answer to stdout */
} nano_signaling_type_t;

struct nano_signaling {
    nano_signaling_type_t type;
};

/* Initialize signaling channel */
int nano_signaling_init(nano_signaling_t *sig, nano_signaling_type_t type);

/* Wait for remote SDP offer. Blocks until complete SDP is received.
 * Returns number of bytes written to buf, or negative on error. */
int nano_signaling_recv_offer(nano_signaling_t *sig, char *buf, size_t buf_len);

/* Send local SDP answer to remote peer. */
int nano_signaling_send_answer(nano_signaling_t *sig, const char *answer);

void nano_signaling_destroy(nano_signaling_t *sig);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_SIGNALING_H_ */
