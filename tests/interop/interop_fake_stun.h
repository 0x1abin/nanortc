/*
 * nanortc interop tests — minimal in-process STUN server
 *
 * Listens on a loopback UDP socket, parses STUN Binding Request, and
 * responds with a Binding Response whose XOR-MAPPED-ADDRESS contains a
 * caller-configured (addr, port). Used to make srflx discovery
 * deterministic in tests without depending on a real STUN service.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INTEROP_FAKE_STUN_H_
#define INTEROP_FAKE_STUN_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sock_fd;
    uint16_t bind_port; /* bound loopback port (filled in by start) */
    uint8_t mapped_addr[16];
    uint16_t mapped_port;
    uint8_t mapped_family; /* 4 or 6 */
    pthread_t thread;
    /* Written from the caller thread (stop), read from the responder thread
     * (main loop). atomic_int avoids the plain-load data race that would be
     * UB under C11 §5.1.2.4 and may become flaky under optimization. */
    atomic_int running;
} interop_fake_stun_t;

/*
 * Bind a loopback UDP socket and start the responder thread.
 *
 *   bind_port    Pass 0 to let the OS pick; the chosen port is written
 *                back to s->bind_port.
 *   mapped_addr  16-byte address to embed in XOR-MAPPED-ADDRESS replies.
 *                IPv4 addresses occupy bytes [0..3]; the remaining bytes
 *                are ignored.
 *   mapped_port  Port number to embed in replies.
 *   mapped_family 4 (IPv4) or 6 (IPv6).
 *
 * Returns 0 on success.
 */
int interop_fake_stun_start(interop_fake_stun_t *s, uint16_t bind_port,
                            const uint8_t mapped_addr[16], uint16_t mapped_port,
                            uint8_t mapped_family);

/* Stop the responder thread and close the socket. */
void interop_fake_stun_stop(interop_fake_stun_t *s);

#ifdef __cplusplus
}
#endif

#endif /* INTEROP_FAKE_STUN_H_ */
