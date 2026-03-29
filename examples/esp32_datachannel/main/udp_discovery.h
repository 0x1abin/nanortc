/*
 * nanortc ESP32 example — UDP broadcast discovery for signaling server
 *
 * Protocol (19 bytes fixed):
 *   Request:  "NANORTC_DISCOVER" (16B) + version(1B) + port(2B big-endian)
 *   Response: "NANORTC_FOUND\0\0\0" (16B) + version(1B) + port(2B big-endian)
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef UDP_DISCOVERY_H_
#define UDP_DISCOVERY_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UDP_DISCOVERY_PORT     19730
#define UDP_DISCOVERY_VERSION  1
#define UDP_DISCOVERY_MSG_SIZE 19

/**
 * Discover the signaling server via UDP broadcast.
 *
 * @param discovery_port  UDP port to broadcast on (default: UDP_DISCOVERY_PORT)
 * @param host_out        Buffer to receive server IP string (at least 16 bytes)
 * @param host_out_len    Size of host_out buffer
 * @param port_out        Receives the HTTP port of the signaling server
 * @param max_retries     Number of broadcast attempts (each with 2s timeout)
 * @return 0 on success, -1 on failure (no server found)
 */
int udp_discover_signaling(uint16_t discovery_port,
                           char *host_out, size_t host_out_len,
                           uint16_t *port_out, int max_retries);

#ifdef __cplusplus
}
#endif

#endif /* UDP_DISCOVERY_H_ */
