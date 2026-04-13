/*
 * nanortc examples — Multi-viewer WebRTC session pool
 *
 * Shared session management for Linux "host-mode" examples (macOS
 * camera, RK3588 camera, ...) that accept multiple simultaneous
 * browser viewers over the signaling relay.
 *
 * Each viewer gets its own `nano_session_t` holding an independent
 * `nanortc_t` + UDP socket. The pool enforces:
 *   - one session per viewer_id (reconnection kills the old one)
 *   - per-session wildcard bind (OS-assigned port) with SO_SNDBUF
 *     sized for bursty IDR frame transmission
 *   - built-in media_connected / active bookkeeping on CONNECTED
 *     and DISCONNECTED events
 *
 * The application injects behaviour through three hooks:
 *   - `track_setup`  : add audio/video tracks after nanortc_init
 *   - `on_event`     : react to nanortc events (keyframe request, ...)
 *   - `local_ips`    : list of IPv4 addresses advertised as candidates
 *
 * The outer select()-loop still lives in the application — this
 * module only provides the per-session plumbing.
 *
 * Example code (not for src/): uses POSIX socket headers.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_MULTI_SESSION_H_
#define NANORTC_MULTI_SESSION_H_

#include "nanortc.h"
#include "http_signaling.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ip[INET_ADDRSTRLEN];
} nano_local_ip_t;

/**
 * Enumerate non-loopback, up IPv4 addresses into @p ips.
 * If none are found (no UP interface or getifaddrs fails), falls back
 * to writing a single "127.0.0.1" entry.
 *
 * @return number of entries written (always >= 1 on success,
 *         capped at @p max_ips).
 */
int nano_enumerate_local_ipv4(nano_local_ip_t *ips, int max_ips);

struct nano_session;
struct nano_session_pool;

/**
 * Called once per session after `nanortc_init`, before binding the
 * UDP socket. Implementations typically call `nanortc_add_audio_track`
 * / `nanortc_add_video_track` and record the returned mids into the
 * session struct (e.g. `s->audio_mid`, `s->video_mid`).
 *
 * Return 0 on success, negative on error (aborts session creation).
 */
typedef int (*nano_session_track_setup_fn)(struct nano_session *s, void *userdata);

/**
 * Invoked for every nanortc event after the pool has updated its own
 * bookkeeping (`media_connected`, `active`). Typical uses:
 *   - force keyframe on `NANORTC_EV_KEYFRAME_REQUEST`
 *   - log ICE state changes
 */
typedef void (*nano_session_event_cb)(struct nano_session *s, const nanortc_event_t *evt,
                                      void *userdata);

typedef struct nano_session {
    int active;    /**< 1 = in use, 0 = free / marked for cleanup */
    int viewer_id; /**< signaling peer id of the browser */
    nanortc_t rtc;
    int udp_fd;
    int video_mid;                  /**< -1 if no video track negotiated */
    int audio_mid;                  /**< -1 if no audio track negotiated */
    int media_connected;            /**< 1 after NANORTC_EV_CONNECTED */
    uint32_t last_kf_ms;            /**< Per-session PLI rate limiter (app-owned) */
    struct nano_session_pool *pool; /**< back-pointer, set on create */
} nano_session_t;

typedef struct nano_session_pool {
    nano_session_t *sessions;
    int max_sessions;

    /* Local IPv4 candidates to advertise on every session.
     * Caller-owned, must outlive the pool. */
    const nano_local_ip_t *local_ips;
    int local_ip_count;

    /* Application hooks */
    nano_session_track_setup_fn track_setup;
    void *track_userdata;
    nano_session_event_cb on_event;
    void *event_userdata;
} nano_session_pool_t;

/** Initialise the pool. @p storage must have room for @p max sessions. */
void nano_session_pool_init(nano_session_pool_t *pool, nano_session_t *storage, int max);

/** Find a free session slot, or NULL if the pool is full. */
nano_session_t *nano_session_find_free(nano_session_pool_t *pool);

/** Find the session serving @p viewer_id, or NULL. */
nano_session_t *nano_session_find_by_viewer(nano_session_pool_t *pool, int viewer_id);

/**
 * Create a session for @p viewer_id: init nanortc, call track_setup,
 * bind UDP socket (ephemeral port), register every pool->local_ips
 * entry as a local candidate, accept the offer, and send the answer
 * back via @p sig using `http_sig_send_to`.
 *
 * If a session already exists for @p viewer_id it is destroyed first
 * (reconnection).
 *
 * @return 0 on success, -1 on failure.
 */
int nano_session_create(nano_session_pool_t *pool, int viewer_id, const char *offer_sdp,
                        const nanortc_config_t *cfg, http_sig_t *sig);

/** Destroy a single session (close fd, free nanortc, mark inactive). */
void nano_session_destroy(nano_session_t *s);

/**
 * Drain the nanortc output queue of one session:
 *   - TRANSMIT → sendto on `s->udp_fd`
 *   - EVENT    → update media_connected / active, call pool->on_event
 *   - TIMEOUT  → clamp `*timeout_ms`
 */
void nano_session_dispatch(nano_session_t *s, uint32_t *timeout_ms);

/** Register every active session's udp_fd into @p rset / @p maxfd. */
void nano_session_pool_add_fds(nano_session_pool_t *pool, fd_set *rset, int *maxfd);

/**
 * Pump UDP input: for each active session either recvfrom and feed
 * `nanortc_handle_input`, or call `nanortc_handle_input(..., NULL, 0, NULL)`
 * so the internal timers advance even without packets.
 */
void nano_session_pool_handle_udp(nano_session_pool_t *pool, int select_ret, const fd_set *rset,
                                  uint32_t now_ms);

/** Iterate every active session and run `nano_session_dispatch` on it. */
void nano_session_pool_dispatch_all(nano_session_pool_t *pool, uint32_t *timeout_ms);

/** Destroy any session whose `active` was cleared but still holds an fd. */
void nano_session_pool_cleanup(nano_session_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_MULTI_SESSION_H_ */
