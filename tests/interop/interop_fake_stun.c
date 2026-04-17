/*
 * nanortc interop tests — minimal in-process STUN server.
 *
 * The responder accepts a Binding Request on a loopback socket, copies the
 * 96-bit transaction ID from the request, and emits a Binding Response with
 * a single XOR-MAPPED-ADDRESS attribute populated from the caller-configured
 * mapped address. No MESSAGE-INTEGRITY (RFC 5389 §10.1.2: STUN servers may
 * omit it for unauthenticated bindings — sufficient for srflx discovery in
 * the nanortc client path).
 *
 * SPDX-License-Identifier: MIT
 */

#include "interop_fake_stun.h"
#include "nano_stun.h"
#include "nanortc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static void write_u16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void write_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

/* Build a Binding Response with one XOR-MAPPED-ADDRESS attribute.
 *
 * Wire layout (RFC 5389 §6, §15.2):
 *   STUN header    [20 bytes]    type | length | magic cookie | txid
 *   Attribute hdr  [4 bytes]     type=0x0020 | length
 *   Attribute val  [8 or 20]     reserved | family | xport | xaddr
 *
 * The header `length` field is the size of all attributes (including their
 * 4-byte headers), NOT including the 20-byte STUN header.
 */
static size_t build_binding_response(const uint8_t txid[12], const uint8_t mapped_addr[16],
                                     uint16_t mapped_port, uint8_t mapped_family, uint8_t *buf,
                                     size_t buf_size)
{
    size_t addr_value_len = (mapped_family == 6) ? 16u : 4u;
    /* Attribute value: 4 (reserved+family+xport) + addr bytes */
    size_t attr_value_len = 4u + addr_value_len;
    /* Length field counts attribute(s) including 4-byte attr header */
    size_t hdr_length_field = 4u + attr_value_len;
    size_t total = 20u + hdr_length_field;
    if (buf_size < total) {
        return 0;
    }

    write_u16be(buf, STUN_BINDING_RESPONSE);
    write_u16be(buf + 2, (uint16_t)hdr_length_field);
    write_u32be(buf + 4, STUN_MAGIC_COOKIE);
    memcpy(buf + 8, txid, 12);

    size_t pos = 20;
    write_u16be(buf + pos, 0x0020);
    write_u16be(buf + pos + 2, (uint16_t)attr_value_len);
    buf[pos + 4] = 0;
    buf[pos + 5] = (mapped_family == 6) ? 0x02 : 0x01;
    write_u16be(buf + pos + 6, (uint16_t)(mapped_port ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16)));
    if (mapped_family == 6) {
        for (size_t i = 0; i < 16; i++) {
            uint8_t key = (i < 4) ? (uint8_t)(STUN_MAGIC_COOKIE >> (24 - 8 * i)) : txid[i - 4];
            buf[pos + 8 + i] = mapped_addr[i] ^ key;
        }
    } else {
        uint32_t a = ((uint32_t)mapped_addr[0] << 24) | ((uint32_t)mapped_addr[1] << 16) |
                     ((uint32_t)mapped_addr[2] << 8) | (uint32_t)mapped_addr[3];
        write_u32be(buf + pos + 8, a ^ STUN_MAGIC_COOKIE);
    }
    return total;
}

static void *fake_stun_thread(void *arg)
{
    interop_fake_stun_t *s = (interop_fake_stun_t *)arg;
    uint8_t buf[512];

    while (atomic_load_explicit(&s->running, memory_order_acquire)) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(s->sock_fd, &rset);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000}; /* 100 ms */

        int n = select(s->sock_fd + 1, &rset, NULL, NULL, &tv);
        if (n < 0) {
            /* EINTR: a signal caught us mid-wait — loop back and re-check
             * the running flag so shutdown stays responsive. Any other
             * error: bail rather than spin. */
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            continue; /* timeout; recheck running flag */
        }

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t r = recvfrom(s->sock_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (r < 20) {
            continue;
        }

        /* Verify Binding Request type + magic cookie before responding;
         * ignore anything else (consent checks land here too in some
         * topologies but they're handled by the ICE socket, not us). */
        if (((uint16_t)buf[0] << 8 | buf[1]) != STUN_BINDING_REQUEST) {
            continue;
        }
        uint32_t cookie =
            ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
        if (cookie != STUN_MAGIC_COOKIE) {
            continue;
        }

        uint8_t resp[64];
        size_t resp_len = build_binding_response(buf + 8, s->mapped_addr, s->mapped_port,
                                                 s->mapped_family, resp, sizeof(resp));
        if (resp_len == 0) {
            continue;
        }
        sendto(s->sock_fd, resp, resp_len, 0, (struct sockaddr *)&from, fromlen);
    }
    return NULL;
}

int interop_fake_stun_start(interop_fake_stun_t *s, uint16_t bind_port,
                            const uint8_t mapped_addr[16], uint16_t mapped_port,
                            uint8_t mapped_family)
{
    if (!s || !mapped_addr || (mapped_family != 4 && mapped_family != 6)) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->mapped_family = mapped_family;
    s->mapped_port = mapped_port;
    memcpy(s->mapped_addr, mapped_addr, 16);

    s->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->sock_fd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(s->sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(bind_port);
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    if (bind(s->sock_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(s->sock_fd);
        s->sock_fd = -1;
        return -1;
    }

    socklen_t addrlen = sizeof(local);
    if (getsockname(s->sock_fd, (struct sockaddr *)&local, &addrlen) == 0) {
        s->bind_port = ntohs(local.sin_port);
    } else {
        s->bind_port = bind_port;
    }

    atomic_store_explicit(&s->running, 1, memory_order_release);
    if (pthread_create(&s->thread, NULL, fake_stun_thread, s) != 0) {
        atomic_store_explicit(&s->running, 0, memory_order_release);
        close(s->sock_fd);
        s->sock_fd = -1;
        return -1;
    }
    return 0;
}

void interop_fake_stun_stop(interop_fake_stun_t *s)
{
    if (!s || !atomic_load_explicit(&s->running, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&s->running, 0, memory_order_release);
    pthread_join(s->thread, NULL);
    if (s->sock_fd >= 0) {
        close(s->sock_fd);
        s->sock_fd = -1;
    }
}
