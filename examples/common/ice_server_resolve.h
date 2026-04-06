/*
 * nanortc examples — ICE server URL resolver
 *
 * Resolves domain names in nanortc_ice_server_t URLs to numeric IPs.
 * nanortc core is Sans I/O (no DNS), so applications must resolve
 * domains before passing ice_servers to nanortc_init() or
 * nanortc_set_ice_servers().
 *
 * Works on both POSIX (Linux/macOS) and ESP-IDF (lwIP) via getaddrinfo.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_ICE_SERVER_RESOLVE_H_
#define NANORTC_ICE_SERVER_RESOLVE_H_

#include "nanortc.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve domain names in ICE server URLs to numeric IPs.
 *
 * Modifies @p servers in-place: each URL with a domain name host is
 * replaced with a resolved "scheme:IP:port" string stored in @p scratch.
 * Query parameters (e.g. ?transport=tcp) are stripped.
 *
 * Usage:
 * @code
 *   nanortc_ice_server_t servers[] = {
 *       {.urls = (const char*[]){"stun:stun.l.google.com:19302"}, .url_count = 1},
 *       {.urls = (const char*[]){"turn:eu-0.turn.peerjs.com:3478"}, .url_count = 1,
 *        .username = "peerjs", .credential = "peerjsp"},
 *   };
 *   char scratch[512];
 *   nano_resolve_ice_servers(servers, 2, scratch, sizeof(scratch));
 *   cfg.ice_servers = servers;  // URLs now contain numeric IPs
 * @endcode
 *
 * @param servers  ICE server array (modified in-place).
 * @param count    Number of entries.
 * @param scratch  Buffer for resolved URL strings and pointer arrays.
 *                 Must remain valid as long as @p servers is used.
 * @param scratch_size  Size of scratch buffer in bytes.
 * @return 0 on success, -1 on failure (scratch too small).
 */
int nano_resolve_ice_servers(nanortc_ice_server_t *servers, size_t count,
                             void *scratch, size_t scratch_size);

#ifdef __cplusplus
}
#endif

#endif /* NANORTC_ICE_SERVER_RESOLVE_H_ */
