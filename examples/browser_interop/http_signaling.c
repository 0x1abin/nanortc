/*
 * nanortc browser_interop — Minimal HTTP signaling client
 *
 * Blocking HTTP/1.0 client for the signaling relay server.
 * Each request opens a new TCP connection (simple, sufficient for signaling).
 *
 * SPDX-License-Identifier: MIT
 */

#include "http_signaling.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ----------------------------------------------------------------
 * TCP helper
 * ---------------------------------------------------------------- */

static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

/*
 * Read full HTTP response. Returns body pointer (within resp_buf)
 * and sets *body_len. Returns NULL on error.
 * *status_code receives the HTTP status (e.g. 200, 204).
 */
static const char *http_read_response(int fd, char *resp_buf, size_t resp_buf_len,
                                       int *status_code, size_t *body_len)
{
    size_t total = 0;
    while (total < resp_buf_len - 1) {
        ssize_t n = recv(fd, resp_buf + total, resp_buf_len - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;

        /* Check if we have the full response (Content-Length or connection close) */
        resp_buf[total] = '\0';
        char *hdr_end = strstr(resp_buf, "\r\n\r\n");
        if (hdr_end) {
            size_t hdr_len = (size_t)(hdr_end - resp_buf) + 4;

            /* Parse Content-Length if present */
            char *cl = strstr(resp_buf, "Content-Length:");
            if (!cl) cl = strstr(resp_buf, "content-length:");
            if (cl) {
                size_t clen = (size_t)atoi(cl + 15);
                size_t needed = hdr_len + clen;
                if (total >= needed) {
                    /* Full response received */
                    break;
                }
                /* Need more data, continue reading */
            } else {
                /* No Content-Length: check status line for 204 */
                char *sp = memchr(resp_buf, ' ', hdr_len);
                if (sp && atoi(sp + 1) == 204) {
                    break; /* 204 No Content */
                }
                /* Connection-close: short timeout for remaining data */
                struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            }
        }
    }
    resp_buf[total] = '\0';

    /* Parse status code from "HTTP/1.x NNN" */
    *status_code = 0;
    char *sp = strchr(resp_buf, ' ');
    if (sp) {
        *status_code = atoi(sp + 1);
    }

    /* Find body */
    char *body = strstr(resp_buf, "\r\n\r\n");
    if (body) {
        body += 4;
        *body_len = total - (size_t)(body - resp_buf);
        return body;
    }

    *body_len = 0;
    return NULL;
}

/* ----------------------------------------------------------------
 * Minimal JSON helpers for signaling messages
 * ---------------------------------------------------------------- */

static int json_extract_string(const char *json, size_t json_len,
                                const char *key, char *out, size_t out_len)
{
    /* Search for "key" then : then optional whitespace then " */
    char needle[64];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen < 0 || (size_t)nlen >= sizeof(needle)) return -1;

    const char *start = NULL;
    const char *end = json + json_len;
    for (size_t i = 0; i + (size_t)nlen <= json_len; i++) {
        if (memcmp(json + i, needle, (size_t)nlen) == 0) {
            const char *p = json + i + nlen;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p < end && *p == ':') {
                p++;
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                if (p < end && *p == '"') {
                    start = p + 1;
                    break;
                }
            }
        }
    }
    if (!start) return -1;

    size_t wi = 0;
    const char *p = start;
    while (p < end && wi < out_len - 1) {
        if (*p == '"') break;
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case '"':  out[wi++] = '"'; break;
            case '\\': out[wi++] = '\\'; break;
            case 'r':  out[wi++] = '\r'; break;
            case 'n':  out[wi++] = '\n'; break;
            case '/':  out[wi++] = '/'; break;
            case 't':  out[wi++] = '\t'; break;
            default:   out[wi++] = '\\';
                       if (wi < out_len - 1) out[wi++] = *p;
                       break;
            }
        } else {
            out[wi++] = *p;
        }
        p++;
    }
    out[wi] = '\0';
    return (int)wi;
}

static int json_build_message(char *buf, size_t buf_len,
                               const char *type, const char *key,
                               const char *payload)
{
    size_t wi = 0;
    int n = snprintf(buf + wi, buf_len - wi, "{\"type\":\"%s\",\"%s\":\"", type, key);
    if (n < 0 || wi + (size_t)n >= buf_len) return -1;
    wi += (size_t)n;

    for (const char *p = payload; *p; p++) {
        if (wi + 3 >= buf_len) return -1;
        switch (*p) {
        case '"':  buf[wi++] = '\\'; buf[wi++] = '"'; break;
        case '\\': buf[wi++] = '\\'; buf[wi++] = '\\'; break;
        case '\r': buf[wi++] = '\\'; buf[wi++] = 'r'; break;
        case '\n': buf[wi++] = '\\'; buf[wi++] = 'n'; break;
        default:   buf[wi++] = *p; break;
        }
    }

    if (wi + 3 >= buf_len) return -1;
    buf[wi++] = '"';
    buf[wi++] = '}';
    buf[wi] = '\0';
    return (int)wi;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

int http_sig_join(http_sig_t *sig, const char *host, uint16_t port)
{
    if (!sig || !host) return -1;

    memset(sig, 0, sizeof(*sig));
    sig->peer_id = -1;

    size_t hlen = strlen(host);
    if (hlen >= sizeof(sig->host)) hlen = sizeof(sig->host) - 1;
    memcpy(sig->host, host, hlen);
    sig->host[hlen] = '\0';
    sig->port = port;

    int fd = tcp_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "[sig] Cannot connect to %s:%u\n", host, port);
        return -1;
    }

    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "POST /join HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        host, port);
    send(fd, req, (size_t)rlen, MSG_NOSIGNAL);

    char resp[1024];
    int status;
    size_t blen;
    const char *body = http_read_response(fd, resp, sizeof(resp), &status, &blen);
    close(fd);

    if (status != 200 || !body) {
        fprintf(stderr, "[sig] Join failed (status=%d)\n", status);
        return -1;
    }

    /* Parse {"id": N} — handles both numeric and quoted forms */
    const char *id_pos = strstr(body, "\"id\"");
    if (!id_pos) return -1;
    id_pos += 4;
    while (*id_pos == ' ' || *id_pos == ':' || *id_pos == '\t') id_pos++;
    sig->peer_id = atoi(id_pos); /* works for both 0 and "0" */

    fprintf(stderr, "[sig] Joined as peer %d (server %s:%u)\n",
            sig->peer_id, host, port);
    return 0;
}

int http_sig_send(http_sig_t *sig, const char *type, const char *payload,
                  const char *payload_key)
{
    if (!sig || sig->peer_id < 0) return -1;

    char json[HTTP_SIG_BUF_SIZE];
    int jlen = json_build_message(json, sizeof(json), type, payload_key, payload);
    if (jlen < 0) return -1;

    int fd = tcp_connect(sig->host, sig->port);
    if (fd < 0) return -1;

    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "POST /send?id=%d HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        sig->peer_id, sig->host, sig->port, jlen);

    send(fd, header, (size_t)hlen, MSG_NOSIGNAL);
    send(fd, json, (size_t)jlen, MSG_NOSIGNAL);

    char resp[512];
    int status;
    size_t blen;
    http_read_response(fd, resp, sizeof(resp), &status, &blen);
    close(fd);

    return (status == 200) ? 0 : -1;
}

int http_sig_recv(http_sig_t *sig, char *type_out, size_t type_len,
                  char *payload_out, size_t payload_len, int timeout_ms)
{
    if (!sig || sig->peer_id < 0) return -1;

    int timeout_sec = timeout_ms / 1000;
    if (timeout_sec < 1 && timeout_ms > 0) timeout_sec = 1;

    int fd = tcp_connect(sig->host, sig->port);
    if (fd < 0) return -1;

    /* Set socket recv timeout slightly longer than server timeout */
    struct timeval tv = {
        .tv_sec = timeout_sec + 2,
        .tv_usec = 0,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "GET /recv?id=%d&timeout=%d HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "\r\n",
        sig->peer_id, timeout_sec, sig->host, sig->port);
    send(fd, req, (size_t)rlen, MSG_NOSIGNAL);

    char resp[HTTP_SIG_BUF_SIZE];
    int status;
    size_t blen;
    const char *body = http_read_response(fd, resp, sizeof(resp), &status, &blen);
    close(fd);

    if (status == 204) return -2; /* timeout, no message */
    if (status != 200 || !body || blen == 0) return -1;

    /* Parse JSON body */
    if (json_extract_string(body, blen, "type", type_out, type_len) < 0) {
        return -1;
    }

    if (json_extract_string(body, blen, "sdp", payload_out, payload_len) >= 0) {
        return 0;
    }
    if (json_extract_string(body, blen, "candidate", payload_out, payload_len) >= 0) {
        return 0;
    }

    payload_out[0] = '\0';
    return 0;
}

void http_sig_leave(http_sig_t *sig)
{
    if (!sig || sig->peer_id < 0) return;

    int fd = tcp_connect(sig->host, sig->port);
    if (fd < 0) return;

    char req[256];
    int rlen = snprintf(req, sizeof(req),
        "POST /leave?id=%d HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        sig->peer_id, sig->host, sig->port);
    send(fd, req, (size_t)rlen, MSG_NOSIGNAL);

    char resp[256];
    int status;
    size_t blen;
    http_read_response(fd, resp, sizeof(resp), &status, &blen);
    close(fd);

    sig->peer_id = -1;
}
