/*
 * nanortc examples — ICE server URL resolver
 *
 * SPDX-License-Identifier: MIT
 */

#include "ice_server_resolve.h"

#include <stdio.h>
#include <string.h>

#if defined(IDF_VER) || defined(ESP_PLATFORM)
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

/* Check if a string is already a numeric IPv4 address */
static int is_numeric_ip(const char *host)
{
    for (const char *p = host; *p; p++) {
        if (*p != '.' && (*p < '0' || *p > '9')) {
            return 0;
        }
    }
    return 1;
}

/* Resolve hostname to IPv4 string. Returns 0 on success. */
static int resolve_host(const char *host, char *ip_out, size_t ip_size)
{
    if (is_numeric_ip(host)) {
        size_t len = strlen(host);
        if (len >= ip_size) {
            return -1;
        }
        memcpy(ip_out, host, len + 1);
        return 0;
    }

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        return -1;
    }

    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;

#if defined(IDF_VER) || defined(ESP_PLATFORM)
    inet_ntoa_r(sa->sin_addr, ip_out, ip_size);
#else
    inet_ntop(AF_INET, &sa->sin_addr, ip_out, (socklen_t)ip_size);
#endif

    freeaddrinfo(res);
    return 0;
}

/*
 * Scratch buffer layout (allocated sequentially):
 *   For each server with N urls:
 *     const char *ptrs[N]   — pointer array (new urls)
 *     char url_0[...]       — "scheme:resolved_ip:port\0"
 *     char url_1[...]       — ...
 */

int nano_resolve_ice_servers(nanortc_ice_server_t *servers, size_t count,
                             void *scratch, size_t scratch_size)
{
    if (!servers || !scratch) {
        return -1;
    }

    uint8_t *buf = (uint8_t *)scratch;
    size_t pos = 0;

    for (size_t i = 0; i < count; i++) {
        nanortc_ice_server_t *s = &servers[i];
        if (!s->urls || s->url_count == 0) {
            continue;
        }

        size_t n = s->url_count;

        /* Allocate pointer array in scratch */
        size_t ptrs_size = n * sizeof(const char *);
        ptrs_size = (ptrs_size + 7) & ~7u; /* align to 8 */
        if (pos + ptrs_size > scratch_size) {
            return -1;
        }
        const char **new_urls = (const char **)(buf + pos);
        pos += ptrs_size;

        for (size_t u = 0; u < n; u++) {
            const char *url = s->urls[u];
            if (!url) {
                new_urls[u] = NULL;
                continue;
            }

            /* Parse scheme */
            const char *p = url;
            const char *scheme;
            if (p[0] == 's' && p[1] == 't' && p[2] == 'u' && p[3] == 'n' && p[4] == ':') {
                scheme = "stun:";
                p += 5;
            } else if (p[0] == 't' && p[1] == 'u' && p[2] == 'r' && p[3] == 'n' && p[4] == ':') {
                scheme = "turn:";
                p += 5;
            } else {
                new_urls[u] = url; /* pass through unknown schemes */
                continue;
            }

            /* Find end (stop at '?' or '\0') */
            const char *end = p;
            while (*end && *end != '?') {
                end++;
            }

            /* Find last ':' for port */
            const char *colon = NULL;
            for (const char *c = p; c < end; c++) {
                if (*c == ':') {
                    colon = c;
                }
            }

            char host[64];
            uint16_t port = 3478;
            if (colon && colon > p) {
                size_t hlen = (size_t)(colon - p);
                if (hlen >= sizeof(host)) {
                    hlen = sizeof(host) - 1;
                }
                memcpy(host, p, hlen);
                host[hlen] = '\0';
                uint32_t pv = 0;
                const char *pp = colon + 1;
                while (pp < end && *pp >= '0' && *pp <= '9') {
                    pv = pv * 10 + (uint32_t)(*pp - '0');
                    pp++;
                }
                port = (uint16_t)pv;
            } else {
                size_t hlen = (size_t)(end - p);
                if (hlen >= sizeof(host)) {
                    hlen = sizeof(host) - 1;
                }
                memcpy(host, p, hlen);
                host[hlen] = '\0';
            }

            /* Resolve host */
            char ip[46];
            if (resolve_host(host, ip, sizeof(ip)) != 0) {
                new_urls[u] = url; /* keep original if resolve fails */
                continue;
            }

            /* Write "scheme:ip:port" into scratch */
            char *dst = (char *)(buf + pos);
            size_t remaining = scratch_size - pos;
            int written = snprintf(dst, remaining, "%s%s:%u", scheme, ip, port);
            if (written < 0 || (size_t)written >= remaining) {
                return -1;
            }
            new_urls[u] = dst;
            pos += (size_t)written + 1; /* include NUL */
        }

        /* Point server's urls to the new resolved array */
        s->urls = new_urls;
    }

    return 0;
}
