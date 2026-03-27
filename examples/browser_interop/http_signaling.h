/*
 * nanortc browser_interop — Minimal HTTP signaling client
 *
 * Simple blocking HTTP client for the signaling relay server.
 * Works on Linux (examples) and ESP32 (esp_http_client drop-in).
 * Example code — uses platform headers (not for src/).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef HTTP_SIGNALING_H_
#define HTTP_SIGNALING_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_SIG_BUF_SIZE 4096

typedef struct {
    char host[64];
    uint16_t port;
    int peer_id; /* assigned by server via /join */
} http_sig_t;

/*
 * Join the signaling relay. Sends POST /join, stores peer_id.
 * Returns 0 on success, negative on error.
 */
int http_sig_join(http_sig_t *sig, const char *host, uint16_t port);

/*
 * Send a JSON signaling message to the other peer.
 * type: "offer", "answer", or "candidate"
 * payload: SDP string or candidate string
 * payload_key: JSON key name ("sdp" or "candidate")
 *
 * Builds JSON with proper escaping and POSTs to /send?id=N.
 * Returns 0 on success, negative on error.
 */
int http_sig_send(http_sig_t *sig, const char *type, const char *payload,
                  const char *payload_key);

/*
 * Receive the next signaling message (blocking with timeout).
 * Sends GET /recv?id=N&timeout=T.
 * On success: fills type_out and payload_out, returns 0.
 * On timeout (204): returns -2.
 * On error: returns -1.
 */
int http_sig_recv(http_sig_t *sig, char *type_out, size_t type_len,
                  char *payload_out, size_t payload_len, int timeout_ms);

/*
 * Leave the signaling session. Sends POST /leave?id=N.
 */
void http_sig_leave(http_sig_t *sig);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SIGNALING_H_ */
