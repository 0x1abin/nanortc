/*
 * session.h — Per-viewer WebRTC session management
 *
 * Each browser viewer gets its own session_t with an independent
 * nanortc_t instance and UDP socket.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SESSION_H_
#define SESSION_H_

#include "nanortc.h"
#include "http_signaling.h"

#include <arpa/inet.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SESSIONS  4
#define MAX_LOCAL_IPS 8

/* ----------------------------------------------------------------
 * Local IP enumeration
 * ---------------------------------------------------------------- */

typedef struct { char ip[INET_ADDRSTRLEN]; } local_ip_t;

extern local_ip_t g_local_ips[MAX_LOCAL_IPS];
extern int g_local_ip_count;

/** Enumerate non-loopback IPv4 addresses into g_local_ips[]. */
void enumerate_local_ipv4(void);

/* ----------------------------------------------------------------
 * Session
 * ---------------------------------------------------------------- */

typedef struct {
    int active;
    int viewer_id;
    nanortc_t rtc;
    int udp_fd;
    int video_mid;
    int media_connected;
} session_t;

extern session_t g_sessions[MAX_SESSIONS];

/** Diagnostic counters (reset by caller each stats interval). */
extern uint32_t g_pli_count;

session_t *session_find_free(void);
session_t *session_find_by_viewer(int viewer_id);

/**
 * Create a session for @p viewer_id: init nanortc, accept offer,
 * bind UDP socket, send answer back via signaling.
 * @return 0 on success, -1 on failure.
 */
int session_create(int viewer_id, const char *offer_sdp,
                   const nanortc_config_t *cfg, http_sig_t *sig);

void session_destroy(session_t *s);

/**
 * Drain nanortc outputs: transmit UDP packets, fire events,
 * update timeout. Called from the main event loop.
 */
void session_dispatch_outputs(session_t *s, uint32_t *timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_H_ */
