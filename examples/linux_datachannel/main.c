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
#include "nanortc_crypto.h"
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

static void on_event(nanortc_t *rtc, const nanortc_event_t *evt, void *userdata)
{
    (void)userdata;

    switch (evt->type) {
    case NANORTC_EV_ICE_STATE_CHANGE:
        if (evt->ice_state == NANORTC_ICE_STATE_CONNECTED) {
            fprintf(stderr, "[event] ICE connected\n");
        }
        break;

    case NANORTC_EV_CONNECTED:
        fprintf(stderr, "[event] Connected\n");
        break;

    case NANORTC_EV_DATACHANNEL_OPEN:
        fprintf(stderr, "[event] DataChannel open (id=%d)\n", evt->datachannel_open.id);
        break;

    case NANORTC_EV_DATACHANNEL_DATA: {
        nanortc_datachannel_t ch;
        if (nanortc_get_datachannel(rtc, evt->datachannel_data.id, &ch) != NANORTC_OK)
            break;
        if (evt->datachannel_data.binary) {
            fprintf(stderr, "[event] DC data (%zu bytes), echoing back\n", evt->datachannel_data.len);
            nanortc_datachannel_send(&ch, evt->datachannel_data.data, evt->datachannel_data.len);
        } else {
            fprintf(stderr, "[event] DC string: %.*s\n", (int)evt->datachannel_data.len,
                    (char *)evt->datachannel_data.data);
            nanortc_datachannel_send_string(&ch, (const char *)evt->datachannel_data.data);
        }
        break;
    }

    case NANORTC_EV_DATACHANNEL_CLOSE:
        fprintf(stderr, "[event] DataChannel closed\n");
        break;

    case NANORTC_EV_DISCONNECTED:
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
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

#if defined(NANORTC_CRYPTO_OPENSSL)
    cfg.crypto = nanortc_crypto_openssl();
#else
    cfg.crypto = nanortc_crypto_mbedtls();
#endif
    cfg.role = NANORTC_ROLE_CONTROLLED;

    int rc = nanortc_init(&rtc, &cfg);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_init failed: %d\n", rc);
        return 1;
    }

    /* 2. Init event loop (bind UDP) */
    rc = nano_run_loop_init(&loop, &rtc, NULL, port);
    if (rc < 0) {
        fprintf(stderr, "Failed to bind UDP port %d\n", port);
        return 1;
    }
    nano_run_loop_set_event_cb(&loop, on_event, NULL);

    fprintf(stderr, "nanortc DataChannel echo (port=%d, DC=%d)\n", port,
            NANORTC_FEATURE_DATACHANNEL);

    /* 3. Signaling: exchange SDP via stdin/stdout */
    nano_signaling_t sig;
    nano_signaling_init(&sig, NANORTC_SIG_STDIN);

    char offer[4096];
    rc = nano_signaling_recv_offer(&sig, offer, sizeof(offer));
    if (rc < 0) {
        fprintf(stderr, "Failed to read SDP offer\n");
        return 1;
    }

    char answer[4096];
    rc = nanortc_accept_offer(&rtc, offer, answer, sizeof(answer), NULL);
    if (rc != NANORTC_OK) {
        fprintf(stderr, "nanortc_accept_offer failed: %d (%s)\n", rc, nanortc_err_name(rc));
        return 1;
    }
    nano_signaling_send_answer(&sig, answer);

    nano_signaling_destroy(&sig);

    /* 4. Run event loop */
    fprintf(stderr, "Entering event loop... (Ctrl+C to quit)\n");
    nano_run_loop_run(&loop);

    /* 5. Cleanup */
    nano_run_loop_destroy(&loop);
    nanortc_destroy(&rtc);

    fprintf(stderr, "Done.\n");
    return 0;
}
