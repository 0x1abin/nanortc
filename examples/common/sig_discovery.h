/*
 * sig_discovery.h — UDP auto-discovery for nanortc signaling server
 *
 * Sends a broadcast on the LAN and waits for a signaling server to reply.
 * Protocol matches signaling_server.py's discovery_listener (port 19730).
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SIG_DISCOVERY_H_
#define SIG_DISCOVERY_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Discover a signaling server on the LAN via UDP broadcast.
 *
 * Sends NANORTC_DISCOVER to 255.255.255.255:19730 and waits for
 * a NANORTC_FOUND reply containing the server's HTTP port.
 *
 * @param host_out   Buffer to receive the server IP (dotted-decimal).
 * @param host_len   Size of @p host_out.
 * @param port_out   Receives the HTTP port of the signaling server.
 * @param timeout_s  Seconds to wait for a reply (typically 3).
 * @return 0 on success, -1 if no server found.
 */
int sig_discover(char *host_out, size_t host_len, uint16_t *port_out, int timeout_s);

#ifdef __cplusplus
}
#endif

#endif /* SIG_DISCOVERY_H_ */
