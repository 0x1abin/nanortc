/*
 * nanortc example — Linux DataChannel echo
 *
 * Minimal example: accept a WebRTC connection from a browser,
 * echo any DataChannel message back to the sender.
 *
 * Usage:
 *   ./linux_datachannel [-p port]
 *
 * Signaling: paste browser's SDP offer into stdin, copy answer back.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_crypto.h"
#include "run_loop.h"
#include "signaling.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static nano_run_loop_t loop;

static void on_signal(int sig)
{
    (void)sig;
    nano_run_loop_stop(&loop);
}

static void on_event(nano_rtc_t *rtc, const nano_event_t *evt, void *userdata)
{
    (void)userdata;

    switch (evt->type) {
    case NANO_EVENT_ICE_CONNECTED:
        fprintf(stderr, "[event] ICE connected\n");
        break;

    case NANO_EVENT_DTLS_CONNECTED:
        fprintf(stderr, "[event] DTLS connected\n");
        break;

    case NANO_EVENT_SCTP_CONNECTED:
        fprintf(stderr, "[event] SCTP connected\n");
        break;

    case NANO_EVENT_DATACHANNEL_OPEN:
        fprintf(stderr, "[event] DataChannel open (stream=%d)\n", evt->stream_id);
        break;

    case NANO_EVENT_DATACHANNEL_DATA:
        fprintf(stderr, "[event] DC data (%zu bytes), echoing back\n", evt->len);
        nano_send_datachannel(rtc, evt->stream_id, evt->data, evt->len);
        break;

    case NANO_EVENT_DATACHANNEL_STRING:
        fprintf(stderr, "[event] DC string: %.*s\n", (int)evt->len, (char *)evt->data);
        nano_send_datachannel_string(rtc, evt->stream_id, (const char *)evt->data);
        break;

    case NANO_EVENT_DATACHANNEL_CLOSE:
        fprintf(stderr, "[event] DataChannel closed\n");
        break;

    case NANO_EVENT_DISCONNECTED:
        fprintf(stderr, "[event] Disconnected\n");
        nano_run_loop_stop(&loop);
        break;

    default:
        break;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-p port]\n", prog);
    fprintf(stderr, "  -p port   UDP port to bind (default: 9999)\n");
}

int main(int argc, char *argv[])
{
    uint16_t port = 9999;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    /* Signal handling */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* 1. Init nanortc */
    nano_rtc_t rtc;
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nano_crypto_openssl();
#else
    cfg.crypto = nano_crypto_mbedtls();
#endif
    cfg.role = NANO_ROLE_CONTROLLED;

    int rc = nano_rtc_init(&rtc, &cfg);
    if (rc != NANO_OK) {
        fprintf(stderr, "nano_rtc_init failed: %d\n", rc);
        return 1;
    }

    /* 2. Init event loop (bind UDP) */
    rc = nano_run_loop_init(&loop, &rtc, NULL, port);
    if (rc < 0) {
        fprintf(stderr, "Failed to bind UDP port %d\n", port);
        return 1;
    }
    nano_run_loop_set_event_cb(&loop, on_event, NULL);

    fprintf(stderr, "nanortc DataChannel echo (port=%d, profile=%d)\n", port, NANORTC_PROFILE);

    /* 3. Signaling: exchange SDP via stdin/stdout */
    nano_signaling_t sig;
    nano_signaling_init(&sig, NANO_SIG_STDIN);

    char offer[4096];
    rc = nano_signaling_recv_offer(&sig, offer, sizeof(offer));
    if (rc < 0) {
        fprintf(stderr, "Failed to read SDP offer\n");
        return 1;
    }

    char answer[4096];
    rc = nano_accept_offer(&rtc, offer, answer, sizeof(answer));
    if (rc == NANO_ERR_NOT_IMPLEMENTED) {
        fprintf(stderr, "nano_accept_offer not implemented yet (stub phase)\n");
        fprintf(stderr, "This example will work once SDP/ICE/DTLS modules are implemented.\n");
    } else if (rc != NANO_OK) {
        fprintf(stderr, "nano_accept_offer failed: %d\n", rc);
        return 1;
    } else {
        nano_signaling_send_answer(&sig, answer);
    }

    nano_signaling_destroy(&sig);

    /* 4. Run event loop */
    fprintf(stderr, "Entering event loop... (Ctrl+C to quit)\n");
    nano_run_loop_run(&loop);

    /* 5. Cleanup */
    nano_run_loop_destroy(&loop);
    nano_rtc_destroy(&rtc);

    fprintf(stderr, "Done.\n");
    return 0;
}
