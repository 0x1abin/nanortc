/*
 * nanortc interop tests — NanoRTC peer implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_nanortc_peer.h"
#include "interop_common.h"
#include "nanortc_crypto.h"
#include "ice_server_resolve.h"

#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>

/* ----------------------------------------------------------------
 * Event callback (runs in nanortc thread)
 * ---------------------------------------------------------------- */

static void nanortc_on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    interop_nanortc_peer_t *peer = (interop_nanortc_peer_t *)userdata;
    (void)rtc;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        fprintf(stderr, "[nanortc] ICE state change (%d)\n", evt->ice_state);
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            atomic_store(&peer->ice_connected, 1);
        }
        break;

    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[nanortc] Fully connected\n");
        atomic_store(&peer->dtls_connected, 1);
        atomic_store(&peer->sctp_connected, 1);
        break;

    case NANORTC_EV_DATACHANNEL_OPEN:
        fprintf(stderr, "[nanortc] DataChannel open (stream=%d)\n", evt->datachannel_open.id);
        atomic_store(&peer->dc_open, 1);
        break;

    case NANORTC_EV_DATACHANNEL_DATA:
        if (evt->datachannel_data.binary) {
            fprintf(stderr, "[nanortc] DC binary data (%zu bytes)\n", evt->datachannel_data.len);
            pthread_mutex_lock(&peer->msg_mutex);
            if (evt->datachannel_data.len <= sizeof(peer->last_msg)) {
                memcpy(peer->last_msg, evt->datachannel_data.data, evt->datachannel_data.len);
                peer->last_msg_len = evt->datachannel_data.len;
                peer->last_msg_is_string = 0;
            }
            pthread_mutex_unlock(&peer->msg_mutex);
        } else {
            fprintf(stderr, "[nanortc] DC string: %.*s\n", (int)evt->datachannel_data.len,
                    (const char *)evt->datachannel_data.data);
            pthread_mutex_lock(&peer->msg_mutex);
            if (evt->datachannel_data.len < sizeof(peer->last_msg)) {
                memcpy(peer->last_msg, evt->datachannel_data.data, evt->datachannel_data.len);
                peer->last_msg[evt->datachannel_data.len] = '\0';
                peer->last_msg_len = evt->datachannel_data.len;
                peer->last_msg_is_string = 1;
            }
            pthread_mutex_unlock(&peer->msg_mutex);
        }
        atomic_fetch_add(&peer->msg_count, 1);
        break;

    case NANORTC_EV_DATACHANNEL_CLOSE:
        fprintf(stderr, "[nanortc] DataChannel closed\n");
        break;

    case NANORTC_EV_DISCONNECTED:
        fprintf(stderr, "[nanortc] Disconnected\n");
        nano_run_loop_stop(&peer->loop);
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------
 * Signaling: handle SDP offer from libdatachannel
 * ---------------------------------------------------------------- */

static int nanortc_do_signaling(interop_nanortc_peer_t *peer)
{
    char buf[8192];
    uint8_t msg_type;

    /* Read SDP offer */
    int len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1, INTEROP_TIMEOUT_MS);
    if (len < 0 || msg_type != SIG_MSG_SDP_OFFER) {
        fprintf(stderr, "[nanortc] Failed to receive SDP offer\n");
        return -1;
    }
    buf[len] = '\0';
    fprintf(stderr, "[nanortc] Got SDP offer (%d bytes)\n", len);

    /* Generate answer */
    char answer[8192];
    size_t answer_len = 0;
    int rc = nanortc_accept_offer(&peer->rtc, buf, answer, sizeof(answer), &answer_len);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "[nanortc] nanortc_accept_offer failed: %d\n", rc);
        return -1;
    }
    rc = interop_sig_send(peer->sig_fd, SIG_MSG_SDP_ANSWER, answer, answer_len);
    if (rc != 0) {
        fprintf(stderr, "[nanortc] Failed to send SDP answer\n");
        return -1;
    }
    fprintf(stderr, "[nanortc] Sent SDP answer (%zu bytes)\n", answer_len);

    /* Exchange ICE candidates until DONE */
    for (;;) {
        len = interop_sig_recv(peer->sig_fd, &msg_type, buf, sizeof(buf) - 1, INTEROP_TIMEOUT_MS);
        if (len < 0) {
            fprintf(stderr, "[nanortc] Signaling recv error\n");
            return -1;
        }
        buf[len] = '\0';

        if (msg_type == SIG_MSG_DONE) {
            fprintf(stderr, "[nanortc] Signaling complete\n");
            break;
        }

        if (msg_type == SIG_MSG_ICE_CANDIDATE) {
            fprintf(stderr, "[nanortc] Got remote ICE candidate: %s\n", buf);
            nanortc_add_remote_candidate(&peer->rtc, buf);
        }
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Thread entry — unified for both localhost and ICE modes.
 * When has_ice is set, runs a TURN warmup phase before signaling.
 * ---------------------------------------------------------------- */

static void *nanortc_thread_fn(void *arg)
{
    interop_nanortc_peer_t *peer = (interop_nanortc_peer_t *)arg;

    /* Optional TURN warmup: pump event loop to complete TURN allocation
     * (Allocate → 401 → auth Allocate → success) before signaling. */
    if (peer->has_ice) {
        fprintf(stderr, "[nanortc] TURN warmup: waiting for relay candidate...\n");
        uint32_t warmup_start = interop_get_millis();
        int relay_ready = 0;
        while (atomic_load(&peer->running)) {
            nano_run_loop_step(&peer->loop);
            if (peer->rtc.sdp.has_relay_candidate) {
                fprintf(stderr, "[nanortc] TURN warmup: relay candidate ready (%u ms)\n",
                        interop_get_millis() - warmup_start);
                relay_ready = 1;
                break;
            }
            if (interop_get_millis() - warmup_start > 5000) {
                fprintf(stderr, "[nanortc] TURN warmup: timeout after 5s\n");
                break;
            }
        }
        if (!relay_ready && peer->relay_only) {
            /* relay_only mode skipped the host candidate at init time. Without
             * a relay candidate the answer SDP would have no viable ICE
             * candidates and signaling would deadlock at the test's
             * INTEROP_TURN_TIMEOUT_MS. Fail fast instead so the test errors
             * out deterministically. */
            fprintf(stderr,
                    "[nanortc] relay-only mode requires a successful TURN warmup; "
                    "aborting thread (signaling would hang with no candidates)\n");
            atomic_store(&peer->running, 0);
            return NULL;
        }
        if (!relay_ready) {
            /* Non-relay-only: the host candidate is already registered, so
             * signaling can still proceed without the relay (existing behavior). */
            fprintf(stderr, "[nanortc] continuing without relay\n");
        }
    }

    /* Signaling phase (blocking) */
    if (nanortc_do_signaling(peer) != 0) {
        fprintf(stderr, "[nanortc] Signaling failed, exiting thread\n");
        atomic_store(&peer->running, 0);
        return NULL;
    }

    /* Event loop: drive nanortc with real UDP */
    fprintf(stderr, "[nanortc] Entering event loop (port=%d)\n", peer->port);
    while (atomic_load(&peer->running)) {
        nano_run_loop_step(&peer->loop);
    }

    fprintf(stderr, "[nanortc] Thread exiting\n");
    return NULL;
}

/* ----------------------------------------------------------------
 * Auto-detect first non-loopback IPv4 address
 * ---------------------------------------------------------------- */

static void detect_local_ip(char *ip_out, size_t ip_size)
{
    ip_out[0] = '\0';
    struct ifaddrs *ifas, *ifa;
    if (getifaddrs(&ifas) == 0) {
        for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &sa->sin_addr, ip_out, (socklen_t)ip_size);
            break;
        }
        freeifaddrs(ifas);
    }
    if (ip_out[0] == '\0') {
        memcpy(ip_out, "127.0.0.1", 10);
    }
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int interop_nanortc_start(interop_nanortc_peer_t *peer, int sig_fd, uint16_t port,
                          const interop_nanortc_ice_config_t *ice_cfg)
{
    if (!peer) {
        return -1;
    }

    memset(peer, 0, sizeof(*peer));
    peer->sig_fd = sig_fd;
    peer->port = port;
    peer->has_ice = (ice_cfg != NULL);
    peer->relay_only = (ice_cfg != NULL && ice_cfg->relay_only);
    pthread_mutex_init(&peer->msg_mutex, NULL);

    /* Resolve ICE server domain names to IPs */
    if (ice_cfg && ice_cfg->ice_servers && ice_cfg->ice_server_count > 0 &&
        ice_cfg->resolve_scratch) {
        nano_resolve_ice_servers((nanortc_ice_server_t *)ice_cfg->ice_servers,
                                 ice_cfg->ice_server_count, ice_cfg->resolve_scratch,
                                 ice_cfg->resolve_scratch_size);
    }

    /* Init nanortc */
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nanortc_crypto_openssl();
#else
    cfg.crypto = nanortc_crypto_mbedtls();
#endif
    cfg.role = NANORTC_ROLE_CONTROLLED; /* answerer */
    if (ice_cfg) {
        cfg.ice_servers = ice_cfg->ice_servers;
        cfg.ice_server_count = ice_cfg->ice_server_count;
    }

    int rc = nanortc_init(&peer->rtc, &cfg);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "[nanortc] nanortc_init failed: %d\n", rc);
        return -1;
    }

    /* Determine local candidate IP */
    char local_ip[64];
    if (ice_cfg) {
        /* ICE mode: advertise real IP as the candidate */
        detect_local_ip(local_ip, sizeof(local_ip));
    } else {
        /* Localhost mode */
        memcpy(local_ip, "127.0.0.1", 10);
    }

    if (!ice_cfg || !ice_cfg->relay_only) {
        nanortc_add_local_candidate(&peer->rtc, local_ip, port);
        fprintf(stderr, "[nanortc] Local candidate: %s:%d\n", local_ip, port);
    } else {
        fprintf(stderr, "[nanortc] relay-only mode: skipping host candidate\n");
    }

    /* Init run loop (binds UDP socket on INADDR_ANY) */
    rc = nano_run_loop_init(&peer->loop, &peer->rtc, port);
    if (rc < 0) {
        fprintf(stderr, "[nanortc] Failed to bind UDP port %d\n", port);
        return -1;
    }
    nano_run_loop_set_event_cb(&peer->loop, nanortc_on_event, peer);

    /* Start thread */
    atomic_store(&peer->running, 1);
    rc = pthread_create(&peer->thread, NULL, nanortc_thread_fn, peer);
    if (rc != 0) {
        fprintf(stderr, "[nanortc] pthread_create failed: %d\n", rc);
        atomic_store(&peer->running, 0);
        nano_run_loop_destroy(&peer->loop);
        return -1;
    }

    return 0;
}

int interop_nanortc_stop(interop_nanortc_peer_t *peer)
{
    if (!peer) {
        return -1;
    }

    atomic_store(&peer->running, 0);
    nano_run_loop_stop(&peer->loop);
    pthread_join(peer->thread, NULL);

    nano_run_loop_destroy(&peer->loop);
    nanortc_destroy(&peer->rtc);
    pthread_mutex_destroy(&peer->msg_mutex);

    return 0;
}

int interop_nanortc_wait_flag(atomic_int *flag, int timeout_ms)
{
    uint32_t start = interop_get_millis();
    while (!atomic_load(flag)) {
        uint32_t elapsed = interop_get_millis() - start;
        if ((int)elapsed >= timeout_ms) {
            return -1;
        }
        interop_sleep_ms(10);
    }
    return 0;
}
