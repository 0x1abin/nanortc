/*
 * nanortc examples — ESP32 HTTP signaling server
 *
 * Provides a shared HTTP server for ESP32 examples:
 *   GET  /       — serve embedded index.html
 *   GET  /myip   — return client's IPv4 address (mDNS replacement)
 *   POST /offer  — SDP offer/answer exchange (delegated to application)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_WEBSERVER_H_
#define NANORTC_WEBSERVER_H_

#include <esp_http_server.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Application callback for POST /offer.
 * Receives the SDP offer string, writes the SDP answer into @p answer.
 * Return 0 on success, negative on error.
 */
typedef int (*nano_offer_handler_fn)(const char *offer, char *answer, size_t answer_size,
                                     size_t *answer_len, void *userdata);

typedef struct nano_webserver_config {
    const uint8_t *html_start;
    const uint8_t *html_end;
    nano_offer_handler_fn offer_handler;
    void *userdata;
    const char *tag; /* ESP_LOG tag */
} nano_webserver_config_t;

/**
 * Start HTTP server with /, /myip, /offer routes.
 * Returns httpd_handle_t (can add custom routes), or NULL on error.
 */
httpd_handle_t nano_webserver_start(const nano_webserver_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_WEBSERVER_H_ */
