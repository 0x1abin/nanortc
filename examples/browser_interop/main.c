/*
 * nanortc browser_interop — DataChannel echo via HTTP signaling
 *
 * Tests nanortc ↔ browser WebRTC interoperability.
 * Supports both offerer (CONTROLLING) and answerer (CONTROLLED) roles.
 *
 * Usage:
 *   ./browser_interop [-p udp_port] [-s host:port] [--offer | --answer]
 *
 * Default: --answer mode, signaling at localhost:8765, UDP port 9999.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nanortc_crypto.h"
#include "run_loop.h"
#include "http_signaling.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>

static nano_run_loop_t loop;
static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
    nano_run_loop_stop(&loop);
}

static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EVENT_ICE_CONNECTED:
        fprintf(stderr, "[event] ICE connected\n");
        break;

    case NANORTC_EVENT_DTLS_CONNECTED:
        fprintf(stderr, "[event] DTLS connected\n");
        break;

    case NANORTC_EVENT_SCTP_CONNECTED:
        fprintf(stderr, "[event] SCTP connected\n");
        break;

    case NANORTC_EVENT_DATACHANNEL_OPEN:
        fprintf(stderr, "[event] DataChannel open (stream=%d)\n", evt->stream_id);
        break;

    case NANORTC_EVENT_DATACHANNEL_DATA:
        fprintf(stderr, "[event] DC data (%zu bytes), echoing back\n", evt->len);
        nanortc_send_datachannel(rtc, evt->stream_id, evt->data, evt->len);
        break;

    case NANORTC_EVENT_DATACHANNEL_STRING:
        fprintf(stderr, "[event] DC string: %.*s\n", (int)evt->len, (char *)evt->data);
        nanortc_send_datachannel_string(rtc, evt->stream_id, (const char *)evt->data);
        break;

    case NANORTC_EVENT_DATACHANNEL_CLOSE:
        fprintf(stderr, "[event] DataChannel closed\n");
        break;

    case NANORTC_EVENT_DISCONNECTED:
        fprintf(stderr, "[event] Disconnected\n");
        nano_run_loop_stop(&loop);
        break;

    default:
        break;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  -p PORT        UDP port (default: 9999)\n");
    fprintf(stderr, "  -b IP          Bind/candidate IP (default: auto-detect)\n");
    fprintf(stderr, "  -s HOST:PORT   Signaling server (default: localhost:8765)\n");
    fprintf(stderr, "  --offer        Act as offerer (CONTROLLING)\n");
    fprintf(stderr, "  --answer       Act as answerer (CONTROLLED, default)\n");
}

/* ----------------------------------------------------------------
 * Signaling: answer mode (CONTROLLED)
 *   Wait for offer → generate answer → send answer
 * ---------------------------------------------------------------- */
static int do_answer_signaling(http_sig_t *sig, nanortc_t *rtc)
{
    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    fprintf(stderr, "Waiting for SDP offer...\n");
    while (!g_quit) {
        int rc = http_sig_recv(sig, type, sizeof(type),
                               payload, sizeof(payload), 2000);
        if (rc == -2) continue; /* timeout, retry */
        if (rc < 0) {
            if (g_quit) return -1; /* interrupted by signal */
            fprintf(stderr, "Signaling error waiting for offer\n");
            return -1;
        }
        if (strcmp(type, "offer") == 0) break;
        fprintf(stderr, "[sig] Ignoring '%s' (waiting for offer)\n", type);
    }

    fprintf(stderr, "[sig] Got SDP offer (%zu bytes)\n", strlen(payload));

    char answer[HTTP_SIG_BUF_SIZE];
    size_t answer_len = 0;
    int rc = nanortc_accept_offer(rtc, payload, answer, sizeof(answer), &answer_len);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_accept_offer failed: %d\n", rc);
        return rc;
    }
    fprintf(stderr, "[sig] Generated SDP answer (%zu bytes)\n", answer_len);

    rc = http_sig_send(sig, "answer", answer, "sdp");
    if (rc < 0) {
        fprintf(stderr, "Failed to send SDP answer\n");
        return -1;
    }
    fprintf(stderr, "[sig] Sent SDP answer\n");
    return 0;
}

/* ----------------------------------------------------------------
 * Signaling: offer mode (CONTROLLING)
 *   Generate offer → send offer → wait for answer
 * ---------------------------------------------------------------- */
static int do_offer_signaling(http_sig_t *sig, nanortc_t *rtc)
{
    char offer[HTTP_SIG_BUF_SIZE];
    int rc = nanortc_create_offer(rtc, offer, sizeof(offer), NULL);
    if (rc < 0) {
        fprintf(stderr, "nanortc_create_offer failed: %d\n", rc);
        return rc;
    }

    rc = http_sig_send(sig, "offer", offer, "sdp");
    if (rc < 0) {
        fprintf(stderr, "Failed to send SDP offer\n");
        return -1;
    }
    fprintf(stderr, "[sig] Sent SDP offer\n");

    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    fprintf(stderr, "Waiting for SDP answer...\n");
    while (!g_quit) {
        rc = http_sig_recv(sig, type, sizeof(type),
                           payload, sizeof(payload), 2000);
        if (rc == -2) continue;
        if (rc < 0) {
            fprintf(stderr, "Signaling error waiting for answer\n");
            return -1;
        }
        if (strcmp(type, "answer") == 0) break;
        /* Buffer any early ICE candidates */
        if (strcmp(type, "candidate") == 0 && payload[0] != '\0') {
            fprintf(stderr, "[sig] Early ICE candidate: %.60s...\n", payload);
            nanortc_add_remote_candidate(rtc, payload);
        }
    }

    fprintf(stderr, "[sig] Got SDP answer (%zu bytes)\n", strlen(payload));
    rc = nanortc_accept_answer(rtc, payload);
    if (rc < 0) {
        fprintf(stderr, "nanortc_accept_answer failed: %d\n", rc);
        return rc;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Trickle ICE: poll signaling for remote candidates periodically
 * ---------------------------------------------------------------- */
static uint32_t last_poll_ms;

static void poll_trickle_ice(http_sig_t *sig, nanortc_t *rtc)
{
    uint32_t now = nano_get_millis();
    if (now - last_poll_ms < 500) return; /* throttle to every 500ms */
    last_poll_ms = now;

    char type[32];
    char payload[HTTP_SIG_BUF_SIZE];

    int rc = http_sig_recv(sig, type, sizeof(type),
                           payload, sizeof(payload), 0);
    if (rc == 0) {
        if (strcmp(type, "candidate") == 0 && payload[0] != '\0') {
            fprintf(stderr, "[sig] Trickle ICE: %.60s...\n", payload);
            nanortc_add_remote_candidate(rtc, payload);
        } else if (strcmp(type, "candidate") == 0) {
            fprintf(stderr, "[sig] End-of-candidates\n");
        }
    }
}

int main(int argc, char *argv[])
{
    uint16_t port = 9999;
    char bind_ip[64] = "";
    char sig_host[256] = "localhost";
    uint16_t sig_port = 8765;
    int offer_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            size_t len = strlen(argv[++i]);
            if (len >= sizeof(bind_ip)) len = sizeof(bind_ip) - 1;
            memcpy(bind_ip, argv[i], len);
            bind_ip[len] = '\0';
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            i++;
            char *colon = strrchr(argv[i], ':');
            if (colon) {
                size_t hlen = (size_t)(colon - argv[i]);
                if (hlen >= sizeof(sig_host)) hlen = sizeof(sig_host) - 1;
                memcpy(sig_host, argv[i], hlen);
                sig_host[hlen] = '\0';
                sig_port = (uint16_t)atoi(colon + 1);
            } else {
                size_t hlen = strlen(argv[i]);
                if (hlen >= sizeof(sig_host)) hlen = sizeof(sig_host) - 1;
                memcpy(sig_host, argv[i], hlen);
                sig_host[hlen] = '\0';
            }
        } else if (strcmp(argv[i], "--offer") == 0) {
            offer_mode = 1;
        } else if (strcmp(argv[i], "--answer") == 0) {
            offer_mode = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1. Join signaling server */
    http_sig_t sig;
    int rc = http_sig_join(&sig, sig_host, sig_port);
    if (rc < 0) {
        fprintf(stderr, "Failed to join signaling server %s:%u\n", sig_host, sig_port);
        return 1;
    }

    /* 2. Init nanortc */
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nanortc_crypto_openssl();
#else
    cfg.crypto = nanortc_crypto_mbedtls();
#endif
    cfg.role = offer_mode ? NANORTC_ROLE_CONTROLLING : NANORTC_ROLE_CONTROLLED;

    rc = nanortc_init(&rtc, &cfg);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_init failed: %d\n", rc);
        http_sig_leave(&sig);
        return 1;
    }

    /* 3. Auto-detect IP if not specified */
    if (bind_ip[0] == '\0') {
        struct ifaddrs *ifas, *ifa;
        if (getifaddrs(&ifas) == 0) {
            for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                if (ifa->ifa_flags & IFF_LOOPBACK)
                    continue;
                struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                inet_ntop(AF_INET, &sa->sin_addr, bind_ip, sizeof(bind_ip));
                break;
            }
            freeifaddrs(ifas);
        }
        if (bind_ip[0] == '\0') {
            memcpy(bind_ip, "127.0.0.1", 10);
        }
    }

    /* 4. Bind UDP on 0.0.0.0, register real IP as candidate */
    rc = nano_run_loop_init(&loop, &rtc, NULL, port);
    if (rc < 0) {
        fprintf(stderr, "Failed to bind UDP port %d\n", port);
        http_sig_leave(&sig);
        nanortc_destroy(&rtc);
        return 1;
    }
    /* Override 0.0.0.0 candidate with actual IP for SDP */
    nanortc_add_local_candidate(&rtc, bind_ip, port);
    nano_run_loop_set_event_cb(&loop, on_event, NULL);

    fprintf(stderr, "nanortc browser_interop (mode=%s, udp=%s:%d, sig=%s:%u)\n",
            offer_mode ? "offer" : "answer", bind_ip, port, sig_host, sig_port);

    /* 5. SDP exchange */
    if (offer_mode) {
        rc = do_offer_signaling(&sig, &rtc);
    } else {
        rc = do_answer_signaling(&sig, &rtc);
    }
    if (rc != 0) {
        goto cleanup;
    }

    /* 6. Event loop with trickle ICE polling */
    fprintf(stderr, "Entering event loop...\n");
    loop.running = 1;
    while (loop.running) {
        nano_run_loop_step(&loop);
        poll_trickle_ice(&sig, &rtc);
    }

cleanup:
    http_sig_leave(&sig);
    nano_run_loop_destroy(&loop);
    nanortc_destroy(&rtc);

    fprintf(stderr, "Done.\n");
    return 0;
}
