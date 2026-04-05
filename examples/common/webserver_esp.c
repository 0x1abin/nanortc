/*
 * nanortc examples — ESP32 HTTP signaling server implementation
 *
 * Pure HTTP layer — all nanortc logic lives in the application.
 *
 * SPDX-License-Identifier: MIT
 */

#include "webserver.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include <lwip/sockets.h>

/*
 * Configuration limits (tuned for ESP32 RAM constraints).
 * Adjust as needed for other platforms.
 */
#define MAX_OFFER_SIZE 10240
#define MAX_ANSWER_SIZE 2048

/* Internal copy of config (only one HTTP server per ESP32) */
static nano_webserver_config_t s_cfg;

/* ----------------------------------------------------------------
 * HTTP handlers
 * ---------------------------------------------------------------- */

/* GET / — serve embedded index.html */
static esp_err_t http_get_root(httpd_req_t *req)
{
    nano_webserver_config_t *cfg = (nano_webserver_config_t *)req->user_ctx;
    size_t html_len = (size_t)(cfg->html_end - cfg->html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)cfg->html_start, (ssize_t)html_len);
    return ESP_OK;
}

/* GET /myip — return client's IPv4 address (mDNS replacement).
 * lwIP uses IPv6 sockets, so IPv4 clients appear as ::FFFF:x.x.x.x. */
static esp_err_t http_get_myip(httpd_req_t *req)
{
    (void)req->user_ctx;
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr6;
    socklen_t addr_len = sizeof(addr6);
    if (getpeername(sockfd, (struct sockaddr *)&addr6, &addr_len) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "getpeername");
        return ESP_FAIL;
    }
    char ip_str[64];
    if (addr6.sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&addr6.sin6_addr)) {
        struct in_addr v4;
        memcpy(&v4, &addr6.sin6_addr.s6_addr[12], 4);
        inet_ntop(AF_INET, &v4, ip_str, sizeof(ip_str));
    } else if (addr6.sin6_family == AF_INET) {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&addr6;
        inet_ntop(AF_INET, &a4->sin_addr, ip_str, sizeof(ip_str));
    } else {
        inet_ntop(AF_INET6, &addr6.sin6_addr, ip_str, sizeof(ip_str));
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, ip_str, -1);
    return ESP_OK;
}

/* POST /offer — delegate to application callback */
static esp_err_t http_post_offer(httpd_req_t *req)
{
    nano_webserver_config_t *cfg = (nano_webserver_config_t *)req->user_ctx;
    const char *tag = cfg->tag ? cfg->tag : "webserver";

    int content_len = req->content_len;
    if (content_len <= 0 || content_len > MAX_OFFER_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad offer size");
        return ESP_FAIL;
    }

    char *offer = malloc((size_t)content_len + 1);
    if (!offer) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, offer, (size_t)content_len);
    if (received <= 0) {
        free(offer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed");
        return ESP_FAIL;
    }
    offer[received] = '\0';

    ESP_LOGI(tag, "Got SDP offer (%d bytes)", received);

    char *answer = malloc(MAX_ANSWER_SIZE);
    if (!answer) {
        free(offer);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t answer_len = 0;
    int rc = cfg->offer_handler(offer, answer, MAX_ANSWER_SIZE, &answer_len, cfg->userdata);
    free(offer);

    if (rc != 0) {
        ESP_LOGE(tag, "offer_handler failed: %d", rc);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Offer failed");
        free(answer);
        return ESP_FAIL;
    }

    ESP_LOGI(tag, "SDP answer generated (%u bytes)", (unsigned)answer_len);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, answer, (ssize_t)answer_len);
    free(answer);
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * nano_webserver_start
 * ---------------------------------------------------------------- */
httpd_handle_t nano_webserver_start(const nano_webserver_config_t *cfg)
{
    const char *tag = cfg->tag ? cfg->tag : "webserver";

    s_cfg = *cfg;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 6;
    config.max_open_sockets = 1;
    config.stack_size = 6144;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(tag, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_get_root,
        .user_ctx = &s_cfg,
    };
    httpd_uri_t uri_myip = {
        .uri = "/myip",
        .method = HTTP_GET,
        .handler = http_get_myip,
        .user_ctx = &s_cfg,
    };
    httpd_uri_t uri_offer = {
        .uri = "/offer",
        .method = HTTP_POST,
        .handler = http_post_offer,
        .user_ctx = &s_cfg,
    };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_myip);
    httpd_register_uri_handler(server, &uri_offer);

    ESP_LOGI(tag, "HTTP server started on port %d", config.server_port);
    return server;
}
