/*
 * nanortc ESP32 example — UDP broadcast discovery for signaling server
 *
 * Sends broadcast packets on the LAN. The signaling server's UDP listener
 * replies with its HTTP port, and the ESP32 derives the IP from recvfrom().
 *
 * SPDX-License-Identifier: MIT
 */

#include "udp_discovery.h"

#include <stdio.h>
#include <string.h>
#include <lwip/sockets.h>

/* Protocol magic (16 bytes, no NUL terminator stored) */
#define DISCOVERY_MAGIC_REQ  "NANORTC_DISCOVER" /* exactly 16 chars */
#define DISCOVERY_MAGIC_RESP "NANORTC_FOUND"    /* 13 chars, compare prefix only */

int udp_discover_signaling(uint16_t discovery_port,
                           char *host_out, size_t host_out_len,
                           uint16_t *port_out, int max_retries)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        printf("[discovery] socket() failed: %d\n", errno);
        return -1;
    }

    /* Enable broadcast */
    int bcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    /* Set receive timeout to 2 seconds */
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(discovery_port);
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    /* Build request: magic(16) + version(1) + port(2) */
    uint8_t req[UDP_DISCOVERY_MSG_SIZE];
    memcpy(req, DISCOVERY_MAGIC_REQ, 16);
    req[16] = UDP_DISCOVERY_VERSION;
    req[17] = 0;
    req[18] = 0;

    int found = -1;
    for (int attempt = 0; attempt < max_retries; attempt++) {
        printf("[discovery] Broadcast attempt %d/%d on port %u\n",
               attempt + 1, max_retries, discovery_port);

        sendto(fd, req, sizeof(req), 0,
               (struct sockaddr *)&dest, sizeof(dest));

        uint8_t resp[64];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(fd, resp, sizeof(resp), 0,
                         (struct sockaddr *)&from, &fromlen);

        if (n >= UDP_DISCOVERY_MSG_SIZE &&
            memcmp(resp, DISCOVERY_MAGIC_RESP, 13) == 0) {
            /* Extract HTTP port from response (big-endian) */
            *port_out = ((uint16_t)resp[17] << 8) | (uint16_t)resp[18];

            /* Server IP from recvfrom source address */
            inet_ntop(AF_INET, &from.sin_addr, host_out, (socklen_t)host_out_len);

            printf("[discovery] Found server at %s:%u\n", host_out, *port_out);
            found = 0;
            break;
        }

        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("[discovery] recvfrom error: %d\n", errno);
        }
    }

    close(fd);
    return found;
}
