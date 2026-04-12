/*
 * sig_discovery.c — UDP auto-discovery for nanortc signaling server
 *
 * Protocol (19 bytes fixed, must match signaling_server.py):
 *   Request:  "NANORTC_DISCOVER" (16B) + version(1B) + port(2B big-endian)
 *   Response: "NANORTC_FOUND\0\0\0" (16B) + version(1B) + port(2B big-endian)
 *
 * SPDX-License-Identifier: MIT
 */

#include "sig_discovery.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DISCOVERY_PORT  19730
#define DISCOVERY_MAGIC "NANORTC_DISCOVER"
#define DISCOVERY_RESP  "NANORTC_FOUND"

int sig_discover(char *host_out, size_t host_len, uint16_t *port_out, int timeout_s)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int bcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    struct timeval tv = {.tv_sec = timeout_s, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Build and send discovery broadcast */
    uint8_t req[19];
    memcpy(req, DISCOVERY_MAGIC, 16);
    req[16] = 1; /* protocol version */
    req[17] = 0; /* port hint (unused by server) */
    req[18] = 0;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(DISCOVERY_PORT),
        .sin_addr.s_addr = INADDR_BROADCAST,
    };
    sendto(fd, req, sizeof(req), 0, (struct sockaddr *)&dest, sizeof(dest));
    fprintf(stderr, "[discovery] broadcast sent, waiting %ds...\n", timeout_s);

    /* Wait for response */
    uint8_t resp[64];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(fd, resp, sizeof(resp), 0,
                         (struct sockaddr *)&from, &fromlen);
    close(fd);

    if (n < 19 || memcmp(resp, DISCOVERY_RESP, 13) != 0) {
        fprintf(stderr, "[discovery] no signaling server found on LAN\n");
        return -1;
    }

    uint16_t port = ((uint16_t)resp[17] << 8) | resp[18];
    if (!inet_ntop(AF_INET, &from.sin_addr, host_out, (socklen_t)host_len)) {
        return -1;
    }
    *port_out = port;
    fprintf(stderr, "[discovery] found signaling at %s:%u\n", host_out, port);
    return 0;
}
